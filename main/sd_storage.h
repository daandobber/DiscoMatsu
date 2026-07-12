#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_STORAGE_MOUNT_POINT "/sd"

esp_err_t sd_storage_mount(void);
bool sd_storage_is_mounted(void);

#ifdef __cplusplus
}
#endif
