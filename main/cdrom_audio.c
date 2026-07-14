#include "cdrom_audio.h"

#include <inttypes.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_SOC_USB_OTG_SUPPORTED
#include "bsp/power.h"
#include "cd_metadata.h"
#include "cdplayer_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb_msc_scsi.h"

static const char *TAG = "cdrom_audio";

#define POLL_INTERVAL_MS   2000
#define SPIN_UP_TIMEOUT_MS 20000  // START STOP UNIT with Immed=0 blocks until the drive is up to speed
#define TOC_BUF_SIZE       804    // header(4) + up to 99 tracks + lead-out, 8 bytes each

static cdrom_status_t s_status = {0};
static SemaphoreHandle_t s_status_mutex = NULL;
static volatile bool s_dirty = false;
static volatile bool s_drive_connected = false;

static void mark_dirty(void) {
    s_dirty = true;
}

static void on_msc_ready(void *arg) {
    (void)arg;
    s_drive_connected = true;
    mark_dirty();
}

static void on_msc_gone(void *arg) {
    (void)arg;
    s_drive_connected = false;
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.drive_present = false;
    s_status.disc_present  = false;
    s_status.track_count   = 0;
    xSemaphoreGive(s_status_mutex);
    mark_dirty();
}

#define SENSE_KEY_UNIT_ATTENTION 0x06

// Fetches REQUEST SENSE (key/ASC/ASCQ) after a failed command - both for
// diagnostic logging and so callers can special-case "unit attention"
// (0x06), which just means "resend the command", not a real failure.
// Returns true and fills *out_sense_key if sense data was retrieved.
static bool fetch_sense(const char *context, uint8_t *out_sense_key) {
    uint8_t cdb[6]    = {0x03, 0, 0, 0, 18, 0};
    uint8_t sense[18] = {0};
    if (usb_msc_scsi_command(cdb, sizeof(cdb), USB_MSC_SCSI_DIR_IN, sense, sizeof(sense), 2000) != ESP_OK) {
        ESP_LOGW(TAG, "%s: request sense also failed", context);
        return false;
    }
    uint8_t sense_key = sense[2] & 0x0F;
    ESP_LOGW(TAG, "%s: sense key=0x%02x ASC=0x%02x ASCQ=0x%02x", context, sense_key, sense[12], sense[13]);
    *out_sense_key = sense_key;
    return true;
}

// Sends a 6-byte CDB with no data phase, transparently retrying (a few
// times, no delay) on "unit attention" - the drive telling us to just
// resend the command, e.g. right after a power-on/reset - rather than
// treating that as a real failure. A unit attention is also how a drive
// reports "medium may have changed" (e.g. the disc was swapped without the
// USB connection ever dropping), so callers that care are told via
// out_saw_unit_attention whether one was seen, even if a retry ultimately
// succeeded.
static esp_err_t send_scsi_no_data_retrying(
    const uint8_t cdb[6], uint32_t timeout_ms, const char *context, bool *out_saw_unit_attention
) {
    if (out_saw_unit_attention) *out_saw_unit_attention = false;

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        ret = usb_msc_scsi_command(cdb, 6, USB_MSC_SCSI_DIR_NONE, NULL, 0, timeout_ms);
        if (ret == ESP_OK) return ESP_OK;

        uint8_t sense_key = 0;
        if (!fetch_sense(context, &sense_key) || sense_key != SENSE_KEY_UNIT_ATTENTION) return ret;
        if (out_saw_unit_attention) *out_saw_unit_attention = true;
        ESP_LOGI(TAG, "%s: unit attention, resending", context);
    }
    return ret;
}

static esp_err_t send_test_unit_ready(bool *out_medium_may_have_changed) {
    uint8_t cdb[6] = {0x00, 0, 0, 0, 0, 0};
    return send_scsi_no_data_retrying(cdb, 2000, "test_unit_ready", out_medium_may_have_changed);
}

// START STOP UNIT with Immed=0: on drives that honor this, the command
// blocks until the motor has actually spun up (or fails, e.g. no disc
// present). Some USB-ATAPI bridges don't honor Immed=0 and return quickly
// regardless - in that case the caller falls back to polling
// TEST UNIT READY, which reports NOT READY / "in progress" until the drive
// is actually up to speed.
static esp_err_t send_spin_up(void) {
    uint8_t cdb[6] = {0x1B, 0x00, 0x00, 0x00, 0x01, 0x00};
    return send_scsi_no_data_retrying(cdb, SPIN_UP_TIMEOUT_MS, "spin_up", NULL);
}

static void fill_read_toc_cdb(uint8_t cdb[10], uint16_t alloc_len) {
    memset(cdb, 0, 10);
    cdb[0] = 0x43;                                // READ TOC/PMA/ATIP
    cdb[1] = 0x00;                                // MSF = 0 (LBA addresses)
    cdb[2] = 0x00;                                // format 0 = TOC
    cdb[6] = 0x01;                                // starting track
    cdb[7] = (uint8_t)((alloc_len >> 8) & 0xFF);
    cdb[8] = (uint8_t)(alloc_len & 0xFF);
}

static esp_err_t read_toc(cdrom_track_t *tracks, int max_tracks, int *out_count) {
    // static: this buffer is too large to put on cdrom_poll's stack safely
    // (it already holds a 99-entry track array further up the call chain).
    static uint8_t buf[TOC_BUF_SIZE];
    uint8_t cdb[10];

    // Some drives stall the CSW phase if asked for more data than they
    // actually have (a generously-oversized allocation length). Read just
    // the 4-byte header first to learn the real size, then re-read with
    // the exact length - the same two-step approach cdparanoia/the Linux
    // kernel use.
    fill_read_toc_cdb(cdb, 4);
    esp_err_t ret = usb_msc_scsi_command(cdb, sizeof(cdb), USB_MSC_SCSI_DIR_IN, buf, 4, 8000);
    if (ret != ESP_OK) {
        uint8_t sense_key = 0;
        fetch_sense("read_toc_header", &sense_key);
        return ret;
    }

    uint16_t header_len = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t exact_len   = (uint16_t)(header_len + 2);  // header_len excludes these 2 length bytes
    if (exact_len < 4) exact_len = 4;
    if (exact_len > TOC_BUF_SIZE) exact_len = TOC_BUF_SIZE;

    fill_read_toc_cdb(cdb, exact_len);
    ret = usb_msc_scsi_command(cdb, sizeof(cdb), USB_MSC_SCSI_DIR_IN, buf, exact_len, 8000);
    if (ret != ESP_OK) {
        uint8_t sense_key = 0;
        fetch_sense("read_toc", &sense_key);
        return ret;
    }

    uint16_t toc_len         = ((uint16_t)buf[0] << 8) | buf[1];
    uint8_t first_track      = buf[2];
    uint8_t last_track       = buf[3];
    uint16_t available       = toc_len > 2 ? (uint16_t)(toc_len - 2) : 0;
    int max_descriptors      = available / 8;
    int count                = 0;

    for (int i = 0; i < max_descriptors && count < max_tracks; i++) {
        const uint8_t *d = &buf[4 + i * 8];
        uint8_t control   = d[1] & 0x0F;
        uint8_t track_num = d[2];
        uint32_t start_lba =
            ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | (uint32_t)d[7];

        if (track_num == 0xAA) {
            // Lead-out: closes off the previous track's range.
            if (count > 0) tracks[count - 1].end_lba = start_lba;
            continue;
        }
        if (track_num < first_track || track_num > last_track) continue;

        tracks[count].number    = track_num;
        tracks[count].is_audio  = (control & 0x04) == 0;
        tracks[count].start_lba = start_lba;
        tracks[count].end_lba   = start_lba;  // filled in from the next track's start, or lead-out
        if (count > 0) tracks[count - 1].end_lba = start_lba;
        count++;
    }

    *out_count = count;
    return ESP_OK;
}

static void poll_task(void *arg) {
    (void)arg;
    bool was_disc_present  = false;
    bool was_connected     = false;
    int not_ready_polls    = 0;

    while (1) {
        bool connected = s_drive_connected;

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        if (s_status.drive_present != connected) {
            s_status.drive_present = connected;
            mark_dirty();
        }
        xSemaphoreGive(s_status_mutex);

        if (connected && !was_connected) {
            // Freshly connected: command the motor to spin up and wait for
            // it to actually finish (or fail, e.g. no disc present) instead
            // of hammering TEST UNIT READY while it's still settling.
            esp_err_t spin_res = send_spin_up();
            ESP_LOGI(TAG, "Initial spin-up %s", spin_res == ESP_OK ? "succeeded" : "failed (no disc, or drive quirk)");
            not_ready_polls = 0;
        }
        was_connected = connected;

        if (connected) {
            bool medium_may_have_changed = false;
            esp_err_t tur      = send_test_unit_ready(&medium_may_have_changed);
            bool disc_present = (tur == ESP_OK);

            if (!disc_present) {
                not_ready_polls++;
                if (not_ready_polls % 5 == 0) {
                    ESP_LOGI(
                        TAG, "Drive still not ready after %d s - if this never resolves, the drive/media may need "
                             "more power than the USB-A port can supply",
                        (not_ready_polls * POLL_INTERVAL_MS) / 1000
                    );
                }
            } else {
                not_ready_polls = 0;
            }

            // A disc can be swapped without the USB connection ever
            // dropping - the drive signals that via a "unit attention"
            // (medium may have changed) on the next TEST UNIT READY, so
            // re-read the TOC on that signal too, not just on the
            // not-present -> present transition.
            if (disc_present && (!was_disc_present || medium_may_have_changed)) {
                if (medium_may_have_changed && was_disc_present) {
                    ESP_LOGI(TAG, "Medium may have changed, re-reading TOC");
                    cdplayer_stop();
                    cd_metadata_clear();
                }
                // static: too large for cdrom_poll's stack.
                static cdrom_track_t tracks[CDROM_AUDIO_MAX_TRACKS];
                int count = 0;
                esp_err_t toc_res = read_toc(tracks, CDROM_AUDIO_MAX_TRACKS, &count);
                if (toc_res == ESP_OK) {
                    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
                    s_status.disc_present = true;
                    s_status.track_count  = count;
                    memcpy(s_status.tracks, tracks, sizeof(tracks[0]) * count);
                    xSemaphoreGive(s_status_mutex);
                    mark_dirty();
                    ESP_LOGI(TAG, "TOC read: %d track(s)", count);
                    cd_metadata_request_lookup(tracks, count);
                } else {
                    disc_present = false;
                    ESP_LOGW(TAG, "TOC read failed: %s", esp_err_to_name(toc_res));
                }
            } else if (!disc_present && was_disc_present) {
                xSemaphoreTake(s_status_mutex, portMAX_DELAY);
                s_status.disc_present = false;
                s_status.track_count  = 0;
                xSemaphoreGive(s_status_mutex);
                mark_dirty();
                cd_metadata_clear();
            }
            was_disc_present = disc_present;
        } else {
            was_disc_present = false;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t cdrom_audio_init(void) {
    s_status_mutex = xSemaphoreCreateMutex();
    if (s_status_mutex == NULL) return ESP_ERR_NO_MEM;

    esp_err_t ret = bsp_power_set_usb_host_boost_enabled(true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable USB host power boost: %s", esp_err_to_name(ret));
    }

    ret = usb_msc_scsi_start(on_msc_ready, on_msc_gone, NULL);
    if (ret != ESP_OK) return ret;

    xTaskCreate(poll_task, "cdrom_poll", 6144, NULL, 4, NULL);
    return ESP_OK;
}

void cdrom_audio_get_status(cdrom_status_t *out_status) {
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    *out_status = s_status;
    xSemaphoreGive(s_status_mutex);
}

bool cdrom_audio_consume_dirty(void) {
    bool was_dirty = s_dirty;
    s_dirty        = false;
    return was_dirty;
}

esp_err_t cdrom_audio_read_sectors(uint32_t lba, uint16_t sector_count, uint8_t *out_buf) {
    if (out_buf == NULL || sector_count == 0) return ESP_ERR_INVALID_ARG;

    uint8_t cdb[12] = {0};
    cdb[0]  = 0xBE;                              // READ CD
    cdb[1]  = 0x00;                               // Expected Sector Type = all types (this drive rejects the
                                                   // CD-DA-specific value 0x20 with ILLEGAL REQUEST/INVALID FIELD)
    cdb[2]  = (uint8_t)((lba >> 24) & 0xFF);
    cdb[3]  = (uint8_t)((lba >> 16) & 0xFF);
    cdb[4]  = (uint8_t)((lba >> 8) & 0xFF);
    cdb[5]  = (uint8_t)(lba & 0xFF);
    cdb[6]  = (uint8_t)((sector_count >> 16) & 0xFF);
    cdb[7]  = (uint8_t)((sector_count >> 8) & 0xFF);
    cdb[8]  = (uint8_t)(sector_count & 0xFF);
    cdb[9]  = 0x10;                               // User Data only, no sync/header/EDC
    cdb[10] = 0x00;                                // no sub-channel data
    cdb[11] = 0x00;

    uint32_t data_len = (uint32_t)sector_count * CDROM_AUDIO_SECTOR_BYTES;
    esp_err_t ret      = usb_msc_scsi_command(cdb, sizeof(cdb), USB_MSC_SCSI_DIR_IN, out_buf, data_len, 4000);
    if (ret != ESP_OK) {
        uint8_t sense_key = 0;
        ESP_LOGW(TAG, "read_sectors(lba=%" PRIu32 ", count=%u) failed", lba, (unsigned)sector_count);
        fetch_sense("read_sectors", &sense_key);
    }
    return ret;
}

esp_err_t cdrom_audio_eject(void) {
    if (!usb_msc_scsi_is_ready()) return ESP_ERR_INVALID_STATE;

    uint8_t cdb[6] = {0x1B, 0x00, 0x00, 0x00, 0x02, 0x00};  // START STOP UNIT: LoEj=1, Start=0
    esp_err_t ret  = send_scsi_no_data_retrying(cdb, 5000, "eject", NULL);
    if (ret != ESP_OK) {
        uint8_t sense_key = 0;
        fetch_sense("eject", &sense_key);
        return ret;
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.disc_present = false;
    s_status.track_count  = 0;
    xSemaphoreGive(s_status_mutex);
    mark_dirty();
    cdplayer_stop();
    cd_metadata_clear();
    return ESP_OK;
}
#else
esp_err_t cdrom_audio_init(void) {
    return ESP_OK;
}

void cdrom_audio_get_status(cdrom_status_t *out_status) {
    if (out_status != NULL) memset(out_status, 0, sizeof(*out_status));
}

bool cdrom_audio_consume_dirty(void) {
    return false;
}

esp_err_t cdrom_audio_read_sectors(uint32_t lba, uint16_t sector_count, uint8_t *out_buf) {
    (void)lba;
    (void)sector_count;
    (void)out_buf;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cdrom_audio_eject(void) {
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
