#include "audio_output.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp/audio.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern esp_err_t bsp_audio_initialize(void);

static const char *TAG = "audio_output";

#define I2S_WRITE_CHUNK_BYTES 2048
static const size_t kI2sWriteChunkBytes = I2S_WRITE_CHUNK_BYTES;
static const int kSilenceDrainMs        = 120;

static SemaphoreHandle_t s_session_mutex = NULL;
static i2s_chan_handle_t s_i2s           = NULL;
static bool s_audio_initialized          = false;
static bool s_session_active             = false;
static uint32_t s_sample_rate            = 0;
static int s_volume_percent              = 55;
static char s_last_error[96]             = "";

static void set_last_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, args);
    va_end(args);
}

const char *audio_output_last_error(void) {
    return s_last_error;
}

void audio_output_set_volume_percent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume_percent = percent;
    if (s_audio_initialized) {
        esp_err_t res = bsp_audio_set_volume((float)s_volume_percent);
        if (res != ESP_OK) ESP_LOGW(TAG, "Volume set failed: %s", esp_err_to_name(res));
    }
}

int audio_output_get_volume_percent(void) {
    return s_volume_percent;
}

esp_err_t audio_output_begin(uint32_t sample_rate) {
    set_last_error("");
    if (s_session_mutex == NULL) {
        s_session_mutex = xSemaphoreCreateMutex();
        if (s_session_mutex == NULL) {
            set_last_error("no mem");
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_session_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        set_last_error("audio busy");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t res = bsp_audio_get_i2s_handle(&s_i2s);
    if ((res != ESP_OK || s_i2s == NULL) && !s_audio_initialized) {
        res = bsp_audio_initialize();
        if (res != ESP_OK) {
            set_last_error("audio init %s", esp_err_to_name(res));
            xSemaphoreGive(s_session_mutex);
            return res;
        }
        s_audio_initialized = true;
        res                 = bsp_audio_get_i2s_handle(&s_i2s);
    } else if (res == ESP_OK && s_i2s != NULL) {
        s_audio_initialized = true;
    }
    if (res != ESP_OK || s_i2s == NULL) {
        set_last_error("i2s handle %s", esp_err_to_name(res));
        xSemaphoreGive(s_session_mutex);
        return res == ESP_OK ? ESP_FAIL : res;
    }

    if (s_sample_rate != sample_rate) {
        esp_err_t disable_res = i2s_channel_disable(s_i2s);
        if (disable_res != ESP_OK && disable_res != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "I2S disable before rate change failed: %s", esp_err_to_name(disable_res));
        }
        res = bsp_audio_set_rate(sample_rate);
        if (res != ESP_OK) {
            set_last_error("rate %s", esp_err_to_name(res));
            xSemaphoreGive(s_session_mutex);
            return res;
        }
        s_sample_rate = sample_rate;
    }

    res = i2s_channel_enable(s_i2s);
    if (res != ESP_OK && res != ESP_ERR_INVALID_STATE) {
        set_last_error("i2s enable %s", esp_err_to_name(res));
        xSemaphoreGive(s_session_mutex);
        return res;
    }

    esp_err_t vol_res = bsp_audio_set_volume((float)s_volume_percent);
    if (vol_res != ESP_OK) ESP_LOGW(TAG, "Volume set failed: %s", esp_err_to_name(vol_res));
    esp_err_t amp_res = bsp_audio_set_amplifier(true);
    if (amp_res != ESP_OK) ESP_LOGW(TAG, "Amplifier enable failed: %s", esp_err_to_name(amp_res));

    s_session_active = true;
    // s_session_mutex stays held for the whole begin..end streaming session.
    return ESP_OK;
}

// Headroom reduction applied to every sample before it reaches the DAC/amp.
// Content at or near 0 dBFS (CD audio masters in particular) otherwise
// clips/distorts on this hardware chain regardless of the volume setting -
// the original MatrixMatsu audio path applied the same -6dB reduction.
static const int kPcmGainNumerator   = 1;
static const int kPcmGainDenominator = 2;

esp_err_t audio_output_write(const int16_t *stereo_pcm, size_t frames) {
    if (!s_session_active || s_i2s == NULL || stereo_pcm == NULL || frames == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    static int16_t scratch[I2S_WRITE_CHUNK_BYTES / sizeof(int16_t)];
    size_t total_samples   = frames * 2;
    size_t samples_written = 0;
    while (samples_written < total_samples) {
        size_t batch = total_samples - samples_written;
        if (batch > (kI2sWriteChunkBytes / sizeof(int16_t))) batch = kI2sWriteChunkBytes / sizeof(int16_t);

        for (size_t i = 0; i < batch; i++) {
            int32_t scaled  = ((int32_t)stereo_pcm[samples_written + i] * kPcmGainNumerator) / kPcmGainDenominator;
            scratch[i]      = (int16_t)scaled;
        }

        size_t to_write = batch * sizeof(int16_t);
        size_t written  = 0;
        esp_err_t res   = i2s_channel_write(s_i2s, scratch, to_write, &written, portMAX_DELAY);
        if (res != ESP_OK || written == 0) {
            set_last_error(res == ESP_OK ? "i2s no progress" : "i2s write %s", esp_err_to_name(res));
            return res == ESP_OK ? ESP_FAIL : res;
        }
        samples_written += written / sizeof(int16_t);
    }
    return ESP_OK;
}

void audio_output_end(void) {
    if (!s_session_active) return;

    if (s_i2s != NULL) {
        int16_t silence[480 * 2] = {0};
        const int blocks = kSilenceDrainMs / 10;
        for (int i = 0; i < blocks; i++) {
            if (audio_output_write(silence, 480) != ESP_OK) break;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    esp_err_t amp_res = bsp_audio_set_amplifier(false);
    if (amp_res != ESP_OK) ESP_LOGW(TAG, "Amplifier disable failed: %s", esp_err_to_name(amp_res));

    s_session_active = false;
    xSemaphoreGive(s_session_mutex);
}
