#include <stdio.h>
#include <string.h>

#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "cd_metadata.h"
#include "cd_ripper.h"
#include "cdplayer_task.h"
#include "cdrom_audio.h"
#include "audio_output.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "pax_types.h"
#include "lastfm_scrobbler.h"
#include "wifi_file_server.h"
#include "wifi_setup.h"

static const char TAG[] = "discmatsu";

#define BLACK      0xFF000000
#define TERM_FG    0xFFC8C8C8
#define TERM_DIM   0xFF707070
#define TERM_GREEN 0xFF33FF66
#define TERM_RED   0xFFFF5555
#define TERM_SELECT_BG 0xFF103318
#define FONT_SIZE  16.0f

static pax_buf_t fb = {0};
static size_t display_h_res = 0;
static size_t display_v_res = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t display_data_endian = 0;
static QueueHandle_t input_event_queue = NULL;
static float g_char_w = 9.0f;
static float g_line_h = 20.0f;

static int track_selected = 0;
static int track_scroll = 0;
static int lastfm_track_index = -1;
static bool lastfm_was_playing = false;

typedef enum {
    OVERLAY_NONE = 0,
    OVERLAY_MAIN_MENU,
    OVERLAY_LASTFM_SETTINGS,
} overlay_t;

static overlay_t overlay = OVERLAY_NONE;
static int menu_selected = 0;
static int lastfm_selected = 0;
static bool lastfm_editing = false;
static char edit_api_key[80] = "";
static char edit_api_secret[80] = "";
static char edit_username[64] = "";
static char edit_password[96] = "";

static void blit(void) {
    esp_err_t res = bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to blit to display: %d", res);
    }
}

static void measure_font(void) {
    pax_vec2f size = pax_text_size(pax_font_sky_mono, FONT_SIZE, "M");
    if (size.x > 0) g_char_w = size.x;
    if (size.y > 0) g_line_h = size.y + 6.0f;
}

static void draw_box(float x, float y, float w, float h, pax_col_t color, const char *title) {
    pax_outline_rect(&fb, color, x, y, w, h);
    if (title != NULL && title[0] != '\0') {
        char labeled[64];
        snprintf(labeled, sizeof(labeled), " %s ", title);
        float label_w = g_char_w * (float)strlen(labeled);
        pax_simple_rect(&fb, BLACK, x + 10, y - g_line_h / 2, label_w, g_line_h);
        pax_draw_text(&fb, color, pax_font_sky_mono, FONT_SIZE, x + 10, y - g_line_h / 2, labeled);
    }
}

static void format_mmss(uint32_t total_seconds, char *out, size_t out_len) {
    snprintf(out, out_len, "%02u:%02u", (unsigned)(total_seconds / 60), (unsigned)(total_seconds % 60));
}

static char *lastfm_field_buffer(int field, size_t *out_len) {
    switch (field) {
        case 0:
            *out_len = sizeof(edit_api_key);
            return edit_api_key;
        case 1:
            *out_len = sizeof(edit_api_secret);
            return edit_api_secret;
        case 2:
            *out_len = sizeof(edit_username);
            return edit_username;
        case 3:
            *out_len = sizeof(edit_password);
            return edit_password;
        default:
            *out_len = 0;
            return NULL;
    }
}

static void load_lastfm_settings_for_edit(void) {
    lastfm_config_t config;
    lastfm_scrobbler_get_config(&config);
    snprintf(edit_api_key, sizeof(edit_api_key), "%s", config.api_key);
    snprintf(edit_api_secret, sizeof(edit_api_secret), "%s", config.api_secret);
    snprintf(edit_username, sizeof(edit_username), "%s", config.username);
    edit_password[0] = '\0';
    lastfm_selected = 0;
    lastfm_editing = false;
}

static void mask_value(const char *src, char *out, size_t out_len) {
    if (out_len == 0) return;
    size_t len = strlen(src);
    if (len == 0) {
        snprintf(out, out_len, "<empty>");
        return;
    }
    size_t shown = len < out_len - 1 ? len : out_len - 1;
    memset(out, '*', shown);
    out[shown] = '\0';
}

static void build_lastfm_track_info(
    const cdrom_status_t *cd_status, const cd_metadata_status_t *meta, int track_index, lastfm_track_info_t *out
) {
    static char fallback_title[24];
    memset(out, 0, sizeof(*out));
    if (track_index < 0 || track_index >= cd_status->track_count) return;

    const cdrom_track_t *track = &cd_status->tracks[track_index];
    snprintf(fallback_title, sizeof(fallback_title), "Track %02d", track->number);
    out->artist = meta->artist;
    out->album = meta->album;
    out->track = (track_index < meta->track_title_count && meta->track_titles[track_index][0] != '\0')
                     ? meta->track_titles[track_index]
                     : fallback_title;
    out->track_number = track->number;
    out->duration_sec = (track->end_lba - track->start_lba) / CDROM_AUDIO_SECTORS_PER_SEC;
}

static void update_lastfm_from_playback(void) {
    static cdrom_status_t cd_status;
    static cd_metadata_status_t meta;
    cdplayer_state_t play_state;
    cdrom_audio_get_status(&cd_status);
    cd_metadata_get_status(&meta);
    cdplayer_get_state(&play_state);

    bool track_changed = play_state.track_index != lastfm_track_index || (play_state.playing && !lastfm_was_playing);
    if (track_changed) {
        lastfm_scrobbler_track_changed();
        lastfm_track_index = play_state.track_index;
        if (play_state.playing && !play_state.paused) {
            lastfm_track_info_t info;
            build_lastfm_track_info(&cd_status, &meta, play_state.track_index, &info);
            lastfm_scrobbler_now_playing(&info);
        }
    }

    if (play_state.playing && !play_state.paused) {
        lastfm_track_info_t info;
        build_lastfm_track_info(&cd_status, &meta, play_state.track_index, &info);
        lastfm_scrobbler_maybe_scrobble(&info, play_state.elapsed_sec);
    }
    lastfm_was_playing = play_state.playing;
}

static void render(void) {
    pax_background(&fb, BLACK);

    float w = pax_buf_get_width(&fb);
    float h = pax_buf_get_height(&fb);

    // static: too large for the "main" task's stack (cd_metadata_status_t
    // alone holds a 99x64 track-title array, ~6.3KB).
    static cdrom_status_t cd_status;
    cdrom_audio_get_status(&cd_status);
    cdplayer_state_t play_state;
    cdplayer_get_state(&play_state);
    static cd_metadata_status_t meta;
    cd_metadata_get_status(&meta);

    pax_draw_text(&fb, TERM_GREEN, pax_font_sky_mono, FONT_SIZE + 4.0f, 16, 12, "Disc-O-Matsu");

    char status_line[200];
    if (!cd_status.drive_present) {
        snprintf(status_line, sizeof(status_line), "No USB drive connected");
    } else if (!cd_status.disc_present) {
        snprintf(status_line, sizeof(status_line), "Drive connected, no disc");
    } else if (meta.state == CD_METADATA_STATE_FOUND && meta.album[0] != '\0') {
        snprintf(status_line, sizeof(status_line), "%s - %s", meta.artist, meta.album);
    } else if (meta.state == CD_METADATA_STATE_LOOKING_UP) {
        snprintf(status_line, sizeof(status_line), "%d track(s) - looking up metadata...", cd_status.track_count);
    } else {
        snprintf(status_line, sizeof(status_line), "%d track(s)", cd_status.track_count);
    }
    pax_draw_text(&fb, TERM_DIM, pax_font_sky_mono, FONT_SIZE, 16, 12 + g_line_h, status_line);

    bool show_art  = (meta.cover_art_rgb565 != NULL && meta.cover_art_width > 0 && meta.cover_art_height > 0);
    float art_size = 0;
    if (show_art) {
        art_size = (float)(meta.cover_art_height > meta.cover_art_width ? meta.cover_art_height : meta.cover_art_width);
        // Wrap the decoded RGB565 pixels in a properly-tagged pax_buf_t and
        // use the format-aware pax_blit() (not pax_blit_raw, which assumes
        // the source is already in fb's own pixel format - fb may not
        // actually be RGB565 even though the cover art decode always is).
        pax_buf_t art_buf = {0};
        pax_buf_init(
            &art_buf, (void *)meta.cover_art_rgb565, meta.cover_art_width, meta.cover_art_height, PAX_BUF_16_565RGB
        );
        pax_blit(&fb, &art_buf, (int)(w - art_size - 16), 12);
    }

    float header_h = show_art && art_size > g_line_h * 2 ? art_size : g_line_h * 2;
    float list_y   = 12 + header_h + 12;
    float list_h   = h - list_y - g_line_h * 3 - 16;
    draw_box(16, list_y, w - 32, list_h, TERM_GREEN, "Tracks");

    int visible_rows = (int)((list_h - g_line_h) / g_line_h);
    if (visible_rows < 1) visible_rows = 1;

    if (track_selected >= cd_status.track_count) track_selected = cd_status.track_count > 0 ? cd_status.track_count - 1 : 0;
    if (track_selected < 0) track_selected = 0;
    if (track_selected < track_scroll) track_scroll = track_selected;
    if (track_selected >= track_scroll + visible_rows) track_scroll = track_selected - visible_rows + 1;
    if (track_scroll < 0) track_scroll = 0;

    for (int row = 0; row < visible_rows; row++) {
        int idx = track_scroll + row;
        if (idx >= cd_status.track_count) break;

        const cdrom_track_t *track = &cd_status.tracks[idx];
        float ry = list_y + g_line_h * (row + 1);
        bool selected = (idx == track_selected);
        bool is_current = (idx == play_state.track_index);

        if (selected) {
            pax_simple_rect(&fb, TERM_SELECT_BG, 24, ry - 2, w - 64, g_line_h);
        }

        char duration[16];
        uint32_t track_secs = (track->end_lba - track->start_lba) / CDROM_AUDIO_SECTORS_PER_SEC;
        format_mmss(track_secs, duration, sizeof(duration));

        char line[80];
        const char *marker = is_current ? (play_state.paused ? "|| " : "> ") : "   ";
        bool has_title      = track->is_audio && idx < meta.track_title_count && meta.track_titles[idx][0] != '\0';
        if (has_title) {
            snprintf(line, sizeof(line), "%s%02d. %s  %s", marker, track->number, meta.track_titles[idx], duration);
        } else {
            snprintf(
                line, sizeof(line), "%s%02d. %s%s  %s", marker, track->number, track->is_audio ? "Track" : "Data ",
                track->is_audio ? "" : " (skip)", duration
            );
        }
        pax_col_t color = !track->is_audio ? TERM_DIM : (selected ? TERM_GREEN : TERM_FG);
        pax_draw_text(&fb, color, pax_font_sky_mono, FONT_SIZE, 32, ry, line);
    }

    cd_ripper_status_t rip;
    cd_ripper_get_status(&rip);
    wifi_file_server_status_t files;
    wifi_file_server_get_status(&files);
    char rip_line[160];
    pax_col_t rip_color = TERM_DIM;
    if (files.state != WIFI_FILE_SERVER_STATE_IDLE) {
        switch (files.state) {
            case WIFI_FILE_SERVER_STATE_CONNECTING_WIFI:
                snprintf(rip_line, sizeof(rip_line), "Files: connecting WiFi...");
                break;
            case WIFI_FILE_SERVER_STATE_MOUNTING_SD:
                snprintf(rip_line, sizeof(rip_line), "Files: mounting SD card...");
                break;
            case WIFI_FILE_SERVER_STATE_RUNNING:
                snprintf(rip_line, sizeof(rip_line), "Files: %s  F4=Stop", files.url);
                rip_color = TERM_GREEN;
                break;
            case WIFI_FILE_SERVER_STATE_ERROR:
                snprintf(rip_line, sizeof(rip_line), "Files error: %s", files.last_error);
                rip_color = TERM_RED;
                break;
            case WIFI_FILE_SERVER_STATE_IDLE:
            default:
                break;
        }
    } else {
        switch (rip.state) {
            case CD_RIPPER_STATE_MOUNTING_SD:
                snprintf(rip_line, sizeof(rip_line), "Rip: mounting SD card...");
                break;
            case CD_RIPPER_STATE_RIPPING:
                snprintf(
                    rip_line, sizeof(rip_line), "Rip: track %d/%d %lu%%", rip.current_track, rip.total_tracks,
                    (unsigned long)rip.current_percent
                );
                break;
            case CD_RIPPER_STATE_DONE:
                snprintf(rip_line, sizeof(rip_line), "Rip done: %.140s", rip.current_path);
                break;
            case CD_RIPPER_STATE_ERROR:
                snprintf(rip_line, sizeof(rip_line), "Rip error: %s", rip.last_error);
                rip_color = TERM_RED;
                break;
            case CD_RIPPER_STATE_IDLE:
            default:
                snprintf(rip_line, sizeof(rip_line), "F3=Rip WAV to SD  F4=WiFi files");
                break;
        }
    }
    pax_draw_text(&fb, rip_color, pax_font_sky_mono, FONT_SIZE, 16, h - g_line_h * 2, rip_line);

    char footer[120];
    if (play_state.playing) {
        char elapsed[16], total[16];
        format_mmss(play_state.elapsed_sec, elapsed, sizeof(elapsed));
        format_mmss(play_state.total_sec, total, sizeof(total));
        snprintf(
            footer, sizeof(footer), "%s %s/%s  Vol %d%%  Enter=Play/Pause </>=Prev/Next F2=Eject",
            play_state.paused ? "Paused" : "Playing", elapsed, total, audio_output_get_volume_percent()
        );
    } else {
        snprintf(
            footer, sizeof(footer), "Vol %d%%  Enter=Play  Left/Right=Prev/Next  F2=Eject",
            audio_output_get_volume_percent()
        );
    }
    pax_draw_text(&fb, TERM_DIM, pax_font_sky_mono, FONT_SIZE, 16, h - g_line_h, footer);

    if (overlay == OVERLAY_MAIN_MENU) {
        float menu_w = 260;
        float menu_h = g_line_h * 4;
        float menu_x = (w - menu_w) / 2;
        float menu_y = (h - menu_h) / 2;
        pax_simple_rect(&fb, BLACK, menu_x, menu_y, menu_w, menu_h);
        draw_box(menu_x, menu_y, menu_w, menu_h, TERM_GREEN, "Menu");
        const char *items[] = {"Last.fm settings", "Exit"};
        for (int i = 0; i < 2; i++) {
            float y = menu_y + g_line_h * (i + 1) + 4;
            if (menu_selected == i) pax_simple_rect(&fb, TERM_SELECT_BG, menu_x + 8, y - 2, menu_w - 16, g_line_h);
            pax_draw_text(&fb, menu_selected == i ? TERM_GREEN : TERM_FG, pax_font_sky_mono, FONT_SIZE, menu_x + 16, y, items[i]);
        }
        pax_draw_text(
            &fb, TERM_DIM, pax_font_sky_mono, FONT_SIZE, menu_x + 16, menu_y + g_line_h * 3 + 8, "Enter=Select Esc=Back"
        );
    } else if (overlay == OVERLAY_LASTFM_SETTINGS) {
        float menu_w = w - 64;
        if (menu_w > 620) menu_w = 620;
        float menu_h = g_line_h * 10;
        float menu_x = (w - menu_w) / 2;
        float menu_y = (h - menu_h) / 2;
        pax_simple_rect(&fb, BLACK, menu_x, menu_y, menu_w, menu_h);
        draw_box(menu_x, menu_y, menu_w, menu_h, TERM_GREEN, "Last.fm");

        lastfm_status_t status;
        lastfm_scrobbler_get_status(&status);
        char status_line[160];
        snprintf(
            status_line, sizeof(status_line), "%s%s%s", status.enabled ? "Ready" : "Not linked",
            status.username[0] != '\0' ? " - " : "", status.username
        );
        pax_draw_text(&fb, status.enabled ? TERM_GREEN : TERM_DIM, pax_font_sky_mono, FONT_SIZE, menu_x + 16, menu_y + g_line_h, status_line);
        if (status.last_error[0] != '\0') {
            pax_draw_text(&fb, TERM_RED, pax_font_sky_mono, FONT_SIZE, menu_x + 16, menu_y + g_line_h * 2, status.last_error);
        }

        const char *labels[] = {"API key", "Secret", "Username", "Password", "Login + save", "Back"};
        for (int i = 0; i < 6; i++) {
            float y = menu_y + g_line_h * (i + 3);
            if (lastfm_selected == i) pax_simple_rect(&fb, TERM_SELECT_BG, menu_x + 8, y - 2, menu_w - 16, g_line_h);

            char value[128] = "";
            if (i < 4) {
                size_t cap;
                char *buf = lastfm_field_buffer(i, &cap);
                (void)cap;
                if (i == 1 || i == 3) {
                    mask_value(buf, value, sizeof(value));
                } else {
                    snprintf(value, sizeof(value), "%s", buf[0] != '\0' ? buf : "<empty>");
                }
                char line[180];
                snprintf(line, sizeof(line), "%s: %.110s%s", labels[i], value, lastfm_editing && lastfm_selected == i ? "_" : "");
                pax_draw_text(&fb, lastfm_selected == i ? TERM_GREEN : TERM_FG, pax_font_sky_mono, FONT_SIZE, menu_x + 16, y, line);
            } else {
                pax_draw_text(&fb, lastfm_selected == i ? TERM_GREEN : TERM_FG, pax_font_sky_mono, FONT_SIZE, menu_x + 16, y, labels[i]);
            }
        }
        pax_draw_text(
            &fb, TERM_DIM, pax_font_sky_mono, FONT_SIZE, menu_x + 16, menu_y + g_line_h * 9,
            lastfm_editing ? "Type text  Enter=Done  Esc=Cancel" : "Enter=Edit/Select  Esc=Back"
        );
    }

    blit();
}

static bool handle_input(bsp_input_event_t *event) {
    if (overlay == OVERLAY_LASTFM_SETTINGS && lastfm_editing && event->type == INPUT_EVENT_TYPE_KEYBOARD) {
        char c = event->args_keyboard.ascii;
        size_t cap;
        char *buf = lastfm_field_buffer(lastfm_selected, &cap);
        if (buf == NULL) return false;
        size_t len = strlen(buf);
        if (c == '\b') {
            if (len > 0) buf[len - 1] = '\0';
            return true;
        }
        if (c >= 32 && c <= 126 && len + 1 < cap) {
            buf[len] = c;
            buf[len + 1] = '\0';
            return true;
        }
        return false;
    }

    if (event->type != INPUT_EVENT_TYPE_NAVIGATION || !event->args_navigation.state) return false;

    if (overlay == OVERLAY_MAIN_MENU) {
        switch (event->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_UP:
                if (menu_selected > 0) menu_selected--;
                return true;
            case BSP_INPUT_NAVIGATION_KEY_DOWN:
                if (menu_selected < 1) menu_selected++;
                return true;
            case BSP_INPUT_NAVIGATION_KEY_RETURN:
                if (menu_selected == 0) {
                    load_lastfm_settings_for_edit();
                    overlay = OVERLAY_LASTFM_SETTINGS;
                } else {
                    bsp_device_restart_to_launcher();
                }
                return true;
            case BSP_INPUT_NAVIGATION_KEY_ESC: overlay = OVERLAY_NONE; return true;
            default: return false;
        }
    }

    if (overlay == OVERLAY_LASTFM_SETTINGS) {
        switch (event->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_ESC:
                if (lastfm_editing) {
                    lastfm_editing = false;
                } else {
                    overlay = OVERLAY_MAIN_MENU;
                }
                return true;
            case BSP_INPUT_NAVIGATION_KEY_UP:
                if (!lastfm_editing && lastfm_selected > 0) lastfm_selected--;
                return true;
            case BSP_INPUT_NAVIGATION_KEY_DOWN:
            case BSP_INPUT_NAVIGATION_KEY_TAB:
                if (!lastfm_editing && lastfm_selected < 5) lastfm_selected++;
                return true;
            case BSP_INPUT_NAVIGATION_KEY_BACKSPACE:
                if (lastfm_editing) {
                    size_t cap;
                    char *buf = lastfm_field_buffer(lastfm_selected, &cap);
                    (void)cap;
                    if (buf != NULL) {
                        size_t len = strlen(buf);
                        if (len > 0) buf[len - 1] = '\0';
                    }
                }
                return true;
            case BSP_INPUT_NAVIGATION_KEY_RETURN:
                if (lastfm_selected < 4) {
                    lastfm_editing = !lastfm_editing;
                } else if (lastfm_selected == 4) {
                    esp_err_t res = lastfm_scrobbler_set_api_credentials(edit_api_key, edit_api_secret);
                    if (res == ESP_OK) res = lastfm_scrobbler_login(edit_username, edit_password);
                    if (res != ESP_OK) ESP_LOGW(TAG, "Last.fm settings failed: %s", esp_err_to_name(res));
                    edit_password[0] = '\0';
                } else {
                    overlay = OVERLAY_MAIN_MENU;
                }
                return true;
            default:
                return false;
        }
    }

    static cdrom_status_t cd_status;
    cdrom_audio_get_status(&cd_status);
    cdplayer_state_t play_state;
    cdplayer_get_state(&play_state);

    switch (event->args_navigation.key) {
        case BSP_INPUT_NAVIGATION_KEY_VOLUME_UP: {
            int vol = audio_output_get_volume_percent() + 5;
            if (vol > 100) vol = 100;
            audio_output_set_volume_percent(vol);
            return true;
        }
        case BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN: {
            int vol = audio_output_get_volume_percent() - 5;
            if (vol < 0) vol = 0;
            audio_output_set_volume_percent(vol);
            return true;
        }
        case BSP_INPUT_NAVIGATION_KEY_UP:
            if (track_selected > 0) track_selected--;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            if (track_selected < cd_status.track_count - 1) track_selected++;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            if (track_selected < 0 || track_selected >= cd_status.track_count) return false;
            if (!cd_status.tracks[track_selected].is_audio) return false;
            if (play_state.playing && play_state.track_index == track_selected) {
                cdplayer_pause(!play_state.paused);
            } else {
                cdplayer_play_track(track_selected);
            }
            return true;
        case BSP_INPUT_NAVIGATION_KEY_LEFT:
            cdplayer_prev();
            return true;
        case BSP_INPUT_NAVIGATION_KEY_RIGHT:
            cdplayer_next();
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F2:
            cdrom_audio_eject();
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F3: {
            if (cd_ripper_is_active()) return false;
            if (wifi_file_server_is_active()) return false;
            if (play_state.playing) cdplayer_stop();
            static cd_metadata_status_t meta;
            cd_metadata_get_status(&meta);
            esp_err_t res = cd_ripper_start(&cd_status, &meta);
            if (res != ESP_OK) ESP_LOGW(TAG, "Rip start failed: %s", esp_err_to_name(res));
            return true;
        }
        case BSP_INPUT_NAVIGATION_KEY_F4: {
            if (cd_ripper_is_active()) return false;
            if (wifi_file_server_is_active()) {
                wifi_file_server_stop();
            } else {
                if (play_state.playing) cdplayer_stop();
                esp_err_t res = wifi_file_server_start();
                if (res != ESP_OK) ESP_LOGW(TAG, "File server start failed: %s", esp_err_to_name(res));
            }
            return true;
        }
        case BSP_INPUT_NAVIGATION_KEY_ESC:
            overlay = OVERLAY_MAIN_MENU;
            menu_selected = 0;
            return true;
        default:
            return false;
    }
}

void app_main(void) {
    gpio_install_isr_service(0);

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        res = nvs_flash_erase();
        if (res == ESP_OK) res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS flash: %d", res);
    }

    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB,
                .num_fbs                = 1,
            },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BSP: %d", res);
        return;
    }

    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get display parameters: %d", res);
        return;
    }

    pax_buf_type_t format = PAX_BUF_24_888RGB;
    switch (display_color_format) {
        case BSP_DISPLAY_COLOR_FORMAT_16_565RGB: format = PAX_BUF_16_565RGB; break;
        case BSP_DISPLAY_COLOR_FORMAT_24_888RGB: format = PAX_BUF_24_888RGB; break;
        case BSP_DISPLAY_COLOR_FORMAT_32_8888ARGB: format = PAX_BUF_32_8888ARGB; break;
        default: break;
    }

    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();
    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90: orientation = PAX_O_ROT_CCW; break;
        case BSP_DISPLAY_ROTATION_180: orientation = PAX_O_ROT_HALF; break;
        case BSP_DISPLAY_ROTATION_270: orientation = PAX_O_ROT_CW; break;
        case BSP_DISPLAY_ROTATION_0:
        default: orientation = PAX_O_UPRIGHT; break;
    }

    pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);
    pax_buf_set_orientation(&fb, orientation);

    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    measure_font();

    pax_background(&fb, BLACK);
    pax_draw_text(&fb, TERM_GREEN, pax_font_sky_mono, FONT_SIZE, 16, 16, "Starting Disc-O-Matsu...");
    blit();

    ESP_ERROR_CHECK(wifi_setup_init());
    ESP_ERROR_CHECK(lastfm_scrobbler_init());
    ESP_ERROR_CHECK(cd_metadata_init());
    ESP_ERROR_CHECK(cdrom_audio_init());
    ESP_ERROR_CHECK(cdplayer_task_init());
    ESP_ERROR_CHECK(cd_ripper_init());
    ESP_ERROR_CHECK(wifi_file_server_init());

    render();

    while (1) {
        bsp_input_event_t event;
        bool need_redraw = false;

        if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(200)) == pdTRUE) {
            need_redraw = handle_input(&event);
        }

        if (cdrom_audio_consume_dirty()) need_redraw = true;
        if (cdplayer_consume_dirty()) {
            update_lastfm_from_playback();
            need_redraw = true;
        }
        if (cd_metadata_consume_dirty()) need_redraw = true;
        if (lastfm_scrobbler_consume_dirty()) need_redraw = true;
        if (cd_ripper_consume_dirty()) need_redraw = true;
        if (wifi_file_server_consume_dirty()) need_redraw = true;

        if (need_redraw) render();
    }
}
