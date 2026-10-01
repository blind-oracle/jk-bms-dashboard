// Private interface shared by the UI files (ui.c and the ui_*.c pages).
// Everything here is called with the LVGL lock held.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "jk_proto.h"
#include "ui.h"

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
#define UI_NA  "--"

#define UI_SCREEN_W 240
#define UI_SCREEN_H 240

// Current within +-0.05 A counts as idle (white)
#define UI_CURRENT_IDLE_10MA 5

// The model as the pages see it
typedef struct {
    const ui_model_t *m;
    const jk_data_t  *d;    // &m->bms.data, meaningful only if has_data
    bool has_data;          // there are values to show
    bool live;              // has_data and not offline: normal colors, else dim
} ui_view_t;

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

// The Alarms page is in the ring
bool ui_alarms_active(const ui_model_t *m);

// ---- Alarms (0x8B) ----
// Bits 14 and 15 have no name in jk_proto; the UI lists them as protections
// ("Alarm bit 14") so that the ring, the banner and the counts always agree.
#define UI_ALARM_BITS 16
unsigned ui_alarm_count(uint16_t alarms);
// Only bit 0 (low capacity) is a warning
bool ui_alarm_is_warning(unsigned bit);
// jk_alarm_top(), except that an unnamed bit outranks the warning. -1 if none.
int ui_alarm_top(uint16_t alarms);
// Long or short name; an unnamed bit is written to buf as "Alarm bit N"
const char *ui_alarm_name(unsigned bit, bool short_name, char *buf, size_t cap);

// ---- Widget factory (ui.c) ----
// Screen with a black background, not scrollable
lv_obj_t *ui_screen_create(void);
// Plain rectangle with every theme style removed, opaque, not scrollable
lv_obj_t *ui_box_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);
// Single-line label: fixed width w, height = font line height, CLIP, white
lv_obj_t *ui_label_create(lv_obj_t *parent, const lv_font_t *font, int32_t x, int32_t y, int32_t w,
                          lv_text_align_t align);
// Bar with range 0-100, grey track, dim indicator
lv_obj_t *ui_bar_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);

// ---- Setters that touch the object only when the value changes (no needless redraws) ----
// Return true if the text changed
bool ui_label_set(lv_obj_t *label, const char *text);
bool ui_label_setf(lv_obj_t *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ui_set_text_color(lv_obj_t *obj, uint32_t rgb);
void ui_set_bg_color(lv_obj_t *obj, lv_part_t part, uint32_t rgb);
void ui_set_font(lv_obj_t *obj, const lv_font_t *font);
void ui_set_hidden(lv_obj_t *obj, bool hidden);

// rgb while the data is live, dim otherwise
static inline uint32_t ui_live_color(const ui_view_t *v, uint32_t rgb)
{
    return v->live ? rgb : UI_COLOR_DIM;
}

// ---- Bounded string builder: appends never overflow, output is always terminated ----
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} ui_str_t;

void ui_str_init(ui_str_t *s, char *buf, size_t cap);
void ui_str_add(ui_str_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// ---- Formatting (fixed point only) ----
// "3.281" from mV
void ui_str_add_mv(ui_str_t *s, uint32_t mv);
// "3.281-3.312" (min-max of the non-zero cells). Writes nothing and returns false
// if there is no valid cell.
bool ui_str_add_cell_range(ui_str_t *s, const jk_data_t *d);
// "-12.34 A" below 100 A, "175.0 A" from 100 A (10 mA units)
void ui_str_add_current(ui_str_t *s, int32_t current_10ma);
// "-652 W", or "17.4 kW" from 10 kW (0.1 W units)
void ui_str_add_power(ui_str_t *s, int32_t power_dw);

// Temperature sensors in display order: 0 MOS, 1 T1, 2 T2. Returns false if the
// sensor is not reported (T1/T2 are also limited by the sensor count 0x86).
#define UI_TEMP_SENSORS 3
bool ui_temp_get(const jk_data_t *d, unsigned sensor, int *out_c);

// Copy a BMS string (src_size bytes at most, stops at NUL), keeping printable
// ASCII only and trimming spaces. Returns the length; 0 = nothing to show.
size_t ui_copy_printable(char *dst, size_t cap, const char *src, size_t src_size);
