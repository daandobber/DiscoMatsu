#include "dobber_splash.h"

#include <stdio.h>

#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"

#define SPLASH_BG       0xFFB8BAB7
#define SPLASH_PAPER    0xFFD8D7D2
#define SPLASH_GRID     0xFF969995
#define SPLASH_INK      0xFF20272B

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t size;
} block_t;

static const block_t BLOCKS[] = {
    {18, 2, 5},  {44, 8, 3},  {62, 15, 4}, {84, 19, 4}, {96, 5, 4},  {9, 29, 3},
    {31, 33, 4}, {70, 31, 3}, {92, 37, 4}, {4, 53, 4},  {64, 48, 5}, {78, 56, 3},
    {96, 59, 4}, {14, 68, 4}, {42, 72, 3}, {68, 70, 4}, {88, 77, 4}, {5, 88, 5},
    {29, 91, 3}, {63, 89, 4}, {77, 94, 3}, {95, 91, 5},
};

static float minf(float a, float b) {
    return a < b ? a : b;
}

static float maxf(float a, float b) {
    return a > b ? a : b;
}

static void draw_centered(
    pax_buf_t *buffer, const char *text, pax_col_t color, float font_size, float center_x, float y
) {
    pax_vec2f text_size = pax_text_size(pax_font_sky_mono, font_size, text);
    pax_draw_text(buffer, color, pax_font_sky_mono, font_size, center_x - text_size.x * 0.5f, y, text);
}

static void draw_window(pax_buf_t *buffer, float x, float y, float width, float height, float unit) {
    // Three almost-aligned window frames give the deliberately cheap copied-
    // and-pasted look without needing any bitmap assets.
    for (int layer = 2; layer >= 0; layer--) {
        float offset_x = (float)(layer * 2) * unit;
        float offset_y = (float)(2 - layer) * unit;
        pax_simple_rect(buffer, SPLASH_INK, x + offset_x, y + offset_y, width, height);
        pax_simple_rect(
            buffer, SPLASH_PAPER, x + offset_x + unit * 0.45f, y + offset_y + unit * 0.45f,
            width - unit * 0.9f, height - unit * 0.9f
        );
    }

    pax_simple_line(buffer, SPLASH_INK, x + unit, y + unit * 2.1f, x + width - unit, y + unit * 2.1f);
    pax_simple_rect(buffer, SPLASH_INK, x + unit, y + unit * 0.9f, unit * 0.65f, unit * 0.65f);
    pax_simple_rect(buffer, SPLASH_INK, x + unit * 2.1f, y + unit * 0.9f, unit * 0.65f, unit * 0.65f);
}

void dobber_splash_render(pax_buf_t *buffer, uint8_t percent) {
    if (buffer == NULL) return;
    if (percent > 100) percent = 100;

    float width = (float)pax_buf_get_width(buffer);
    float height = (float)pax_buf_get_height(buffer);
    float shortest = minf(width, height);
    float unit = maxf(2.0f, shortest / 64.0f);
    float grid = maxf(8.0f, shortest / 28.0f);

    pax_background(buffer, SPLASH_BG);

    for (float x = 0; x < width; x += grid) {
        pax_simple_line(buffer, SPLASH_GRID, x, 0, x, height);
    }
    for (float y = 0; y < height; y += grid) {
        pax_simple_line(buffer, SPLASH_GRID, 0, y, width, y);
    }

    // Long construction lines provide the crude editorial-layout feeling
    // of the reference while the centered windows remain the clear focus.
    for (int i = 0; i < 7; i++) {
        float x = width * (0.09f + (float)i * 0.105f);
        float top = height * (0.08f + (float)(i % 3) * 0.05f);
        float bottom = height * (0.80f + (float)(i % 2) * 0.12f);
        pax_simple_line(buffer, SPLASH_INK, x, top, x + (float)(i - 3) * unit * 0.35f, bottom);
    }

    for (size_t i = 0; i < sizeof(BLOCKS) / sizeof(BLOCKS[0]); i++) {
        float block_size = unit * (float)BLOCKS[i].size * 0.72f;
        float x = width * (float)BLOCKS[i].x / 100.0f;
        float y = height * (float)BLOCKS[i].y / 100.0f;
        pax_simple_rect(buffer, SPLASH_INK, x, y, block_size, block_size);
    }

    float panel_w = width * 0.51f;
    float panel_x = (width - panel_w) * 0.5f;
    float panel_y = height * 0.15f;
    float panel_h = height * 0.29f;
    draw_window(buffer, panel_x, panel_y, panel_w, panel_h, unit);

    float title_size = maxf(10.0f, minf(25.0f, shortest * 0.042f));
    draw_centered(
        buffer, "Daan Dobber made this", SPLASH_INK, title_size, panel_x + panel_w * 0.5f,
        panel_y + panel_h * 0.47f
    );

    // The only status copy is LOADING plus the real percentage supplied by
    // the app; no invented phase names or smoothly faked progress.
    float status_w = width * 0.32f;
    float status_x = (width - status_w) * 0.5f;
    float status_y = height * 0.51f;
    float status_h = height * 0.20f;
    pax_simple_rect(buffer, SPLASH_INK, status_x + unit, status_y + unit, status_w, status_h);
    pax_simple_rect(buffer, SPLASH_PAPER, status_x, status_y, status_w, status_h);
    pax_outline_rect(buffer, SPLASH_INK, status_x, status_y, status_w, status_h);

    float label_size = maxf(9.0f, minf(17.0f, shortest * 0.027f));
    float percent_size = maxf(18.0f, minf(42.0f, shortest * 0.072f));
    draw_centered(buffer, "LOADING", SPLASH_INK, label_size, status_x + status_w * 0.5f, status_y + unit * 1.2f);

    char percent_text[8];
    snprintf(percent_text, sizeof(percent_text), "%u%%", (unsigned)percent);
    draw_centered(
        buffer, percent_text, SPLASH_INK, percent_size, status_x + status_w * 0.5f,
        status_y + status_h * 0.38f
    );

    float bar_x = status_x + unit * 1.3f;
    float bar_y = status_y + status_h - unit * 1.7f;
    float bar_w = status_w - unit * 2.6f;
    float bar_h = maxf(2.0f, unit * 0.65f);
    pax_outline_rect(buffer, SPLASH_INK, bar_x, bar_y, bar_w, bar_h);
    if (percent > 0) {
        pax_simple_rect(
            buffer, SPLASH_INK, bar_x + 1.0f, bar_y + 1.0f,
            (bar_w - 2.0f) * (float)percent / 100.0f, maxf(1.0f, bar_h - 2.0f)
        );
    }

}
