#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cdrom_audio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CD_METADATA_STATE_IDLE = 0,
    CD_METADATA_STATE_LOOKING_UP,
    CD_METADATA_STATE_FOUND,
    CD_METADATA_STATE_NOT_FOUND,
} cd_metadata_state_t;

typedef enum {
    CD_METADATA_SEARCH_IDLE = 0,
    CD_METADATA_SEARCH_SEARCHING,
    CD_METADATA_SEARCH_RESULTS,
    CD_METADATA_SEARCH_APPLYING,
    CD_METADATA_SEARCH_ERROR,
} cd_metadata_search_state_t;

typedef struct {
    char id[64];
    char album[96];
    char artist[96];
    char year[8];
    int track_count;
} cd_metadata_search_result_t;

#define CD_METADATA_SEARCH_MAX_RESULTS 6

typedef struct {
    cd_metadata_search_state_t state;
    char query[96];
    char last_error[96];
    int result_count;
    cd_metadata_search_result_t results[CD_METADATA_SEARCH_MAX_RESULTS];
} cd_metadata_search_status_t;

typedef struct {
    cd_metadata_state_t state;
    char album[96];
    char artist[96];
    char year[8];
    int disc_number;
    int disc_count;
    char track_titles[CDROM_AUDIO_MAX_TRACKS][64];
    int track_title_count;
    const uint16_t *cover_art_rgb565;  // NULL if none available
    int cover_art_width;
    int cover_art_height;
} cd_metadata_status_t;

esp_err_t cd_metadata_init(void);

// Starts a background lookup (MusicBrainz + Cover Art Archive) for the
// given track list. Safe to call with no WiFi connection - just results in
// CD_METADATA_STATE_NOT_FOUND.
void cd_metadata_request_lookup(const cdrom_track_t *tracks, int track_count);
void cd_metadata_request_search(const char *query, int current_track_count);
void cd_metadata_apply_search_result(int index, int current_track_count);

// Call when the disc is removed, to clear stale metadata/art.
void cd_metadata_clear(void);

void cd_metadata_get_status(cd_metadata_status_t *out);
void cd_metadata_get_search_status(cd_metadata_search_status_t *out);
bool cd_metadata_consume_dirty(void);

#ifdef __cplusplus
}
#endif
