#include "lastfm_config_file.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_log.h"
#include "lastfm_scrobbler.h"
#include "sd_storage.h"

#define LASTFM_CONFIG_FILE_PATH "/sd/discomatsu/config.json"
#define LASTFM_CONFIG_MAX_SIZE  4096

static const char *TAG = "lastfm_config";

static bool nonempty_json_string(const cJSON *item) {
    return cJSON_IsString(item) && item->valuestring != NULL && item->valuestring[0] != '\0';
}

esp_err_t lastfm_config_file_load(void) {
    lastfm_status_t status;
    lastfm_scrobbler_get_status(&status);
    if (status.has_session) {
        ESP_LOGI(TAG, "Stored Last.fm session found, SD config not needed");
        return ESP_OK;
    }

    esp_err_t res = sd_storage_mount();
    if (res != ESP_OK) {
        ESP_LOGI(TAG, "SD card unavailable, skipping optional Last.fm config");
        return res;
    }

    FILE *file = fopen(LASTFM_CONFIG_FILE_PATH, "rb");
    if (file == NULL) {
        ESP_LOGI(TAG, "%s not found, skipping", LASTFM_CONFIG_FILE_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long size = ftell(file);
    if (size <= 0 || size > LASTFM_CONFIG_MAX_SIZE || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        ESP_LOGW(TAG, "config.json ignored: bad size (%ld bytes)", size);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buffer = malloc((size_t)size + 1);
    if (buffer == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    size_t bytes_read = fread(buffer, 1, (size_t)size, file);
    bool read_ok = bytes_read == (size_t)size && ferror(file) == 0;
    fclose(file);
    if (!read_ok) {
        free(buffer);
        ESP_LOGW(TAG, "config.json ignored: could not read complete file");
        return ESP_FAIL;
    }
    buffer[bytes_read] = '\0';

    // Accept files saved by editors that add a UTF-8 byte-order mark.
    char *json_start = buffer;
    if (bytes_read >= 3 && (uint8_t)buffer[0] == 0xEF && (uint8_t)buffer[1] == 0xBB &&
        (uint8_t)buffer[2] == 0xBF) {
        json_start += 3;
    }

    cJSON *root = cJSON_Parse(json_start);
    free(buffer);
    if (root == NULL) {
        ESP_LOGW(TAG, "config.json ignored: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *lastfm     = cJSON_GetObjectItem(root, "lastfm");
    cJSON *api_key    = cJSON_IsObject(lastfm) ? cJSON_GetObjectItem(lastfm, "api_key") : NULL;
    cJSON *api_secret = cJSON_IsObject(lastfm) ? cJSON_GetObjectItem(lastfm, "api_secret") : NULL;
    cJSON *username   = cJSON_IsObject(lastfm) ? cJSON_GetObjectItem(lastfm, "username") : NULL;
    cJSON *password   = cJSON_IsObject(lastfm) ? cJSON_GetObjectItem(lastfm, "password") : NULL;

    if (!cJSON_IsObject(lastfm)) {
        ESP_LOGW(TAG, "config.json ignored: missing lastfm object");
        res = ESP_ERR_INVALID_ARG;
        goto done;
    }

    bool has_file_api = nonempty_json_string(api_key) && nonempty_json_string(api_secret);
    if (has_file_api) {
        res = lastfm_scrobbler_set_api_credentials(api_key->valuestring, api_secret->valuestring);
        if (res != ESP_OK) goto done;
    } else if (!status.has_api_key || !status.has_api_secret) {
        ESP_LOGW(TAG, "config.json ignored: missing api_key or api_secret");
        res = ESP_ERR_INVALID_ARG;
        goto done;
    }

    if (!nonempty_json_string(username) || !nonempty_json_string(password)) {
        ESP_LOGW(TAG, "config.json ignored: missing username or password");
        res = ESP_ERR_INVALID_ARG;
        goto done;
    }

    res = lastfm_scrobbler_login(username->valuestring, password->valuestring);
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "Last.fm login requested using %s", LASTFM_CONFIG_FILE_PATH);
    }

done:
    cJSON_Delete(root);
    return res;
}
