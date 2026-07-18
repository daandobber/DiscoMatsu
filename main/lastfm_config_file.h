#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Optionally provisions Last.fm from /sd/discomatsu/config.json when no
// session is stored in NVS yet. Existing in-app/NVS settings take priority.
esp_err_t lastfm_config_file_load(void);

#ifdef __cplusplus
}
#endif
