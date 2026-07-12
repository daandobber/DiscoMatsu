#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool configured;
    bool has_session;
    bool enabled;
    char username[64];
    char last_error[96];
} lastfm_status_t;

typedef struct {
    const char *artist;
    const char *album;
    const char *track;
    int track_number;
    uint32_t duration_sec;
} lastfm_track_info_t;

esp_err_t lastfm_scrobbler_init(void);

// Optional first-run account bootstrap. On success only the Last.fm session
// key is retained; the password is not stored.
esp_err_t lastfm_scrobbler_login(const char *username, const char *password);

void lastfm_scrobbler_now_playing(const lastfm_track_info_t *track);
void lastfm_scrobbler_maybe_scrobble(const lastfm_track_info_t *track, uint32_t elapsed_sec);
void lastfm_scrobbler_track_changed(void);

void lastfm_scrobbler_get_status(lastfm_status_t *out);
bool lastfm_scrobbler_consume_dirty(void);

#ifdef __cplusplus
}
#endif
