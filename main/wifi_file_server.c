#include "wifi_file_server.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
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
#define TAR_BLOCK_SIZE 512

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

static void trim_trailing_slashes(char *path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
}

static const char *path_basename(const char *path) {
    const char *name = strrchr(path, '/');
    return name != NULL ? name + 1 : path;
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
        trim_trailing_slashes(path);
        const char *rel = path + strlen(MUSIC_ROOT);
        if (*rel == '/') rel++;
        httpd_resp_sendstr_chunk(req, "<a href='/album/");
        url_encode_send(req, rel);
        httpd_resp_sendstr_chunk(req, "'>Download album (.tar)</a><hr>");
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
    name = name == NULL ? path : name + 1;
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

static void tar_write_octal(char *dst, size_t len, unsigned long value) {
    if (len == 0) return;
    memset(dst, '0', len);
    dst[len - 1] = '\0';
    snprintf(dst, len, "%0*lo", (int)len - 2, value);
    dst[len - 2] = ' ';
}

static void tar_write_checksum(char *dst, unsigned long value) {
    snprintf(dst, 8, "%06lo", value);
    dst[6] = '\0';
    dst[7] = ' ';
}

static void tar_fill_name(char header[TAR_BLOCK_SIZE], const char *name) {
    size_t name_len = strlen(name);
    if (name_len < 100) {
        snprintf(header, 100, "%s", name);
        return;
    }

    const char *split = NULL;
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '/' && (size_t)(p - name) < 155 && strlen(p + 1) < 100) split = p;
    }
    if (split != NULL) {
        memcpy(header, split + 1, strlen(split + 1));
        memcpy(header + 345, name, split - name);
        return;
    }

    const char *base = strrchr(name, '/');
    base = base != NULL ? base + 1 : name;
    snprintf(header, 100, "%.99s", base);
}

static esp_err_t tar_send_header(httpd_req_t *req, const char *name, const struct stat *st, char typeflag) {
    char header[TAR_BLOCK_SIZE];
    memset(header, 0, sizeof(header));
    tar_fill_name(header, name);
    tar_write_octal(header + 100, 8, typeflag == '5' ? 0775 : 0664);
    tar_write_octal(header + 108, 8, 0);
    tar_write_octal(header + 116, 8, 0);
    tar_write_octal(header + 124, 12, typeflag == '5' ? 0 : (unsigned long)st->st_size);
    tar_write_octal(header + 136, 12, (unsigned long)st->st_mtime);
    memset(header + 148, ' ', 8);
    header[156] = typeflag;
    memcpy(header + 257, "ustar", 5);
    memcpy(header + 263, "00", 2);

    unsigned int checksum = 0;
    for (size_t i = 0; i < sizeof(header); i++) checksum += (unsigned char)header[i];
    tar_write_checksum(header + 148, checksum);
    return httpd_resp_send_chunk(req, header, sizeof(header));
}

static esp_err_t tar_send_padding(httpd_req_t *req, size_t size) {
    size_t pad = (TAR_BLOCK_SIZE - (size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
    if (pad == 0) return ESP_OK;
    char zeros[TAR_BLOCK_SIZE] = {0};
    return httpd_resp_send_chunk(req, zeros, pad);
}

static esp_err_t tar_send_file(httpd_req_t *req, const char *path, const char *tar_name, const struct stat *st) {
    esp_err_t res = tar_send_header(req, tar_name, st, '0');
    if (res != ESP_OK) return res;

    FILE *f = fopen(path, "rb");
    if (f == NULL) return ESP_FAIL;

    char *chunk = malloc(FILE_CHUNK_SIZE);
    if (chunk == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read_len;
    while ((read_len = fread(chunk, 1, FILE_CHUNK_SIZE, f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_len) != ESP_OK) {
            free(chunk);
            fclose(f);
            return ESP_FAIL;
        }
    }
    free(chunk);
    fclose(f);
    return tar_send_padding(req, (size_t)st->st_size);
}

static esp_err_t tar_send_dir(httpd_req_t *req, const char *path, const char *tar_name) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return ESP_FAIL;

    char *dir_name = malloc(768);
    if (dir_name == NULL) return ESP_ERR_NO_MEM;
    snprintf(dir_name, 768, "%s/", tar_name);
    esp_err_t res = tar_send_header(req, dir_name, &st, '5');
    free(dir_name);
    if (res != ESP_OK) return res;

    DIR *dir = opendir(path);
    if (dir == NULL) return ESP_FAIL;

    char *child_path = malloc(1024);
    char *child_name = malloc(768);
    if (child_path == NULL || child_name == NULL) {
        free(child_path);
        free(child_name);
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        int path_len = snprintf(child_path, 1024, "%s/%s", path, entry->d_name);
        int name_len = snprintf(child_name, 768, "%s/%s", tar_name, entry->d_name);
        if (path_len < 0 || path_len >= 1024 || name_len < 0 || name_len >= 768) {
            continue;
        }

        struct stat child_st;
        if (stat(child_path, &child_st) != 0) continue;
        if (S_ISDIR(child_st.st_mode)) {
            res = tar_send_dir(req, child_path, child_name);
        } else if (S_ISREG(child_st.st_mode)) {
            res = tar_send_file(req, child_path, child_name, &child_st);
        }
        if (res != ESP_OK) {
            free(child_path);
            free(child_name);
            closedir(dir);
            return res;
        }
    }
    free(child_path);
    free(child_name);
    closedir(dir);
    return ESP_OK;
}

static esp_err_t album_handler(httpd_req_t *req) {
    char path[1024];
    const char *encoded_rel = req->uri + strlen("/album/");
    if (!build_safe_path(encoded_rel, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_OK;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Album not found");
        return ESP_OK;
    }
    trim_trailing_slashes(path);

    const char *name = path_basename(path);
    if (name[0] == '\0') name = "album";

    httpd_resp_set_type(req, "application/x-tar");
    char disposition[280];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%.220s.tar\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    if (tar_send_dir(req, path, name) != ESP_OK) return ESP_OK;
    char zeros[TAR_BLOCK_SIZE * 2] = {0};
    httpd_resp_send_chunk(req, zeros, sizeof(zeros));
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
    config.stack_size = 12288;

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
    httpd_uri_t album = {
        .uri = "/album/*",
        .method = HTTP_GET,
        .handler = album_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &browse));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &file));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &album));

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
