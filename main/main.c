#include <stdio.h>
#include <string.h>

#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "cd_metadata.h"
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
static bool show_exit_menu = false;

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
    float list_h   = h - list_y - g_line_h * 2 - 16;
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

    char footer[110];
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

    if (show_exit_menu) {
        float menu_w = 220;
        float menu_h = g_line_h * 3;
        float menu_x = (w - menu_w) / 2;
        float menu_y = (h - menu_h) / 2;
        pax_simple_rect(&fb, BLACK, menu_x, menu_y, menu_w, menu_h);
        draw_box(menu_x, menu_y, menu_w, menu_h, TERM_GREEN, "Menu");
        pax_simple_rect(&fb, TERM_SELECT_BG, menu_x + 8, menu_y + g_line_h + 4, menu_w - 16, g_line_h);
        pax_draw_text(&fb, TERM_GREEN, pax_font_sky_mono, FONT_SIZE, menu_x + 16, menu_y + g_line_h + 6, "Exit");
        pax_draw_text(
            &fb, TERM_DIM, pax_font_sky_mono, FONT_SIZE, menu_x + 16, menu_y + g_line_h * 2 + 8, "Enter=Exit Esc=Back"
        );
    }

    blit();
}

static bool handle_input(bsp_input_event_t *event) {
    if (event->type != INPUT_EVENT_TYPE_NAVIGATION || !event->args_navigation.state) return false;

    if (show_exit_menu) {
        switch (event->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_RETURN: bsp_device_restart_to_launcher(); return true;
            case BSP_INPUT_NAVIGATION_KEY_ESC: show_exit_menu = false; return true;
            default: return false;
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
        case BSP_INPUT_NAVIGATION_KEY_ESC:
            show_exit_menu = true;
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
    ESP_ERROR_CHECK(cd_metadata_init());
    ESP_ERROR_CHECK(cdrom_audio_init());
    ESP_ERROR_CHECK(cdplayer_task_init());

    render();

    while (1) {
        bsp_input_event_t event;
        bool need_redraw = false;

        if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(200)) == pdTRUE) {
            need_redraw = handle_input(&event);
        }

        if (cdrom_audio_consume_dirty()) need_redraw = true;
        if (cdplayer_consume_dirty()) need_redraw = true;
        if (cd_metadata_consume_dirty()) need_redraw = true;

        if (need_redraw) render();
    }
}
