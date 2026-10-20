// Helpers shared by the TFT and OLED UIs (see ui_common.h).
#include "ui_common.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define COLOR_BG    0x000000
#define COLOR_TEXT  0xFFFFFF

void ui_view_init(ui_view_t *v, const ui_model_t *m)
{
    v->m = m;
    v->d = &m->bms.data;
    v->has_data = m->bms.has_data;
    v->live = m->bms.has_data && !m->bms.offline;
}

// ===================== Widget factory =====================

lv_obj_t *ui_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_scrollable(scr, false);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    return scr;
}

lv_obj_t *ui_box_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_clickable(box, false);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    return box;
}

lv_obj_t *ui_label_create(lv_obj_t *parent, const lv_font_t *font, int32_t x, int32_t y, int32_t w,
                          lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    // CLIP never wraps (single line) and clips the text to the label box
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
    lv_obj_set_pos(label, x, y);
    // Fixed height too: a text change then never triggers a relayout
    lv_obj_set_size(label, w, lv_font_get_line_height(font));
    lv_label_set_text(label, "");
    return label;
}

// ===================== Change-only setters =====================

bool ui_label_set(lv_obj_t *label, const char *text)
{
    if (text == NULL) {
        text = "";
    }
    const char *cur = lv_label_get_text(label);
    if (cur != NULL && strcmp(cur, text) == 0) {
        return false;
    }
    lv_label_set_text(label, text);
    return true;
}

bool ui_label_setf(lv_obj_t *label, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        buf[0] = '\0';
    }
    return ui_label_set(label, buf);
}

void ui_set_text_color(lv_obj_t *obj, uint32_t rgb)
{
    lv_color_t c = lv_color_hex(rgb);
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), c)) {
        lv_obj_set_style_text_color(obj, c, LV_PART_MAIN);
    }
}

void ui_set_bg_color(lv_obj_t *obj, lv_part_t part, uint32_t rgb)
{
    lv_color_t c = lv_color_hex(rgb);
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, part), c)) {
        lv_obj_set_style_bg_color(obj, c, part);
    }
}

void ui_set_font(lv_obj_t *obj, const lv_font_t *font)
{
    if (lv_obj_get_style_text_font(obj, LV_PART_MAIN) != font) {
        lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
    }
}

void ui_set_hidden(lv_obj_t *obj, bool hidden)
{
    if (lv_obj_is_hidden(obj) != hidden) {
        lv_obj_set_hidden(obj, hidden);
    }
}

// ===================== Strings =====================

void ui_str_init(ui_str_t *s, char *buf, size_t cap)
{
    s->buf = buf;
    s->cap = cap;
    s->len = 0;
    if (cap > 0) {
        buf[0] = '\0';
    }
}

void ui_str_add(ui_str_t *s, const char *fmt, ...)
{
    if (s->cap == 0 || s->len + 1 >= s->cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        s->buf[s->len] = '\0';
        return;
    }
    s->len += (size_t)n;
    if (s->len > s->cap - 1) {
        s->len = s->cap - 1;    // truncated; vsnprintf terminated it
    }
}

void ui_str_add_mv(ui_str_t *s, uint32_t mv)
{
    ui_str_add(s, "%" PRIu32 ".%03" PRIu32, mv / 1000, mv % 1000);
}

bool ui_temp_get(const jk_data_t *d, unsigned sensor, int *out_c)
{
    // 0x86 (battery sensor count) limits T1/T2 when it is reported
    unsigned count = (d->present & JK_HAS_TEMP_COUNT) ? d->temp_sensor_count : 2;
    switch (sensor) {
    case 0:
        if (!(d->present & JK_HAS_TEMP_MOS)) {
            return false;
        }
        *out_c = d->temp_mos_c;
        return true;
    case 1:
        if (!(d->present & JK_HAS_TEMP_1) || count < 1) {
            return false;
        }
        *out_c = d->temp_1_c;
        return true;
    case 2:
        if (!(d->present & JK_HAS_TEMP_2) || count < 2) {
            return false;
        }
        *out_c = d->temp_2_c;
        return true;
    default:
        return false;
    }
}

size_t ui_copy_printable(char *dst, size_t cap, const char *src, size_t src_size)
{
    if (cap == 0) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < src_size && src[i] != '\0' && n + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20 || c > 0x7E) {
            continue;                   // no glyph in the fonts
        }
        if (c == ' ' && n == 0) {
            continue;                   // leading space
        }
        dst[n++] = (char)c;
    }
    while (n > 0 && dst[n - 1] == ' ') {
        n--;
    }
    dst[n] = '\0';
    return n;
}

// ===================== Alarms =====================

bool ui_alarms_active(const ui_model_t *m)
{
    return m->bms.has_data && (m->bms.data.present & JK_HAS_ALARMS) && m->bms.data.alarms != 0;
}

#define NAMED_ALARMS ((1u << JK_ALARM_BITS) - 1)

unsigned ui_alarm_count(uint16_t alarms)
{
    return (unsigned)__builtin_popcount(alarms);
}

bool ui_alarm_is_warning(unsigned bit)
{
    return bit < JK_ALARM_BITS && jk_alarm_is_warning(bit);
}

int ui_alarm_top(uint16_t alarms)
{
    int top = jk_alarm_top((uint16_t)(alarms & NAMED_ALARMS));
    unsigned unnamed = alarms & ~NAMED_ALARMS;
    if (unnamed != 0 && (top < 0 || ui_alarm_is_warning((unsigned)top))) {
        top = __builtin_ctz(unnamed);
    }
    if (top < 0 && alarms != 0) {
        top = __builtin_ctz(alarms);
    }
    return top;
}

const char *ui_alarm_name(unsigned bit, bool short_name, char *buf, size_t cap)
{
    const char *name = NULL;
    if (bit < JK_ALARM_BITS) {
        name = short_name ? jk_alarm_short_name(bit) : jk_alarm_name(bit);
    }
    if (name == NULL) {
        snprintf(buf, cap, "Alarm bit %u", bit);
        name = buf;
    }
    return name;
}
