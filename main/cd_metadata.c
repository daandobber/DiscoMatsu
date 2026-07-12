#include "cd_metadata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"
#include "wifi_setup.h"

static const char *TAG = "cd_metadata";

#define USER_AGENT   "Disc-O-Matsu/0.1 (https://github.com/daandobber/DiscoMatsu)"
#define HTTP_BUF_CAP     (512 * 1024)  // popular albums can have dozens of cataloged releases -> a big JSON response
#define JPEG_BUF_CAP     (256 * 1024)
#define JPEG_WORK_BUF_SIZE (128 * 1024)
#define COVER_ART_MAX_DIM 120

static SemaphoreHandle_t s_mutex          = NULL;
static cd_metadata_status_t s_status      = {0};
static volatile bool s_dirty              = false;
static uint16_t *s_cover_art               = NULL;

typedef struct {
    cdrom_track_t tracks[CDROM_AUDIO_MAX_TRACKS];
    int track_count;
} lookup_request_t;

static void set_state(cd_metadata_state_t state) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.state = state;
    xSemaphoreGive(s_mutex);
    s_dirty = true;
}

// Builds the MusicBrainz Disc ID: SHA-1 of a fixed-format TOC string
// (2 hex first-track, 2 hex last-track, then 100 slots of 8 hex digits -
// slot 0 is the lead-out LBA+150, slots 1..last_track are each track's
// start LBA+150), base64-encoded with MusicBrainz's URL-safe substitutions.
static void compute_musicbrainz_discid(const cdrom_track_t *tracks, int track_count, char *out_discid, size_t out_cap) {
    out_discid[0] = '\0';
    if (track_count <= 0) return;

    uint8_t first_track  = tracks[0].number;
    uint8_t last_track    = tracks[track_count - 1].number;
    uint32_t leadout_lba = tracks[track_count - 1].end_lba;

    char toc[2 + 2 + 100 * 8 + 1];
    int pos = snprintf(toc, sizeof(toc), "%02X%02X", first_track, last_track);
    pos += snprintf(toc + pos, sizeof(toc) - (size_t)pos, "%08lX", (unsigned long)(leadout_lba + 150));

    for (int slot = 1; slot <= 99; slot++) {
        uint32_t offset = 0;
        for (int j = 0; j < track_count; j++) {
            if (tracks[j].number == slot) {
                offset = tracks[j].start_lba + 150;
                break;
            }
        }
        pos += snprintf(toc + pos, sizeof(toc) - (size_t)pos, "%08lX", (unsigned long)offset);
    }

    unsigned char sha1[20];
    mbedtls_sha1((const unsigned char *)toc, (size_t)pos, sha1);

    unsigned char b64[32];
    size_t b64_len = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &b64_len, sha1, sizeof(sha1));
    b64[b64_len] = '\0';

    for (size_t i = 0; i < b64_len; i++) {
        if (b64[i] == '+') b64[i] = '.';
        else if (b64[i] == '/') b64[i] = '_';
        else if (b64[i] == '=') b64[i] = '-';
    }

    snprintf(out_discid, out_cap, "%s", (const char *)b64);
}

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} http_recv_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        http_recv_ctx_t *ctx = (http_recv_ctx_t *)evt->user_data;
        if (ctx->len + (size_t)evt->data_len <= ctx->cap) {
            memcpy(ctx->buf + ctx->len, evt->data, (size_t)evt->data_len);
            ctx->len += (size_t)evt->data_len;
        }
    }
    return ESP_OK;
}

static esp_err_t http_get(const char *url, uint8_t *out_buf, size_t cap, size_t *out_len, int *out_status) {
    http_recv_ctx_t ctx = {.buf = out_buf, .len = 0, .cap = cap};
    esp_http_client_config_t config = {
        .url               = url,
        .event_handler     = http_event_handler,
        .user_data         = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_FAIL;
    esp_http_client_set_header(client, "User-Agent", USER_AGENT);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        *out_status = esp_http_client_get_status_code(client);
        *out_len    = ctx.len;
        if (ctx.len >= cap) {
            ESP_LOGW(TAG, "%s: response filled the %u-byte buffer, likely truncated", url, (unsigned)cap);
        }
    }
    esp_http_client_cleanup(client);
    return err;
}

// Scans JPEG markers for a Start-Of-Frame to identify the encoding
// variant - in particular, TJpgDec (the decoder behind esp_jpeg) only
// supports baseline (SOF0) JPEGs, not progressive (SOF2), which is a
// common choice for web-optimized images like Cover Art Archive thumbnails.
static const char *jpeg_sof_type(const uint8_t *data, size_t len) {
    if (len < 2 || data[0] != 0xFF || data[1] != 0xD8) return "not a JPEG (no FFD8 SOI marker)";

    size_t i = 2;  // skip SOI (FFD8)
    while (i + 4 <= len) {
        if (data[i] != 0xFF) {
            i++;
            continue;
        }
        uint8_t marker = data[i + 1];
        if (marker == 0x00 || marker == 0xFF || (marker >= 0xD0 && marker <= 0xD9)) {
            i += 2;
            continue;
        }
        uint16_t seg_len = ((uint16_t)data[i + 2] << 8) | data[i + 3];
        if (marker == 0xC0) return "baseline (SOF0)";
        if (marker == 0xC1) return "extended sequential (SOF1)";
        if (marker == 0xC2) return "progressive (SOF2) - NOT supported by this decoder";
        if (marker == 0xC3) return "lossless (SOF3)";
        if (marker >= 0xC5 && marker <= 0xCF && marker != 0xC8) return "other SOF variant, likely unsupported";
        i += 2 + seg_len;
    }
    return "unknown (no SOF marker found)";
}

// If esp_http_client's redirect handling ever surfaces an intermediate
// hop's small notice body through our accumulation buffer, the real JPEG
// data can end up starting at some offset > 0 rather than at the very
// start. Search for the actual SOI (FFD8FF) marker instead of assuming
// it's at offset 0.
static int find_jpeg_start(const uint8_t *data, size_t len) {
    if (len < 3) return -1;
    for (size_t i = 0; i + 3 <= len; i++) {
        if (data[i] == 0xFF && data[i + 1] == 0xD8 && data[i + 2] == 0xFF) return (int)i;
    }
    return -1;
}

static void decode_and_store_cover_art(const uint8_t *jpeg_data, size_t jpeg_len) {
    int jpeg_start = find_jpeg_start(jpeg_data, jpeg_len);
    ESP_LOGI(
        TAG, "Cover art download: %u bytes, magic=%02x%02x%02x%02x, jpeg_start=%d, %s", (unsigned)jpeg_len,
        jpeg_len > 0 ? jpeg_data[0] : 0, jpeg_len > 1 ? jpeg_data[1] : 0, jpeg_len > 2 ? jpeg_data[2] : 0,
        jpeg_len > 3 ? jpeg_data[3] : 0, jpeg_start,
        jpeg_start >= 0 ? jpeg_sof_type(jpeg_data + jpeg_start, jpeg_len - (size_t)jpeg_start) : "n/a"
    );

    if (jpeg_start < 0) {
        // Not a JPEG anywhere in the buffer - probably a redirect/error body
        // coverartarchive.org or archive.org returned instead of the actual
        // image. Log it as text (bounded, since it's clearly not binary
        // image data) to see what it actually says.
        char preview[161];
        size_t n = jpeg_len < sizeof(preview) - 1 ? jpeg_len : sizeof(preview) - 1;
        for (size_t i = 0; i < n; i++) {
            uint8_t c  = jpeg_data[i];
            preview[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        preview[n] = '\0';
        ESP_LOGW(TAG, "Cover art response was not a JPEG: \"%s\"", preview);
        return;
    }
    if (jpeg_start > 0) {
        ESP_LOGI(TAG, "Skipping %d bytes of leading non-JPEG data before the real image", jpeg_start);
        jpeg_data += jpeg_start;
        jpeg_len -= (size_t)jpeg_start;
    }

    // esp_jpeg/TJpgDec's default internal scratch buffer (a few KB) is too
    // small for a lot of real-world JPEGs, failing with JDR_MEM1
    // ("insufficient memory pool for the image", surfaced as ESP_FAIL /
    // "Error in preparing JPEG image! 3"). Providing a generous working
    // buffer explicitly (cheap given PSRAM) avoids that entirely instead of
    // relying on the undersized default.
    static uint8_t *s_jpeg_work_buf = NULL;
    if (s_jpeg_work_buf == NULL) {
        s_jpeg_work_buf = heap_caps_malloc(JPEG_WORK_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata      = (uint8_t *)jpeg_data,
        .indata_size = (uint32_t)jpeg_len,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = JPEG_IMAGE_SCALE_0,
        .flags       = {.swap_color_bytes = 0},
    };
    if (s_jpeg_work_buf != NULL) {
        cfg.advanced.working_buffer      = s_jpeg_work_buf;
        cfg.advanced.working_buffer_size = JPEG_WORK_BUF_SIZE;
    }
    esp_jpeg_image_output_t info = {0};
    esp_err_t info_res = esp_jpeg_get_image_info(&cfg, &info);
    if (info_res != ESP_OK || info.width == 0 || info.height == 0) {
        ESP_LOGW(
            TAG, "Failed to read cover art JPEG info: %s (%ux%u)", esp_err_to_name(info_res), info.width, info.height
        );
        return;
    }

    size_t out_size  = (size_t)info.width * (size_t)info.height * 2;
    uint16_t *pixels = heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        ESP_LOGW(TAG, "No memory for cover art (%ux%u)", info.width, info.height);
        return;
    }

    cfg.outbuf      = (uint8_t *)pixels;
    cfg.outbuf_size = (uint32_t)out_size;
    esp_jpeg_image_output_t out = {0};
    if (esp_jpeg_decode(&cfg, &out) != ESP_OK) {
        ESP_LOGW(TAG, "Cover art JPEG decode failed");
        heap_caps_free(pixels);
        return;
    }
    ESP_LOGI(TAG, "Cover art decoded: %ux%u", out.width, out.height);

    int final_w         = out.width;
    int final_h         = out.height;
    uint16_t *final_buf = pixels;
    if (out.width > COVER_ART_MAX_DIM || out.height > COVER_ART_MAX_DIM) {
        int dst_w, dst_h;
        if (out.width >= out.height) {
            dst_w = COVER_ART_MAX_DIM;
            dst_h = (int)((int64_t)out.height * COVER_ART_MAX_DIM / out.width);
        } else {
            dst_h = COVER_ART_MAX_DIM;
            dst_w = (int)((int64_t)out.width * COVER_ART_MAX_DIM / out.height);
        }
        if (dst_w < 1) dst_w = 1;
        if (dst_h < 1) dst_h = 1;

        uint16_t *small = heap_caps_malloc((size_t)dst_w * (size_t)dst_h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (small != NULL) {
            for (int y = 0; y < dst_h; y++) {
                int sy = y * out.height / dst_h;
                for (int x = 0; x < dst_w; x++) {
                    int sx                    = x * out.width / dst_w;
                    small[y * dst_w + x] = pixels[sy * out.width + sx];
                }
            }
            heap_caps_free(pixels);
            final_buf = small;
            final_w   = dst_w;
            final_h   = dst_h;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_cover_art != NULL) heap_caps_free(s_cover_art);
    s_cover_art               = final_buf;
    s_status.cover_art_rgb565 = s_cover_art;
    s_status.cover_art_width  = final_w;
    s_status.cover_art_height = final_h;
    xSemaphoreGive(s_mutex);
    s_dirty = true;
}

static void lookup_task(void *arg) {
    lookup_request_t *req = (lookup_request_t *)arg;
    uint8_t *buf           = NULL;

    set_state(CD_METADATA_STATE_LOOKING_UP);

    // WiFi is only brought up for the duration of this lookup - an active
    // connection appears to disturb the USB host connection to the CD
    // drive on this hardware, so it must not be left connected any longer
    // than necessary.
    if (!wifi_setup_connect_blocking(15000)) {
        ESP_LOGI(TAG, "No WiFi, skipping metadata lookup");
        set_state(CD_METADATA_STATE_NOT_FOUND);
        goto done;
    }

    char discid[32];
    compute_musicbrainz_discid(req->tracks, req->track_count, discid, sizeof(discid));
    ESP_LOGI(TAG, "MusicBrainz disc ID: %s", discid);

    char url[192];
    snprintf(
        url, sizeof(url), "https://musicbrainz.org/ws/2/discid/%s?fmt=json&inc=recordings+artist-credits", discid
    );

    buf = heap_caps_malloc(HTTP_BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        set_state(CD_METADATA_STATE_NOT_FOUND);
        goto done;
    }

    size_t len = 0;
    int status = 0;
    esp_err_t err = http_get(url, buf, HTTP_BUF_CAP, &len, &status);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "MusicBrainz lookup failed: %s status=%d", esp_err_to_name(err), status);
        set_state(CD_METADATA_STATE_NOT_FOUND);
        goto done;
    }
    ESP_LOGI(TAG, "MusicBrainz response: %u bytes%s", (unsigned)len, len >= HTTP_BUF_CAP ? " (possibly truncated!)" : "");

    cJSON *root = cJSON_ParseWithLength((const char *)buf, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "MusicBrainz JSON parse failed");
        set_state(CD_METADATA_STATE_NOT_FOUND);
        goto done;
    }

    cJSON *releases = cJSON_GetObjectItem(root, "releases");
    if (!cJSON_IsArray(releases) || cJSON_GetArraySize(releases) == 0) {
        ESP_LOGI(TAG, "No MusicBrainz release match for this disc");
        cJSON_Delete(root);
        set_state(CD_METADATA_STATE_NOT_FOUND);
        goto done;
    }

    cJSON *release        = cJSON_GetArrayItem(releases, 0);
    cJSON *title          = cJSON_GetObjectItem(release, "title");
    cJSON *artist_credit  = cJSON_GetObjectItem(release, "artist-credit");
    cJSON *release_id     = cJSON_GetObjectItem(release, "id");
    cJSON *date           = cJSON_GetObjectItem(release, "date");
    cJSON *media          = cJSON_GetObjectItem(release, "media");

    char release_mbid[64] = {0};
    if (cJSON_IsString(release_id)) snprintf(release_mbid, sizeof(release_mbid), "%s", release_id->valuestring);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_status.album, 0, sizeof(s_status.album));
    memset(s_status.artist, 0, sizeof(s_status.artist));
    memset(s_status.year, 0, sizeof(s_status.year));
    memset(s_status.track_titles, 0, sizeof(s_status.track_titles));
    s_status.track_title_count = 0;
    s_status.disc_number = 0;
    s_status.disc_count = 0;

    if (cJSON_IsString(title)) snprintf(s_status.album, sizeof(s_status.album), "%s", title->valuestring);
    if (cJSON_IsString(date) && strlen(date->valuestring) >= 4) {
        snprintf(s_status.year, sizeof(s_status.year), "%.4s", date->valuestring);
    }
    if (cJSON_IsArray(artist_credit) && cJSON_GetArraySize(artist_credit) > 0) {
        cJSON *first_artist = cJSON_GetArrayItem(artist_credit, 0);
        cJSON *name         = cJSON_GetObjectItem(first_artist, "name");
        if (cJSON_IsString(name)) snprintf(s_status.artist, sizeof(s_status.artist), "%s", name->valuestring);
    }
    if (cJSON_IsArray(media) && cJSON_GetArraySize(media) > 0) {
        s_status.disc_count = cJSON_GetArraySize(media);
        cJSON *first_medium = cJSON_GetArrayItem(media, 0);
        cJSON *position      = cJSON_GetObjectItem(first_medium, "position");
        if (cJSON_IsNumber(position)) s_status.disc_number = position->valueint;
        cJSON *track_list    = cJSON_GetObjectItem(first_medium, "tracks");
        if (cJSON_IsArray(track_list)) {
            int n = cJSON_GetArraySize(track_list);
            if (n > CDROM_AUDIO_MAX_TRACKS) n = CDROM_AUDIO_MAX_TRACKS;
            for (int i = 0; i < n; i++) {
                cJSON *t       = cJSON_GetArrayItem(track_list, i);
                cJSON *t_title = cJSON_GetObjectItem(t, "title");
                if (cJSON_IsString(t_title)) {
                    snprintf(s_status.track_titles[i], sizeof(s_status.track_titles[i]), "%s", t_title->valuestring);
                }
            }
            s_status.track_title_count = n;
        }
    }
    s_status.state = CD_METADATA_STATE_FOUND;
    xSemaphoreGive(s_mutex);
    s_dirty = true;

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Found: %s - %s", s_status.artist, s_status.album);

    if (release_mbid[0] != '\0') {
        char art_url[160];
        snprintf(art_url, sizeof(art_url), "https://coverartarchive.org/release/%s/front-250", release_mbid);

        uint8_t *jbuf = heap_caps_malloc(JPEG_BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (jbuf != NULL) {
            size_t jlen  = 0;
            int jstatus  = 0;
            esp_err_t jerr = http_get(art_url, jbuf, JPEG_BUF_CAP, &jlen, &jstatus);
            if (jerr == ESP_OK && jstatus == 200 && jlen > 0) {
                decode_and_store_cover_art(jbuf, jlen);
            } else {
                ESP_LOGI(TAG, "No cover art available (status=%d)", jstatus);
            }
            heap_caps_free(jbuf);
        }
    }

done:
    wifi_setup_disconnect();
    if (buf != NULL) heap_caps_free(buf);
    free(req);
    vTaskDelete(NULL);
}

esp_err_t cd_metadata_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void cd_metadata_request_lookup(const cdrom_track_t *tracks, int track_count) {
    if (track_count <= 0) return;

    lookup_request_t *req = calloc(1, sizeof(lookup_request_t));
    if (req == NULL) return;

    req->track_count = track_count > CDROM_AUDIO_MAX_TRACKS ? CDROM_AUDIO_MAX_TRACKS : track_count;
    memcpy(req->tracks, tracks, sizeof(cdrom_track_t) * (size_t)req->track_count);

    if (xTaskCreate(lookup_task, "cd_metadata", 16384, req, 4, NULL) != pdPASS) {
        free(req);
    }
}

void cd_metadata_clear(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = CD_METADATA_STATE_IDLE;
    if (s_cover_art != NULL) {
        heap_caps_free(s_cover_art);
        s_cover_art = NULL;
    }
    xSemaphoreGive(s_mutex);
    s_dirty = true;
}

void cd_metadata_get_status(cd_metadata_status_t *out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mutex);
}

bool cd_metadata_consume_dirty(void) {
    bool was_dirty = s_dirty;
    s_dirty         = false;
    return was_dirty;
}
