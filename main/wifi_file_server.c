#include "wifi_file_server.h"

#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_storage.h"
#include "wifi_setup.h"

static const char *TAG = "wifi_file_server";

#define MUSIC_ROOT SD_STORAGE_MOUNT_POINT "/Music"
#define FILE_CHUNK_SIZE 2048

static SemaphoreHandle_t s_mutex = NULL;
static wifi_file_server_status_t s_status = {0};
static volatile bool s_dirty = false;
static httpd_handle_t s_server = NULL;
static TaskHandle_t s_start_task = NULL;

static void publish_status(wifi_file_server_state_t state, const char *url, const char *error) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.state = state;
    if (url != NULL) {
        snprintf(s_status.url, sizeof(s_status.url), "%s", url);
    } else if (state != WIFI_FILE_SERVER_STATE_RUNNING) {
        s_status.url[0] = '\0';
    }
    if (error != NULL) {
        snprintf(s_status.last_error, sizeof(s_status.last_error), "%s", error);
    } else if (state != WIFI_FILE_SERVER_STATE_ERROR) {
        s_status.last_error[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
    s_dirty = true;
}

static const char *content_type_for_path(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot == NULL) return "application/octet-stream";
    if (strcasecmp(dot, ".wav") == 0) return "audio/wav";
    if (strcasecmp(dot, ".txt") == 0) return "text/plain";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    return "application/octet-stream";
}

static void html_escape_send(httpd_req_t *req, const char *text) {
    for (const char *p = text; *p != '\0'; p++) {
        switch (*p) {
            case '&': httpd_resp_sendstr_chunk(req, "&amp;"); break;
            case '<': httpd_resp_sendstr_chunk(req, "&lt;"); break;
            case '>': httpd_resp_sendstr_chunk(req, "&gt;"); break;
            case '"': httpd_resp_sendstr_chunk(req, "&quot;"); break;
            default: {
                char c[2] = {*p, '\0'};
                httpd_resp_sendstr_chunk(req, c);
                break;
            }
        }
    }
}

static void url_encode_send(httpd_req_t *req, const char *text) {
    static const char hex[] = "0123456789ABCDEF";
    char out[4];
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '/') {
            out[0] = (char)*p;
            out[1] = '\0';
        } else {
            out[0] = '%';
            out[1] = hex[*p >> 4];
            out[2] = hex[*p & 0x0F];
            out[3] = '\0';
        }
        httpd_resp_sendstr_chunk(req, out);
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool url_decode(const char *in, char *out, size_t out_len) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        if (o + 1 >= out_len) return false;
        if (in[i] == '%' && in[i + 1] != '\0' && in[i + 2] != '\0') {
            int hi = hex_value(in[i + 1]);
            int lo = hex_value(in[i + 2]);
            if (hi < 0 || lo < 0) return false;
            out[o++] = (char)((hi << 4) | lo);
            i += 2;
        } else if (in[i] == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
    return true;
}

static bool build_safe_path(const char *encoded_rel, char *out, size_t out_len) {
    char rel[768];
    if (!url_decode(encoded_rel, rel, sizeof(rel))) return false;
    if (rel[0] == '/') memmove(rel, rel + 1, strlen(rel));
    if (strstr(rel, "..") != NULL || strchr(rel, '\\') != NULL) return false;
    if (rel[0] == '\0') {
        snprintf(out, out_len, "%s", MUSIC_ROOT);
    } else {
        snprintf(out, out_len, "%s/%s", MUSIC_ROOT, rel);
    }
    return true;
}

static esp_err_t list_handler(httpd_req_t *req) {
    char path[1024];
    const char *encoded_rel = req->uri;
    if (strncmp(encoded_rel, "/browse", 7) == 0) encoded_rel += 7;
    if (!build_safe_path(encoded_rel, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_OK;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Folder not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(
        req,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Disc-O-Matsu files</title>"
        "<style>body{font-family:system-ui,sans-serif;margin:24px;line-height:1.4}"
        "a{display:block;padding:8px 0;color:#0645ad}small{color:#666}</style>"
        "<h1>Disc-O-Matsu</h1><small>/sd/Music</small><hr>"
    );

    if (strcmp(path, MUSIC_ROOT) != 0) {
        httpd_resp_sendstr_chunk(req, "<a href='/browse/'>../</a>");
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char full[1024];
        int full_len = snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        if (full_len < 0 || full_len >= (int)sizeof(full)) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;

        const char *rel = full + strlen(MUSIC_ROOT);
        if (*rel == '/') rel++;
        httpd_resp_sendstr_chunk(req, "<a href='");
        httpd_resp_sendstr_chunk(req, S_ISDIR(st.st_mode) ? "/browse/" : "/file/");
        url_encode_send(req, rel);
        httpd_resp_sendstr_chunk(req, S_ISDIR(st.st_mode) ? "/'>" : "'>");
        html_escape_send(req, entry->d_name);
        if (S_ISDIR(st.st_mode)) {
            httpd_resp_sendstr_chunk(req, "/");
        } else {
            char size[48];
            snprintf(size, sizeof(size), " <small>%ld MB</small>", (long)((st.st_size + 1024 * 1024 - 1) / (1024 * 1024)));
            httpd_resp_sendstr_chunk(req, size);
        }
        httpd_resp_sendstr_chunk(req, "</a>");
    }
    closedir(dir);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t file_handler(httpd_req_t *req) {
    char path[1024];
    const char *encoded_rel = req->uri + strlen("/file/");
    if (!build_safe_path(encoded_rel, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_OK;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, content_type_for_path(path));
    const char *name = strrchr(path, '/');
    if (name == NULL) name = path;
    else name++;
    char disposition[256];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%.220s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    char *chunk = malloc(FILE_CHUNK_SIZE);
    if (chunk == NULL) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    size_t read_len;
    while ((read_len = fread(chunk, 1, FILE_CHUNK_SIZE, f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_len) != ESP_OK) break;
    }
    free(chunk);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static bool get_ip_url(char *out, size_t out_len) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            snprintf(out, out_len, "http://" IPSTR "/", IP2STR(&ip.ip));
            return true;
        }
    }
    snprintf(out, out_len, "http://<device-ip>/");
    return false;
}

static esp_err_t start_http_server(char *url, size_t url_len) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = 4;
    config.stack_size = 8192;

    esp_err_t res = httpd_start(&s_server, &config);
    if (res != ESP_OK) return res;

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = list_handler,
    };
    httpd_uri_t browse = {
        .uri = "/browse/*",
        .method = HTTP_GET,
        .handler = list_handler,
    };
    httpd_uri_t file = {
        .uri = "/file/*",
        .method = HTTP_GET,
        .handler = file_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &browse));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &file));

    get_ip_url(url, url_len);
    return ESP_OK;
}

static void start_task(void *arg) {
    (void)arg;
    publish_status(WIFI_FILE_SERVER_STATE_CONNECTING_WIFI, NULL, NULL);
    if (!wifi_setup_connect_blocking(20000)) {
        publish_status(WIFI_FILE_SERVER_STATE_ERROR, NULL, "WiFi connect failed");
        s_start_task = NULL;
        vTaskDelete(NULL);
    }

    publish_status(WIFI_FILE_SERVER_STATE_MOUNTING_SD, NULL, NULL);
    esp_err_t res = sd_storage_mount();
    if (res != ESP_OK) {
        char error[96];
        snprintf(error, sizeof(error), "SD mount failed: %s", esp_err_to_name(res));
        wifi_setup_disconnect();
        publish_status(WIFI_FILE_SERVER_STATE_ERROR, NULL, error);
        s_start_task = NULL;
        vTaskDelete(NULL);
    }

    mkdir(MUSIC_ROOT, 0775);
    char url[64];
    res = start_http_server(url, sizeof(url));
    if (res != ESP_OK) {
        char error[96];
        snprintf(error, sizeof(error), "HTTP start failed: %s", esp_err_to_name(res));
        wifi_setup_disconnect();
        publish_status(WIFI_FILE_SERVER_STATE_ERROR, NULL, error);
        s_start_task = NULL;
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "File server running at %s", url);
    publish_status(WIFI_FILE_SERVER_STATE_RUNNING, url, NULL);
    s_start_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t wifi_file_server_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t wifi_file_server_start(void) {
    if (wifi_file_server_is_active() || s_start_task != NULL) return ESP_ERR_INVALID_STATE;
    if (xTaskCreate(start_task, "wifi_files", 8192, NULL, 4, &s_start_task) != pdPASS) {
        s_start_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void wifi_file_server_stop(void) {
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    wifi_setup_disconnect();
    publish_status(WIFI_FILE_SERVER_STATE_IDLE, NULL, NULL);
}

void wifi_file_server_get_status(wifi_file_server_status_t *out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mutex);
}

bool wifi_file_server_is_active(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool active = s_status.state == WIFI_FILE_SERVER_STATE_CONNECTING_WIFI ||
                  s_status.state == WIFI_FILE_SERVER_STATE_MOUNTING_SD ||
                  s_status.state == WIFI_FILE_SERVER_STATE_RUNNING;
    xSemaphoreGive(s_mutex);
    return active;
}

bool wifi_file_server_consume_dirty(void) {
    bool was_dirty = s_dirty;
    s_dirty = false;
    return was_dirty;
}
