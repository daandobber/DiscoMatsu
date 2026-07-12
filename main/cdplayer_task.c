#include "cdplayer_task.h"

#include "audio_output.h"
#include "cdrom_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cdplayer";

#define CHUNK_SECTORS   8  // ~18.8 KB / chunk, ~106 ms of audio per chunk - kept small since this
                           // particular drive stalls on larger bulk requests
#define CHUNK_BYTES     (CHUNK_SECTORS * CDROM_AUDIO_SECTOR_BYTES)

typedef enum {
    CDPLAYER_CMD_PLAY,
    CDPLAYER_CMD_STOP,
    CDPLAYER_CMD_PAUSE,
    CDPLAYER_CMD_RESUME,
    CDPLAYER_CMD_NEXT,
    CDPLAYER_CMD_PREV,
} cdplayer_cmd_type_t;

typedef struct {
    cdplayer_cmd_type_t type;
    int track_index;
} cdplayer_cmd_t;

static QueueHandle_t s_cmd_queue        = NULL;
static SemaphoreHandle_t s_state_mutex  = NULL;
static cdplayer_state_t s_state         = {.track_index = -1};
static volatile bool s_dirty            = false;

static void publish_state(bool playing, bool paused, int track_index, uint32_t elapsed_sec, uint32_t total_sec) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool changed =
        s_state.playing != playing || s_state.paused != paused || s_state.track_index != track_index ||
        s_state.elapsed_sec != elapsed_sec || s_state.total_sec != total_sec;
    s_state.playing     = playing;
    s_state.paused      = paused;
    s_state.track_index = track_index;
    s_state.elapsed_sec = elapsed_sec;
    s_state.total_sec   = total_sec;
    xSemaphoreGive(s_state_mutex);
    if (changed) s_dirty = true;
}

static bool find_next_audio_track(const cdrom_status_t *status, int from_index, int step, int *out_index) {
    int idx = from_index + step;
    while (idx >= 0 && idx < status->track_count) {
        if (status->tracks[idx].is_audio) {
            *out_index = idx;
            return true;
        }
        idx += step;
    }
    return false;
}

static void player_task(void *arg) {
    (void)arg;
    uint8_t *chunk_buf = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (chunk_buf == NULL) chunk_buf = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_8BIT);
    if (chunk_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate CD-audio chunk buffer");
        vTaskDelete(NULL);
        return;
    }

    bool active         = false;
    bool paused         = false;
    int current_index   = -1;
    cdrom_track_t track = {0};
    uint32_t current_lba = 0;

    while (1) {
        cdplayer_cmd_t cmd;
        TickType_t wait = (active && !paused) ? 0 : portMAX_DELAY;
        if (xQueueReceive(s_cmd_queue, &cmd, wait) == pdTRUE) {
            cdrom_status_t status;
            switch (cmd.type) {
                case CDPLAYER_CMD_PLAY:
                    cdrom_audio_get_status(&status);
                    if (cmd.track_index < 0 || cmd.track_index >= status.track_count ||
                        !status.tracks[cmd.track_index].is_audio) {
                        break;
                    }
                    if (active) audio_output_end();
                    track         = status.tracks[cmd.track_index];
                    current_index = cmd.track_index;
                    current_lba   = track.start_lba;
                    active        = (audio_output_begin(44100) == ESP_OK);
                    paused        = false;
                    publish_state(active, false, current_index, 0, (track.end_lba - track.start_lba) / CDROM_AUDIO_SECTORS_PER_SEC);
                    break;

                case CDPLAYER_CMD_STOP:
                    if (active) audio_output_end();
                    active        = false;
                    paused        = false;
                    current_index = -1;
                    publish_state(false, false, -1, 0, 0);
                    break;

                case CDPLAYER_CMD_PAUSE:
                    if (active) paused = true;
                    publish_state(
                        active, paused, current_index, (current_lba - track.start_lba) / CDROM_AUDIO_SECTORS_PER_SEC,
                        (track.end_lba - track.start_lba) / CDROM_AUDIO_SECTORS_PER_SEC
                    );
                    break;

                case CDPLAYER_CMD_RESUME:
                    if (active) paused = false;
                    publish_state(
                        active, paused, current_index, (current_lba - track.start_lba) / CDROM_AUDIO_SECTORS_PER_SEC,
                        (track.end_lba - track.start_lba) / CDROM_AUDIO_SECTORS_PER_SEC
                    );
                    break;

                case CDPLAYER_CMD_NEXT:
                case CDPLAYER_CMD_PREV: {
                    cdrom_audio_get_status(&status);
                    int step = (cmd.type == CDPLAYER_CMD_NEXT) ? 1 : -1;
                    int next_index;
                    if (!find_next_audio_track(&status, current_index, step, &next_index)) break;
                    if (active) audio_output_end();
                    track         = status.tracks[next_index];
                    current_index = next_index;
                    current_lba   = track.start_lba;
                    active        = (audio_output_begin(44100) == ESP_OK);
                    paused        = false;
                    publish_state(active, false, current_index, 0, (track.end_lba - track.start_lba) / CDROM_AUDIO_SECTORS_PER_SEC);
                    break;
                }
            }
            continue;
        }

        if (!active || paused) continue;

        uint32_t left  = track.end_lba - current_lba;
        uint16_t chunk = (left < CHUNK_SECTORS) ? (uint16_t)left : CHUNK_SECTORS;
        if (chunk == 0) {
            cdrom_status_t status;
            cdrom_audio_get_status(&status);
            int next_index;
            if (find_next_audio_track(&status, current_index, 1, &next_index)) {
                audio_output_end();
                track         = status.tracks[next_index];
                current_index = next_index;
                current_lba   = track.start_lba;
                active        = (audio_output_begin(44100) == ESP_OK);
                publish_state(active, false, current_index, 0, (track.end_lba - track.start_lba) / CDROM_AUDIO_SECTORS_PER_SEC);
            } else {
                audio_output_end();
                active        = false;
                current_index = -1;
                publish_state(false, false, -1, 0, 0);
            }
            continue;
        }

        esp_err_t res = cdrom_audio_read_sectors(current_lba, chunk, chunk_buf);
        if (res != ESP_OK) {
            ESP_LOGW(TAG, "read_sectors failed: %s", esp_err_to_name(res));
            audio_output_end();
            active        = false;
            current_index = -1;
            publish_state(false, false, -1, 0, 0);
            continue;
        }

        audio_output_write((const int16_t *)chunk_buf, (size_t)chunk * CDROM_AUDIO_FRAMES_PER_SECTOR);
        current_lba += chunk;

        uint32_t elapsed = (current_lba - track.start_lba) / CDROM_AUDIO_SECTORS_PER_SEC;
        uint32_t total   = (track.end_lba - track.start_lba) / CDROM_AUDIO_SECTORS_PER_SEC;
        publish_state(true, false, current_index, elapsed, total);
    }
}

esp_err_t cdplayer_task_init(void) {
    s_state_mutex = xSemaphoreCreateMutex();
    s_cmd_queue   = xQueueCreate(4, sizeof(cdplayer_cmd_t));
    if (s_state_mutex == NULL || s_cmd_queue == NULL) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreate(player_task, "cdplayer", 8192, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

void cdplayer_play_track(int track_index) {
    cdplayer_cmd_t cmd = {.type = CDPLAYER_CMD_PLAY, .track_index = track_index};
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void cdplayer_stop(void) {
    cdplayer_cmd_t cmd = {.type = CDPLAYER_CMD_STOP};
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void cdplayer_pause(bool pause) {
    cdplayer_cmd_t cmd = {.type = pause ? CDPLAYER_CMD_PAUSE : CDPLAYER_CMD_RESUME};
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void cdplayer_next(void) {
    cdplayer_cmd_t cmd = {.type = CDPLAYER_CMD_NEXT};
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void cdplayer_prev(void) {
    cdplayer_cmd_t cmd = {.type = CDPLAYER_CMD_PREV};
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void cdplayer_get_state(cdplayer_state_t *out_state) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out_state = s_state;
    xSemaphoreGive(s_state_mutex);
}

bool cdplayer_consume_dirty(void) {
    bool was_dirty = s_dirty;
    s_dirty         = false;
    return was_dirty;
}
