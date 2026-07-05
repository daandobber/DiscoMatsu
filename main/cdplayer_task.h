#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool playing;
    bool paused;
    int track_index;  // index into the cdrom_audio track list, -1 if nothing loaded
    uint32_t elapsed_sec;
    uint32_t total_sec;
} cdplayer_state_t;

// Spawns the playback task. Call once at startup.
esp_err_t cdplayer_task_init(void);

// Starts playing the given audio-track index (from cdrom_audio's track
// list), stopping whatever was playing first. No-op if the index is out of
// range or is not an audio track.
void cdplayer_play_track(int track_index);

void cdplayer_stop(void);
void cdplayer_pause(bool pause);

// Skip to the next/previous audio track, if one exists.
void cdplayer_next(void);
void cdplayer_prev(void);

void cdplayer_get_state(cdplayer_state_t *out_state);

// True (and clears the flag) if playback state has changed since the last
// call - used by the UI main loop to decide whether to redraw.
bool cdplayer_consume_dirty(void);

#ifdef __cplusplus
}
#endif
