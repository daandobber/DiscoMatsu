#include "cd_ripper.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_storage.h"

static const char *TAG = "cd_ripper";

#define RIP_CHUNK_SECTORS 8
#define RIP_CHUNK_BYTES   (RIP_CHUNK_SECTORS * CDROM_AUDIO_SECTOR_BYTES)

typedef struct {
    cdrom_status_t disc;
    cd_metadata_status_t meta;
} rip_request_t;

static SemaphoreHandle_t s_mutex = NULL;
static cd_ripper_status_t s_status = {0};
static bool s_task_running = false;
static volatile bool s_dirty = false;
static volatile bool s_cancel_requested = false;

static void publish_status(cd_ripper_state_t state, int current_track, int total_tracks, uint32_t percent,
                           const char *path, const char *error) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.state = state;
    s_status.current_track = current_track;
    s_status.total_tracks = total_tracks;
    s_status.current_percent = percent;
    if (path != NULL) snprintf(s_status.current_path, sizeof(s_status.current_path), "%s", path);
    if (error != NULL) snprintf(s_status.last_error, sizeof(s_status.last_error), "%s", error);
    if (state != CD_RIPPER_STATE_ERROR && error == NULL) s_status.last_error[0] = '\0';
    xSemaphoreGive(s_mutex);
    s_dirty = true;
}

static bool rip_task_is_running(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool running = s_task_running;
    xSemaphoreGive(s_mutex);
    return running;
}

static const char *fallback(const char *s, const char *fb) {
    return (s != NULL && s[0] != '\0') ? s : fb;
}

static void sanitize_component(char *s) {
    char *dst = s;
    for (char *src = s; *src != '\0'; src++) {
        unsigned char c = (unsigned char)*src;
        if (c < 0x20 || strchr("\\/:*?\"<>|", c) != NULL) {
            *dst++ = '_';
        } else {
            *dst++ = *src;
        }
    }
    *dst = '\0';
    while (dst > s && (dst[-1] == ' ' || dst[-1] == '.')) {
        *--dst = '\0';
    }
    if (s[0] == '\0') snprintf(s, 2, "_");
}

static esp_err_t mkdir_if_needed(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    if (mkdir(path, 0775) == 0) return ESP_OK;
    return errno == EEXIST ? ESP_OK : ESP_FAIL;
}

static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void make_wav_header(uint8_t header[44], uint32_t pcm_bytes) {
    memset(header, 0, 44);
    memcpy(header + 0, "RIFF", 4);
    put_le32(header + 4, 36 + pcm_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    put_le32(header + 16, 16);
    put_le16(header + 20, 1);
    put_le16(header + 22, 2);
    put_le32(header + 24, 44100);
    put_le32(header + 28, 44100 * CDROM_AUDIO_FRAME_BYTES);
    put_le16(header + 32, CDROM_AUDIO_FRAME_BYTES);
    put_le16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    put_le32(header + 40, pcm_bytes);
}

static void build_album_stem(const cd_metadata_status_t *meta, char *out, size_t out_len) {
    const char *artist = fallback(meta->artist, "Unknown Artist");
    const char *album = fallback(meta->album, "Unknown Album");
    const char *year = fallback(meta->year, "0000");
    if (meta->disc_count > 1 && meta->disc_number > 0) {
        snprintf(out, out_len, "%s - %s - CD%d (%s)", artist, album, meta->disc_number, year);
    } else {
        snprintf(out, out_len, "%s - %s (%s)", artist, album, year);
    }
    sanitize_component(out);
}

static void build_file_name(const cd_metadata_status_t *meta, const cdrom_track_t *track, int track_index, char *out,
                            size_t out_len) {
    const char *artist = fallback(meta->artist, "Unknown Artist");
    const char *album = fallback(meta->album, "Unknown Album");
    const char *year = fallback(meta->year, "0000");
    const char *title = "Track";
    char fallback_title[24];
    if (track_index < meta->track_title_count && meta->track_titles[track_index][0] != '\0') {
        title = meta->track_titles[track_index];
    } else {
        snprintf(fallback_title, sizeof(fallback_title), "Track %02d", track->number);
        title = fallback_title;
    }

    if (meta->disc_count > 1 && meta->disc_number > 0) {
        snprintf(
            out, out_len, "%s - %s - CD%d - %02d - %s (%s).wav", artist, album, meta->disc_number, track->number,
            title, year
        );
    } else {
        snprintf(out, out_len, "%s - %s - %02d - %s (%s).wav", artist, album, track->number, title, year);
    }
    sanitize_component(out);
}

static int count_audio_tracks(const cdrom_status_t *disc) {
    int count = 0;
    for (int i = 0; i < disc->track_count; i++) {
        if (disc->tracks[i].is_audio) count++;
    }
    return count;
}

static esp_err_t rip_one_track(const cdrom_track_t *track, int track_index, int audio_ordinal, int total_audio,
                               const cd_metadata_status_t *meta, const char *album_dir, uint8_t *buf) {
    char filename[320];
    build_file_name(meta, track, track_index, filename, sizeof(filename));

    char path[896];
    char part_path[sizeof(path) + 5];
    snprintf(path, sizeof(path), "%s/%s", album_dir, filename);
    snprintf(part_path, sizeof(part_path), "%s.part", path);
    publish_status(CD_RIPPER_STATE_RIPPING, audio_ordinal, total_audio, 0, path, NULL);

    unlink(part_path);
    FILE *f = fopen(part_path, "wb");
    if (f == NULL) {
        char err[96];
        snprintf(err, sizeof(err), "open failed: errno %d", errno);
        publish_status(CD_RIPPER_STATE_ERROR, audio_ordinal, total_audio, 0, path, err);
        return ESP_FAIL;
    }

    uint32_t sectors = track->end_lba - track->start_lba;
    uint32_t pcm_bytes = sectors * CDROM_AUDIO_SECTOR_BYTES;
    uint8_t header[44];
    make_wav_header(header, pcm_bytes);
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        publish_status(CD_RIPPER_STATE_ERROR, audio_ordinal, total_audio, 0, path, "header write failed");
        return ESP_FAIL;
    }

    uint32_t lba = track->start_lba;
    uint32_t done = 0;
    uint32_t last_percent = 0;
    while (done < sectors) {
        if (s_cancel_requested) {
            fclose(f);
            unlink(part_path);
            publish_status(CD_RIPPER_STATE_CANCELLED, audio_ordinal, total_audio, (done * 100u) / sectors, path, NULL);
            return ESP_ERR_INVALID_STATE;
        }

        uint16_t chunk = (sectors - done) > RIP_CHUNK_SECTORS ? RIP_CHUNK_SECTORS : (uint16_t)(sectors - done);
        esp_err_t res = ESP_FAIL;
        while (chunk > 0) {
            res = cdrom_audio_read_sectors(lba, chunk, buf);
            if (res == ESP_OK) break;
            if (chunk == 1) break;
            chunk /= 2;
        }
        if (res != ESP_OK) {
            fclose(f);
            publish_status(CD_RIPPER_STATE_ERROR, audio_ordinal, total_audio, 0, path, esp_err_to_name(res));
            return res;
        }

        size_t bytes = (size_t)chunk * CDROM_AUDIO_SECTOR_BYTES;
        if (fwrite(buf, 1, bytes, f) != bytes) {
            fclose(f);
            publish_status(CD_RIPPER_STATE_ERROR, audio_ordinal, total_audio, 0, path, "audio write failed");
            return ESP_FAIL;
        }
        lba += chunk;
        done += chunk;
        uint32_t percent = (done * 100u) / sectors;
        if (percent == 0 && done > 0) percent = 1;
        if (percent != last_percent) {
            publish_status(CD_RIPPER_STATE_RIPPING, audio_ordinal, total_audio, percent, path, NULL);
            last_percent = percent;
        }
    }

    if (fclose(f) != 0) {
        publish_status(CD_RIPPER_STATE_ERROR, audio_ordinal, total_audio, 100, path, "close failed");
        return ESP_FAIL;
    }

    unlink(path);
    if (rename(part_path, path) != 0) {
        char err[96];
        snprintf(err, sizeof(err), "rename failed: errno %d", errno);
        unlink(part_path);
        publish_status(CD_RIPPER_STATE_ERROR, audio_ordinal, total_audio, 100, path, err);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Ripped %s", path);
    return ESP_OK;
}

static void rip_task(void *arg) {
    rip_request_t *req = (rip_request_t *)arg;
    publish_status(CD_RIPPER_STATE_MOUNTING_SD, 0, 0, 0, "", NULL);

    esp_err_t res = sd_storage_mount();
    if (res != ESP_OK) {
        publish_status(CD_RIPPER_STATE_ERROR, 0, 0, 0, "", esp_err_to_name(res));
        goto done;
    }

    char album_stem[256];
    build_album_stem(&req->meta, album_stem, sizeof(album_stem));

    char root_dir[192];
    snprintf(root_dir, sizeof(root_dir), "%s/Music", SD_STORAGE_MOUNT_POINT);
    res = mkdir_if_needed(root_dir);
    if (res != ESP_OK) {
        publish_status(CD_RIPPER_STATE_ERROR, 0, 0, 0, root_dir, "mkdir Music failed");
        goto done;
    }

    char album_dir[512];
    snprintf(album_dir, sizeof(album_dir), "%s/%s", root_dir, album_stem);
    res = mkdir_if_needed(album_dir);
    if (res != ESP_OK) {
        publish_status(CD_RIPPER_STATE_ERROR, 0, 0, 0, album_dir, "mkdir album failed");
        goto done;
    }

    uint8_t *buf = heap_caps_malloc(RIP_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) buf = heap_caps_malloc(RIP_CHUNK_BYTES, MALLOC_CAP_8BIT);
    if (buf == NULL) {
        publish_status(CD_RIPPER_STATE_ERROR, 0, 0, 0, album_dir, "no rip buffer");
        goto done;
    }

    int total_audio = count_audio_tracks(&req->disc);
    int audio_ordinal = 0;
    for (int i = 0; i < req->disc.track_count; i++) {
        if (s_cancel_requested) {
            publish_status(CD_RIPPER_STATE_CANCELLED, audio_ordinal, total_audio, 0, album_dir, NULL);
            heap_caps_free(buf);
            goto done;
        }
        if (!req->disc.tracks[i].is_audio) continue;
        audio_ordinal++;
        res = rip_one_track(&req->disc.tracks[i], i, audio_ordinal, total_audio, &req->meta, album_dir, buf);
        if (res != ESP_OK) {
            heap_caps_free(buf);
            goto done;
        }
    }
    heap_caps_free(buf);
    publish_status(CD_RIPPER_STATE_DONE, total_audio, total_audio, 100, album_dir, NULL);

done:
    s_cancel_requested = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_task_running = false;
    xSemaphoreGive(s_mutex);
    free(req);
    vTaskDelete(NULL);
}

esp_err_t cd_ripper_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    publish_status(CD_RIPPER_STATE_IDLE, 0, 0, 0, "", NULL);
    return ESP_OK;
}

esp_err_t cd_ripper_start(const cdrom_status_t *disc, const cd_metadata_status_t *metadata) {
    if (disc == NULL || metadata == NULL || !disc->disc_present || disc->track_count <= 0) return ESP_ERR_INVALID_ARG;
    if (count_audio_tracks(disc) <= 0) return ESP_ERR_INVALID_ARG;
    if (rip_task_is_running()) return ESP_ERR_INVALID_STATE;
    s_cancel_requested = false;

    rip_request_t *req = calloc(1, sizeof(rip_request_t));
    if (req == NULL) return ESP_ERR_NO_MEM;
    req->disc = *disc;
    req->meta = *metadata;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_task_running = true;
    xSemaphoreGive(s_mutex);

    if (xTaskCreate(rip_task, "cd_ripper", 8192, req, 4, NULL) != pdPASS) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_task_running = false;
        xSemaphoreGive(s_mutex);
        free(req);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void cd_ripper_stop(void) {
    if (!rip_task_is_running()) return;
    s_cancel_requested = true;
    s_dirty = true;
}

void cd_ripper_get_status(cd_ripper_status_t *out) {
    if (out == NULL) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mutex);
}

bool cd_ripper_is_active(void) {
    return rip_task_is_running();
}

bool cd_ripper_consume_dirty(void) {
    bool was_dirty = s_dirty;
    s_dirty = false;
    return was_dirty;
}
