// Private interface of the TFT UI (ui.c and the ui_*.c pages). The helpers
// shared with the OLED UI are in ui_common.h. Everything here is called with
// the LVGL lock held.
#pragma once

#include "ui_common.h"

// ---- Colors (0xRRGGBB) ----
#define UI_COLOR_BG     0x000000
#define UI_COLOR_TEXT   0xFFFFFF
#define UI_COLOR_DIM    0x808080    // stale, unknown or greyed values
#define UI_COLOR_GREEN  0x30D158
#define UI_COLOR_AMBER  0xFFB000
#define UI_COLOR_RED    0xFF453A
#define UI_COLOR_BLUE   0x0A84FF
#define UI_COLOR_BLACK  0x000000    // text on amber and grey bars
#define UI_COLOR_CARD   0x2C2C2E    // overlay card background
#define UI_COLOR_TRACK  0x3A3A3C    // empty part of the bars

// Recolor command prefix (labels with recolor enabled): "#0A84FF BAL#"
#define UI_RECOLOR_BLUE "#0A84FF "

// ---- Fonts (Montserrat line heights 16/22/30/52) ----
#define UI_FONT_14 (&lv_font_montserrat_14)
#define UI_FONT_20 (&lv_font_montserrat_20)
#define UI_FONT_28 (&lv_font_montserrat_28)
#define UI_FONT_48 (&lv_font_montserrat_48)

// The built-in fonts have ASCII, the degree sign, the bullet and LV_SYMBOL_* only
#define UI_DEG "\xC2\xB0"

#define UI_SCREEN_W 240
#define UI_SCREEN_H 240

// Current within +-0.05 A counts as idle (white)
#define UI_CURRENT_IDLE_10MA 5

// ---- Pages ----
// create: build the page on its own screen (once, from ui_build) and return the screen.
// update: refresh the page; called only while it is visible (and once at build).
lv_obj_t *ui_main_create(void);
void ui_main_update(const ui_view_t *v);
lv_obj_t *ui_cells_create(void);
void ui_cells_update(const ui_view_t *v);
lv_obj_t *ui_alarms_create(void);
void ui_alarms_update(const ui_view_t *v);
lv_obj_t *ui_details_create(void);
void ui_details_update(const ui_view_t *v);

// ---- Widget factory (ui.c) ----
// Bar with range 0-100, grey track, dim indicator
lv_obj_t *ui_bar_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);

// rgb while the data is live, dim otherwise
static inline uint32_t ui_live_color(const ui_view_t *v, uint32_t rgb)
{
    return v->live ? rgb : UI_COLOR_DIM;
}

// ---- Formatting (fixed point only) ----
// "3.281-3.312" (min-max of the non-zero cells). Writes nothing and returns false
// if there is no valid cell.
bool ui_str_add_cell_range(ui_str_t *s, const jk_data_t *d);
// "-12.34 A" below 100 A, "175.0 A" from 100 A (10 mA units)
void ui_str_add_current(ui_str_t *s, int32_t current_10ma);
// "-652 W", or "17.4 kW" from 10 kW (0.1 W units)
void ui_str_add_power(ui_str_t *s, int32_t power_dw);
