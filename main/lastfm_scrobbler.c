#include "lastfm_scrobbler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md5.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "wifi_setup.h"

#define LASTFM_API_URL "https://ws.audioscrobbler.com/2.0/"
#define LASTFM_HTTP_CAP (32 * 1024)
#define LASTFM_SCROBBLE_AFTER_SEC 30

#ifndef CONFIG_DISCOMATSU_LASTFM_API_KEY
#define CONFIG_DISCOMATSU_LASTFM_API_KEY ""
#endif
#ifndef CONFIG_DISCOMATSU_LASTFM_API_SECRET
#define CONFIG_DISCOMATSU_LASTFM_API_SECRET ""
#endif
#ifndef CONFIG_DISCOMATSU_LASTFM_BOOTSTRAP_USERNAME
#define CONFIG_DISCOMATSU_LASTFM_BOOTSTRAP_USERNAME ""
#endif
#ifndef CONFIG_DISCOMATSU_LASTFM_BOOTSTRAP_PASSWORD
#define CONFIG_DISCOMATSU_LASTFM_BOOTSTRAP_PASSWORD ""
#endif

typedef struct {
    char method[32];
    char artist[96];
    char album[96];
    char track[96];
    char username[64];
    char password[96];
    char session_key[64];
    int track_number;
    uint32_t duration_sec;
    uint32_t timestamp;
} lastfm_request_t;

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} http_recv_ctx_t;

static SemaphoreHandle_t s_mutex = NULL;
static char s_session_key[64]    = "";
static char s_username[64]       = "";
static char s_last_error[96]     = "";
static bool s_scrobbled_current  = false;
static volatile bool s_dirty     = false;

static bool api_configured(void) {
    return CONFIG_DISCOMATSU_LASTFM_API_KEY[0] != '\0' && CONFIG_DISCOMATSU_LASTFM_API_SECRET[0] != '\0';
}

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, args);
    xSemaphoreGive(s_mutex);
    va_end(args);
    s_dirty = true;
}

static void set_session(const char *username, const char *session_key) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_username, sizeof(s_username), "%s", username ? username : "");
    snprintf(s_session_key, sizeof(s_session_key), "%s", session_key ? session_key : "");
    s_last_error[0] = '\0';
    xSemaphoreGive(s_mutex);
    s_dirty = true;
}

static esp_err_t load_session(void) {
    nvs_handle_t nvs;
    esp_err_t res = nvs_open("lastfm", NVS_READONLY, &nvs);
    if (res != ESP_OK) return res;

    char username[64] = "";
    char session_key[64] = "";
    size_t username_len = sizeof(username);
    size_t session_len = sizeof(session_key);
    esp_err_t user_res = nvs_get_str(nvs, "username", username, &username_len);
    esp_err_t key_res = nvs_get_str(nvs, "session", session_key, &session_len);
    nvs_close(nvs);

    if (user_res == ESP_OK && key_res == ESP_OK) {
        set_session(username, session_key);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t save_session(const char *username, const char *session_key) {
    nvs_handle_t nvs;
    esp_err_t res = nvs_open("lastfm", NVS_READWRITE, &nvs);
    if (res != ESP_OK) return res;
    res = nvs_set_str(nvs, "username", username);
    if (res == ESP_OK) res = nvs_set_str(nvs, "session", session_key);
    if (res == ESP_OK) res = nvs_commit(nvs);
    nvs_close(nvs);
    if (res == ESP_OK) set_session(username, session_key);
    return res;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        http_recv_ctx_t *ctx = (http_recv_ctx_t *)evt->user_data;
        if (ctx->len + (size_t)evt->data_len <= ctx->cap) {
            memcpy(ctx->buf + ctx->len, evt->data, (size_t)evt->data_len);
            ctx->len += (size_t)evt->data_len;
        }
    }
    return ESP_OK;
}

static esp_err_t http_post_form(const char *body, uint8_t *out_buf, size_t cap, size_t *out_len, int *out_status) {
    http_recv_ctx_t ctx = {.buf = out_buf, .len = 0, .cap = cap};
    esp_http_client_config_t config = {
        .url = LASTFM_API_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_FAIL;

    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_header(client, "User-Agent", "Disc-O-Matsu/0.1");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        *out_status = esp_http_client_get_status_code(client);
        *out_len = ctx.len;
    }
    esp_http_client_cleanup(client);
    return err;
}

static int append_urlenc(char *out, size_t cap, size_t pos, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; s != NULL && s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '_' || c == '.' || c == '~';
        if (safe) {
            if (pos + 1 >= cap) return -1;
            out[pos++] = (char)c;
        } else {
            if (pos + 3 >= cap) return -1;
            out[pos++] = '%';
            out[pos++] = hex[c >> 4];
            out[pos++] = hex[c & 0x0F];
        }
    }
    if (pos >= cap) return -1;
    out[pos] = '\0';
    return (int)pos;
}

static void md5_hex(const char *input, char out[33]) {
    unsigned char digest[16];
    mbedtls_md5((const unsigned char *)input, strlen(input), digest);
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
    out[32] = '\0';
}

static void sign_auth(const char *username, const char *password, char out[33]) {
    char sig_src[384];
    snprintf(
        sig_src, sizeof(sig_src), "api_key%smethodauth.getMobileSessionpassword%susername%s%s",
        CONFIG_DISCOMATSU_LASTFM_API_KEY, password, username, CONFIG_DISCOMATSU_LASTFM_API_SECRET
    );
    md5_hex(sig_src, out);
}

static void sign_track(const lastfm_request_t *req, char out[33]) {
    char sig_src[768];
    if (strcmp(req->method, "track.scrobble") == 0) {
        snprintf(
            sig_src, sizeof(sig_src),
            "album%sapi_key%sartist%sduration%umethodtrack.scrobblesk%stimestamp%utrack%strackNumber%d%s",
            req->album, CONFIG_DISCOMATSU_LASTFM_API_KEY, req->artist, (unsigned)req->duration_sec, req->session_key,
            (unsigned)req->timestamp, req->track, req->track_number, CONFIG_DISCOMATSU_LASTFM_API_SECRET
        );
    } else {
        snprintf(
            sig_src, sizeof(sig_src),
            "album%sapi_key%sartist%sduration%umethodtrack.updateNowPlayingsk%strack%strackNumber%d%s",
            req->album, CONFIG_DISCOMATSU_LASTFM_API_KEY, req->artist, (unsigned)req->duration_sec, req->session_key,
            req->track, req->track_number, CONFIG_DISCOMATSU_LASTFM_API_SECRET
        );
    }
    md5_hex(sig_src, out);
}

static int append_pair(char *body, size_t cap, size_t pos, const char *key, const char *value) {
    int next = snprintf(body + pos, cap - pos, "%s%s=", pos > 0 ? "&" : "", key);
    if (next < 0 || pos + (size_t)next >= cap) return -1;
    pos += (size_t)next;
    return append_urlenc(body, cap, pos, value);
}

static int append_pair_u32(char *body, size_t cap, size_t pos, const char *key, uint32_t value) {
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%u", (unsigned)value);
    return append_pair(body, cap, pos, key, tmp);
}

static void request_task(void *arg) {
    lastfm_request_t *req = (lastfm_request_t *)arg;
    uint8_t *buf = heap_caps_malloc(LASTFM_HTTP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        set_error("no http buffer");
        free(req);
        vTaskDelete(NULL);
        return;
    }

    if (!wifi_setup_connect_blocking(15000)) {
        set_error("no wifi");
        goto done;
    }

    char api_sig[33];
    char body[1400];
    int pos = 0;

    if (strcmp(req->method, "auth.getMobileSession") == 0) {
        sign_auth(req->username, req->password, api_sig);
        pos = append_pair(body, sizeof(body), pos, "method", "auth.getMobileSession");
        pos = append_pair(body, sizeof(body), pos, "username", req->username);
        pos = append_pair(body, sizeof(body), pos, "password", req->password);
        pos = append_pair(body, sizeof(body), pos, "api_key", CONFIG_DISCOMATSU_LASTFM_API_KEY);
        pos = append_pair(body, sizeof(body), pos, "api_sig", api_sig);
        pos = append_pair(body, sizeof(body), pos, "format", "json");
    } else {
        sign_track(req, api_sig);
        pos = append_pair(body, sizeof(body), pos, "method", req->method);
        pos = append_pair(body, sizeof(body), pos, "artist", req->artist);
        pos = append_pair(body, sizeof(body), pos, "track", req->track);
        pos = append_pair(body, sizeof(body), pos, "album", req->album);
        pos = append_pair_u32(body, sizeof(body), pos, "duration", req->duration_sec);
        pos = append_pair_u32(body, sizeof(body), pos, "trackNumber", (uint32_t)req->track_number);
        if (strcmp(req->method, "track.scrobble") == 0) {
            pos = append_pair_u32(body, sizeof(body), pos, "timestamp", req->timestamp);
        }
        pos = append_pair(body, sizeof(body), pos, "api_key", CONFIG_DISCOMATSU_LASTFM_API_KEY);
        pos = append_pair(body, sizeof(body), pos, "sk", req->session_key);
        pos = append_pair(body, sizeof(body), pos, "api_sig", api_sig);
        pos = append_pair(body, sizeof(body), pos, "format", "json");
    }
    if (pos < 0) {
        set_error("request too large");
        goto done_wifi;
    }

    size_t len = 0;
    int status = 0;
    esp_err_t res = http_post_form(body, buf, LASTFM_HTTP_CAP, &len, &status);
    if (res != ESP_OK || status != 200) {
        set_error("http %s/%d", esp_err_to_name(res), status);
        goto done_wifi;
    }

    if (strcmp(req->method, "auth.getMobileSession") == 0) {
        cJSON *root = cJSON_ParseWithLength((const char *)buf, len);
        cJSON *session = root ? cJSON_GetObjectItem(root, "session") : NULL;
        cJSON *key = session ? cJSON_GetObjectItem(session, "key") : NULL;
        if (cJSON_IsString(key) && key->valuestring[0] != '\0') {
            esp_err_t save_res = save_session(req->username, key->valuestring);
            if (save_res != ESP_OK) set_error("nvs %s", esp_err_to_name(save_res));
        } else {
            set_error("auth rejected");
        }
        if (root) cJSON_Delete(root);
    } else {
        set_error("");
    }

done_wifi:
    wifi_setup_disconnect();
done:
    heap_caps_free(buf);
    free(req);
    vTaskDelete(NULL);
}

static bool fill_track_request(lastfm_request_t *req, const char *method, const lastfm_track_info_t *track) {
    if (!api_configured() || track == NULL || track->artist == NULL || track->track == NULL) return false;
    if (track->artist[0] == '\0' || track->track[0] == '\0') return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = s_session_key[0] != '\0';
    snprintf(req->session_key, sizeof(req->session_key), "%s", s_session_key);
    xSemaphoreGive(s_mutex);
    if (!ok) return false;

    snprintf(req->method, sizeof(req->method), "%s", method);
    snprintf(req->artist, sizeof(req->artist), "%s", track->artist);
    snprintf(req->album, sizeof(req->album), "%s", track->album ? track->album : "");
    snprintf(req->track, sizeof(req->track), "%s", track->track);
    req->track_number = track->track_number;
    req->duration_sec = track->duration_sec;
    req->timestamp = (uint32_t)time(NULL);
    return true;
}

esp_err_t lastfm_scrobbler_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;

    load_session();
    if (api_configured() && s_session_key[0] == '\0' && CONFIG_DISCOMATSU_LASTFM_BOOTSTRAP_USERNAME[0] != '\0' &&
        CONFIG_DISCOMATSU_LASTFM_BOOTSTRAP_PASSWORD[0] != '\0') {
        return lastfm_scrobbler_login(
            CONFIG_DISCOMATSU_LASTFM_BOOTSTRAP_USERNAME, CONFIG_DISCOMATSU_LASTFM_BOOTSTRAP_PASSWORD
        );
    }
    return ESP_OK;
}

esp_err_t lastfm_scrobbler_login(const char *username, const char *password) {
    if (!api_configured() || username == NULL || password == NULL || username[0] == '\0' || password[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    lastfm_request_t *req = calloc(1, sizeof(lastfm_request_t));
    if (req == NULL) return ESP_ERR_NO_MEM;
    snprintf(req->method, sizeof(req->method), "auth.getMobileSession");
    snprintf(req->username, sizeof(req->username), "%s", username);
    snprintf(req->password, sizeof(req->password), "%s", password);
    if (xTaskCreate(request_task, "lastfm_auth", 12288, req, 3, NULL) != pdPASS) {
        free(req);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void lastfm_scrobbler_now_playing(const lastfm_track_info_t *track) {
    lastfm_request_t *req = calloc(1, sizeof(lastfm_request_t));
    if (req == NULL) return;
    if (!fill_track_request(req, "track.updateNowPlaying", track)) {
        free(req);
        return;
    }
    xTaskCreate(request_task, "lastfm_now", 12288, req, 3, NULL);
}

void lastfm_scrobbler_maybe_scrobble(const lastfm_track_info_t *track, uint32_t elapsed_sec) {
    if (s_scrobbled_current || track == NULL) return;
    uint32_t threshold = track->duration_sec / 2;
    if (threshold > 240) threshold = 240;
    if (threshold < LASTFM_SCROBBLE_AFTER_SEC) threshold = LASTFM_SCROBBLE_AFTER_SEC;
    if (elapsed_sec < threshold) return;

    lastfm_request_t *req = calloc(1, sizeof(lastfm_request_t));
    if (req == NULL) return;
    if (!fill_track_request(req, "track.scrobble", track)) {
        free(req);
        return;
    }
    if (req->timestamp < 1600000000u) {
        free(req);
        return;
    }
    // Last.fm wants the start time, not the time we crossed the threshold.
    if (req->timestamp > elapsed_sec) req->timestamp -= elapsed_sec;
    s_scrobbled_current = true;
    xTaskCreate(request_task, "lastfm_scrob", 12288, req, 3, NULL);
}

void lastfm_scrobbler_track_changed(void) {
    s_scrobbled_current = false;
}

void lastfm_scrobbler_get_status(lastfm_status_t *out) {
    if (out == NULL) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(out, 0, sizeof(*out));
    out->configured = api_configured();
    out->has_session = s_session_key[0] != '\0';
    out->enabled = out->configured && out->has_session;
    snprintf(out->username, sizeof(out->username), "%s", s_username);
    snprintf(out->last_error, sizeof(out->last_error), "%s", s_last_error);
    xSemaphoreGive(s_mutex);
}

bool lastfm_scrobbler_consume_dirty(void) {
    bool was_dirty = s_dirty;
    s_dirty = false;
    return was_dirty;
}
