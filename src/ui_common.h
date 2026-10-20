// Helpers shared by both UIs: the TFT UI (ui.c, ui_*.c) and the OLED UI
// (ui_oled*.c). Everything that touches an LVGL object is called with the
// LVGL lock held.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "jk_proto.h"
#include "ui.h"

#define UI_NA "--"

// The model as the pages see it
typedef struct {
    const ui_model_t *m;
    const jk_data_t  *d;    // &m->bms.data, meaningful only if has_data
    bool has_data;          // there are values to show
    bool live;              // has_data and not offline: normal colors, else dim
} ui_view_t;

void ui_view_init(ui_view_t *v, const ui_model_t *m);

// ---- Widget factory ----
// Screen with a black background, not scrollable
lv_obj_t *ui_screen_create(void);
// Plain rectangle with every theme style removed, opaque, not scrollable
lv_obj_t *ui_box_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);
// Single-line label: fixed width w, height = font line height, CLIP, white
lv_obj_t *ui_label_create(lv_obj_t *parent, const lv_font_t *font, int32_t x, int32_t y, int32_t w,
                          lv_text_align_t align);

// ---- Setters that touch the object only when the value changes (no needless redraws) ----
// Return true if the text changed
bool ui_label_set(lv_obj_t *label, const char *text);
bool ui_label_setf(lv_obj_t *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ui_set_text_color(lv_obj_t *obj, uint32_t rgb);
void ui_set_bg_color(lv_obj_t *obj, lv_part_t part, uint32_t rgb);
void ui_set_font(lv_obj_t *obj, const lv_font_t *font);
void ui_set_hidden(lv_obj_t *obj, bool hidden);

// ---- Bounded string builder: appends never overflow, output is always terminated ----
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} ui_str_t;

void ui_str_init(ui_str_t *s, char *buf, size_t cap);
void ui_str_add(ui_str_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// "3.281" from mV
void ui_str_add_mv(ui_str_t *s, uint32_t mv);

// Temperature sensors in display order: 0 MOS, 1 T1, 2 T2. Returns false if the
// sensor is not reported (T1/T2 are also limited by the sensor count 0x86).
#define UI_TEMP_SENSORS 3
bool ui_temp_get(const jk_data_t *d, unsigned sensor, int *out_c);

// Copy a BMS string (src_size bytes at most, stops at NUL), keeping printable
// ASCII only and trimming spaces. Returns the length; 0 = nothing to show.
size_t ui_copy_printable(char *dst, size_t cap, const char *src, size_t src_size);

// ---- Alarms (0x8B) ----
// The Alarms page is in the ring
bool ui_alarms_active(const ui_model_t *m);
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
