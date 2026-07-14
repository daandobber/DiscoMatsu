#include "wifi_file_server.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
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
#define COVER_HTTP_BUF_SIZE (128 * 1024)
#define USER_AGENT "Disc-O-Matsu/0.1"

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

static int url_encode_to_buf(char *out, size_t out_len, const char *text) {
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (pos + 1 >= out_len) return -1;
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[pos++] = (char)*p;
        } else {
            if (pos + 3 >= out_len) return -1;
            out[pos++] = '%';
            out[pos++] = hex[*p >> 4];
            out[pos++] = hex[*p & 0x0F];
        }
    }
    out[pos] = '\0';
    return (int)pos;
}

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
} http_capture_t;

static esp_err_t capture_http_event(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0) return ESP_OK;
    http_capture_t *capture = (http_capture_t *)evt->user_data;
    if (capture == NULL || capture->len + (size_t)evt->data_len > capture->cap) return ESP_FAIL;
    memcpy(capture->buf + capture->len, evt->data, (size_t)evt->data_len);
    capture->len += (size_t)evt->data_len;
    return ESP_OK;
}

static esp_err_t http_get_capture(const char *url, uint8_t *buf, size_t cap, size_t *out_len, int *out_status) {
    http_capture_t capture = {.buf = buf, .cap = cap, .len = 0};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = capture_http_event,
        .user_data = &capture,
        .user_agent = USER_AGENT,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return ESP_FAIL;
    esp_err_t res = esp_http_client_perform(client);
    if (out_status != NULL) *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (out_len != NULL) *out_len = capture.len;
    return res;
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

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static esp_err_t delete_recursive(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return ESP_FAIL;
    if (S_ISREG(st.st_mode)) return unlink(path) == 0 ? ESP_OK : ESP_FAIL;
    if (!S_ISDIR(st.st_mode)) return ESP_FAIL;

    DIR *dir = opendir(path);
    if (dir == NULL) return ESP_FAIL;

    char *child_path = malloc(1024);
    if (child_path == NULL) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t res = ESP_OK;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        int len = snprintf(child_path, 1024, "%s/%s", path, entry->d_name);
        if (len < 0 || len >= 1024) {
            res = ESP_FAIL;
            break;
        }
        res = delete_recursive(child_path);
        if (res != ESP_OK) break;
    }

    free(child_path);
    closedir(dir);
    if (res != ESP_OK) return res;
    return rmdir(path) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t redirect_to_music_root(httpd_req_t *req) {
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/browse/");
    httpd_resp_sendstr(req, "See /browse/");
    return ESP_OK;
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
        "<style>body{font-family:system-ui,sans-serif;margin:24px;line-height:1.4;background:#f6f5f2;color:#181818}"
        "a{color:#0645ad}.top{display:flex;align-items:baseline;gap:12px;flex-wrap:wrap;margin-bottom:18px}"
        "h1{margin:0;font-size:28px}.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:16px}"
        ".album{background:#fff;border:1px solid #ddd;border-radius:8px;overflow:hidden;box-shadow:0 1px 3px #0001}"
        ".cover{aspect-ratio:1;background:#ddd;display:block;width:100%;object-fit:cover}"
        ".blank{aspect-ratio:1;background:linear-gradient(135deg,#333,#777);display:flex;align-items:center;justify-content:center;color:white;font-size:42px}"
        ".body{padding:12px}.name{font-weight:650;margin-bottom:10px;overflow-wrap:anywhere}.actions{display:flex;gap:8px;flex-wrap:wrap}"
        ".button,button{display:inline-block;text-decoration:none;border:1px solid #bbb;border-radius:6px;padding:7px 10px;background:#f8f8f8;color:#111;font:inherit}"
        ".primary{background:#111;color:#fff;border-color:#111}.danger{border-color:#b22;color:#b22;background:#fff}.list a{display:block;padding:8px 0}"
        "form{display:inline}.row{display:flex;align-items:center;gap:8px;flex-wrap:wrap;padding:6px 0}</style>"
        "<div class=top><h1>Disc-O-Matsu</h1><small>/sd/Music</small></div>"
    );

    if (strcmp(path, MUSIC_ROOT) != 0) {
        trim_trailing_slashes(path);
        const char *rel = path + strlen(MUSIC_ROOT);
        if (*rel == '/') rel++;
        httpd_resp_sendstr_chunk(req, "<p><a class=button href='/browse/'>Back</a> <a class='button primary' href='/album/");
        url_encode_send(req, rel);
        httpd_resp_sendstr_chunk(req, "'>Download album (.tar)</a> <form method=post action='/cover/");
        url_encode_send(req, rel);
        httpd_resp_sendstr_chunk(req, "'><button type=submit>Find cover</button></form></p><div class=list>");
    } else {
        httpd_resp_sendstr_chunk(req, "<div class=grid>");
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
        if (strcmp(path, MUSIC_ROOT) == 0 && S_ISDIR(st.st_mode)) {
            char cover_path[1040];
            snprintf(cover_path, sizeof(cover_path), "%s/cover.jpg", full);
            bool has_cover = file_exists(cover_path);
            httpd_resp_sendstr_chunk(req, "<article class=album>");
            if (has_cover) {
                httpd_resp_sendstr_chunk(req, "<img class=cover src='/file/");
                url_encode_send(req, rel);
                httpd_resp_sendstr_chunk(req, "/cover.jpg'>");
            } else {
                httpd_resp_sendstr_chunk(req, "<div class=blank>CD</div>");
            }
            httpd_resp_sendstr_chunk(req, "<div class=body><div class=name>");
            html_escape_send(req, entry->d_name);
            httpd_resp_sendstr_chunk(req, "</div><div class=actions><a class='button primary' href='/browse/");
            url_encode_send(req, rel);
            httpd_resp_sendstr_chunk(req, "/'>Open</a><a class=button href='/album/");
            url_encode_send(req, rel);
            httpd_resp_sendstr_chunk(req, "'>Download</a>");
            if (!has_cover) {
                httpd_resp_sendstr_chunk(req, "<form method=post action='/cover/");
                url_encode_send(req, rel);
                httpd_resp_sendstr_chunk(req, "'><button type=submit>Find cover</button></form>");
            }
            httpd_resp_sendstr_chunk(req, "<form method=get action='/delete/");
            url_encode_send(req, rel);
            httpd_resp_sendstr_chunk(req, "'><button class=danger type=submit>Delete</button></form></div></div></article>");
        } else {
            httpd_resp_sendstr_chunk(req, "<div class=row><a href='");
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
            httpd_resp_sendstr_chunk(req, "</a><form method=get action='/delete/");
            url_encode_send(req, rel);
            httpd_resp_sendstr_chunk(req, "'><button class=danger type=submit>Delete</button></form></div>");
        }
    }
    closedir(dir);
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t delete_confirm_handler(httpd_req_t *req) {
    char path[1024];
    const char *encoded_rel = req->uri + strlen("/delete/");
    if (!build_safe_path(encoded_rel, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_OK;
    }
    trim_trailing_slashes(path);
    if (strcmp(path, MUSIC_ROOT) == 0) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Refusing to delete Music root");
        return ESP_OK;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(
        req,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Delete</title><style>body{font-family:system-ui,sans-serif;margin:24px;line-height:1.4}"
        ".button,button{display:inline-block;text-decoration:none;border:1px solid #bbb;border-radius:6px;padding:8px 12px;background:#f8f8f8;color:#111;font:inherit}"
        ".danger{background:#b22;border-color:#b22;color:#fff}</style><h1>Delete?</h1><p>"
    );
    html_escape_send(req, path_basename(path));
    httpd_resp_sendstr_chunk(req, S_ISDIR(st.st_mode) ? "</p><p>This deletes the whole album folder.</p>" : "</p>");
    httpd_resp_sendstr_chunk(req, "<form method=post action='/delete/");
    httpd_resp_sendstr_chunk(req, encoded_rel);
    httpd_resp_sendstr_chunk(
        req,
        "'><button class=danger type=submit>Delete</button> <a class=button href='/browse/'>Cancel</a></form>"
    );
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t delete_post_handler(httpd_req_t *req) {
    char path[1024];
    const char *encoded_rel = req->uri + strlen("/delete/");
    if (!build_safe_path(encoded_rel, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_OK;
    }
    trim_trailing_slashes(path);
    if (strcmp(path, MUSIC_ROOT) == 0) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Refusing to delete Music root");
        return ESP_OK;
    }

    esp_err_t res = delete_recursive(path);
    if (res != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ESP_OK;
    }
    return redirect_to_music_root(req);
}

static esp_err_t save_album_cover_from_query(const char *album_dir, const char *query) {
    uint8_t *buf = malloc(COVER_HTTP_BUF_SIZE);
    if (buf == NULL) return ESP_ERR_NO_MEM;

    char encoded[512];
    if (url_encode_to_buf(encoded, sizeof(encoded), query) < 0) {
        free(buf);
        return ESP_ERR_INVALID_ARG;
    }

    char url[768];
    snprintf(url, sizeof(url), "https://musicbrainz.org/ws/2/release/?fmt=json&limit=1&query=%s", encoded);

    size_t len = 0;
    int status = 0;
    esp_err_t res = http_get_capture(url, buf, COVER_HTTP_BUF_SIZE - 1, &len, &status);
    if (res != ESP_OK || status != 200 || len == 0) {
        free(buf);
        return res != ESP_OK ? res : ESP_FAIL;
    }
    buf[len] = '\0';

    char release_id[64] = {0};
    cJSON *root = cJSON_ParseWithLength((const char *)buf, len);
    cJSON *releases = root != NULL ? cJSON_GetObjectItem(root, "releases") : NULL;
    cJSON *release = cJSON_IsArray(releases) ? cJSON_GetArrayItem(releases, 0) : NULL;
    cJSON *id = release != NULL ? cJSON_GetObjectItem(release, "id") : NULL;
    if (cJSON_IsString(id)) snprintf(release_id, sizeof(release_id), "%s", id->valuestring);
    if (root != NULL) cJSON_Delete(root);
    if (release_id[0] == '\0') {
        free(buf);
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(url, sizeof(url), "https://coverartarchive.org/release/%s/front-250", release_id);
    res = http_get_capture(url, buf, COVER_HTTP_BUF_SIZE, &len, &status);
    if (res != ESP_OK || status != 200 || len == 0) {
        free(buf);
        return res != ESP_OK ? res : ESP_FAIL;
    }

    char cover_path[1040];
    int path_len = snprintf(cover_path, sizeof(cover_path), "%s/cover.jpg", album_dir);
    if (path_len < 0 || path_len >= (int)sizeof(cover_path)) {
        free(buf);
        return ESP_FAIL;
    }

    FILE *f = fopen(cover_path, "wb");
    if (f == NULL) {
        free(buf);
        return ESP_FAIL;
    }
    size_t written = fwrite(buf, 1, len, f);
    int close_res = fclose(f);
    free(buf);
    return written == len && close_res == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t cover_post_handler(httpd_req_t *req) {
    char path[1024];
    const char *encoded_rel = req->uri + strlen("/cover/");
    if (!build_safe_path(encoded_rel, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_OK;
    }
    trim_trailing_slashes(path);
    if (strcmp(path, MUSIC_ROOT) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad album");
        return ESP_OK;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Album not found");
        return ESP_OK;
    }

    esp_err_t res = save_album_cover_from_query(path, path_basename(path));
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "Cover lookup failed: %s", esp_err_to_name(res));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cover lookup failed");
        return ESP_OK;
    }
    return redirect_to_music_root(req);
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
    httpd_uri_t delete_get = {
        .uri = "/delete/*",
        .method = HTTP_GET,
        .handler = delete_confirm_handler,
    };
    httpd_uri_t delete_post = {
        .uri = "/delete/*",
        .method = HTTP_POST,
        .handler = delete_post_handler,
    };
    httpd_uri_t cover_post = {
        .uri = "/cover/*",
        .method = HTTP_POST,
        .handler = cover_post_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &browse));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &file));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &album));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &delete_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &delete_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &cover_post));

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
