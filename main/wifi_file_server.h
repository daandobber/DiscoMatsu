#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_FILE_SERVER_STATE_IDLE = 0,
    WIFI_FILE_SERVER_STATE_CONNECTING_WIFI,
    WIFI_FILE_SERVER_STATE_MOUNTING_SD,
    WIFI_FILE_SERVER_STATE_RUNNING,
    WIFI_FILE_SERVER_STATE_ERROR,
} wifi_file_server_state_t;

typedef struct {
    wifi_file_server_state_t state;
    char url[64];
    char last_error[96];
} wifi_file_server_status_t;

esp_err_t wifi_file_server_init(void);
esp_err_t wifi_file_server_start(void);
void wifi_file_server_stop(void);

void wifi_file_server_get_status(wifi_file_server_status_t *out);
bool wifi_file_server_is_active(void);
bool wifi_file_server_consume_dirty(void);

#ifdef __cplusplus
}
#endif
