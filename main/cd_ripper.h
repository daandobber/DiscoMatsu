#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cd_metadata.h"
#include "cdrom_audio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CD_RIPPER_STATE_IDLE = 0,
    CD_RIPPER_STATE_MOUNTING_SD,
    CD_RIPPER_STATE_RIPPING,
    CD_RIPPER_STATE_DONE,
    CD_RIPPER_STATE_ERROR,
} cd_ripper_state_t;

typedef struct {
    cd_ripper_state_t state;
    int current_track;
    int total_tracks;
    uint32_t current_percent;
    char current_path[768];
    char last_error[96];
} cd_ripper_status_t;

esp_err_t cd_ripper_init(void);
esp_err_t cd_ripper_start(const cdrom_status_t *disc, const cd_metadata_status_t *metadata);

void cd_ripper_get_status(cd_ripper_status_t *out);
bool cd_ripper_is_active(void);
bool cd_ripper_consume_dirty(void);

#ifdef __cplusplus
}
#endif
