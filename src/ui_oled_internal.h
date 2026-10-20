// Private interface of the 64x128 monochrome OLED UI (ui_oled.c and
// ui_oled_pages.c). Everything here is called with the LVGL lock held.
//
// 1 bit per pixel: anything lighter than mid-grey is lit. There is no dim
// colour, so state is shown by inverting (white box, black text) or outlining.
// unscii_8 is 8x8 pixels on a 9 px line: 8 characters per line, ASCII only.
#pragma once

#include "ui_common.h"

#define OLED_FONT       (&lv_font_unscii_8)
#define OLED_FONT_BIG   (&lv_font_unscii_16)    // 16 px per character, 17 px line
#define OLED_W          64
#define OLED_H          128
#define OLED_LINE       9                       // unscii_8 line height
#define OLED_CHARS      8                       // characters per line
#define OLED_TOP        10                      // content starts below the status strip

#define OLED_WHITE      0xFFFFFF
#define OLED_BLACK      0x000000

// ---- Widgets ----
// One unscii_8 line, white on black, CLIP
lv_obj_t *oled_text_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, lv_text_align_t align);
// Black text on a white background (true), or white on black
void oled_set_inverted(lv_obj_t *obj, bool inverted);

// Tag: a w x h box with a centred one-line label, plain, inverted or outlined
// (1 px white frame). The frame is always there and only its opacity changes:
// a border moves the children, so this way the text never moves.
typedef enum {
    OLED_TAG_PLAIN = 0,
    OLED_TAG_INVERTED,
    OLED_TAG_OUTLINED,
} oled_tag_style_t;
lv_obj_t *oled_tag_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);
lv_obj_t *oled_tag_label(lv_obj_t *tag);
void oled_tag_set(lv_obj_t *tag, oled_tag_style_t style);

// ---- Formatting: every line is at most OLED_CHARS characters ----
// key left, value right-aligned: ("Cyc", "123") -> "Cyc  123". A value too long
// for the line follows the key directly and is clipped.
void oled_kv(char *buf, size_t cap, const char *key, const char *value);
// oled_kv() with a number that is never clipped: "Nom280Ah", else without the
// unit ("Nom 1000"), else in k, M or G ("CyAh 35k")
void oled_kv_u32(char *buf, size_t cap, const char *key, uint32_t value, const char *unit);
// "T 24/23C" for the reported battery sensors (T1, T2), the "C" dropped if it
// does not fit. Returns false (and writes "T " UI_NA) if neither is reported.
bool oled_temps_line(char *buf, size_t cap, const ui_view_t *v);
// Alarm name of at most 8 characters ("Cell OV"; "Bit 14" for unnamed bits)
const char *oled_alarm_name(unsigned bit, char *buf, size_t cap);

// ---- Pages ----
// create: build the page on its own screen (once, from ui_build) and return it.
// update: refresh the page; called only while it is visible (and once at build).
lv_obj_t *oled_main_create(void);
void oled_main_update(const ui_view_t *v);
// Cells come in sub-pages of up to OLED_CELL_ROWS, balanced (16S -> 8 + 8)
#define OLED_CELL_ROWS  12
unsigned oled_cells_pages(const ui_view_t *v);
lv_obj_t *oled_cells_create(void);
void oled_cells_update(const ui_view_t *v, unsigned sub_page);
lv_obj_t *oled_alarms_create(void);
void oled_alarms_update(const ui_view_t *v);
lv_obj_t *oled_details_create(void);
void oled_details_update(const ui_view_t *v);
