#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Begins a streaming PCM playback session at the given sample rate (44100
// for CD-DA). Takes ownership of the I2S output until audio_output_end() is
// called - only one streaming session can be active at a time.
esp_err_t audio_output_begin(uint32_t sample_rate);

// Blocks until all frames have been written to I2S. stereo_pcm must contain
// frames * 2 interleaved 16-bit samples (left, right).
esp_err_t audio_output_write(const int16_t *stereo_pcm, size_t frames);

// Drains a short silence tail, disables the amplifier, and ends the session.
void audio_output_end(void);

void audio_output_set_volume_percent(int percent);
int audio_output_get_volume_percent(void);

const char *audio_output_last_error(void);

#ifdef __cplusplus
}
#endif
