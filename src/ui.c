// Dashboard UI core: page ring, status strip, overlay card and the helpers
// shared by the page files. Every page is its own screen, created once; the
// strip and the card live on lv_layer_top() so screen loads never delete them.
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "app_config.h"
#include "jk_bms.h"
#include "ui_internal.h"

static const char *TAG = "ui";

// ---- Status strip on lv_layer_top(), y 0-15 ----
#define DOT_SIZE        6       // page dots
#define DOT_X0          4
#define DOT_PITCH       10
#define DOT_Y           5
#define LINK_SIZE       8       // BMS link dot, centered on x=110
#define LINK_X          106
#define LINK_Y          4
#define VBAT_X          150     // board battery, right-aligned to x=236
#define VBAT_W          86
#define VBAT_BLINK_MS   500

// ---- Overlay card on lv_layer_top() ----
#define CARD_X          16
#define CARD_Y          90
#define CARD_W          208
#define CARD_H          60
#define CARD_BORDER     2
#define CARD_RADIUS     10
#define CARD_TEXT_W     196     // 20 px if the text fits one line, else 14 px (up to 2 lines)
#define CARD_BAR_W      180
#define CARD_BAR_H      8
#define CARD_HOLD_SHIFT 7       // hold card: text moved up, bar this far above the bottom

static lv_obj_t *s_screen[UI_PAGE_COUNT];
static ui_page_t s_page = UI_PAGE_MAIN;

static lv_obj_t *s_dot[UI_PAGE_COUNT];
static lv_obj_t *s_link;
static lv_obj_t *s_vbat;
static lv_timer_t *s_blink_timer;
static bool s_blinking;

static lv_obj_t *s_card;
static lv_obj_t *s_card_text;
static lv_obj_t *s_card_bar;
static lv_timer_t *s_card_timer;
static ui_overlay_kind_t s_card_kind = UI_OVL_NONE;
static bool s_card_hold_layout;

static void (*const s_page_update[UI_PAGE_COUNT])(const ui_view_t *v) = {
    [UI_PAGE_MAIN]    = ui_main_update,
    [UI_PAGE_CELLS]   = ui_cells_update,
    [UI_PAGE_ALARMS]  = ui_alarms_update,
    [UI_PAGE_DETAILS] = ui_details_update,
};

// ===================== Widget factory =====================

lv_obj_t *ui_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_scrollable(scr, false);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
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
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
    lv_obj_set_pos(label, x, y);
    // Fixed height too: a text change then never triggers a relayout
    lv_obj_set_size(label, w, lv_font_get_line_height(font));
    lv_label_set_text(label, "");
    return label;
}

lv_obj_t *ui_bar_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_DIM), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    return bar;
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

// ===================== Strings and formatting =====================

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

bool ui_str_add_cell_range(ui_str_t *s, const jk_data_t *d)
{
    if (!(d->present & JK_HAS_CELLS) || d->cells_valid == 0) {
        return false;
    }
    ui_str_add_mv(s, d->cell_min_mv);
    ui_str_add(s, "-");
    ui_str_add_mv(s, d->cell_max_mv);
    return true;
}

// |v|, also for INT32_MIN
static uint32_t abs_u32(int32_t v)
{
    return v < 0 ? (uint32_t)0 - (uint32_t)v : (uint32_t)v;
}

void ui_str_add_current(ui_str_t *s, int32_t current_10ma)
{
    uint32_t a = abs_u32(current_10ma);
    const char *sign = current_10ma < 0 ? "-" : "";
    if (a < 10000) {
        ui_str_add(s, "%s%" PRIu32 ".%02" PRIu32 " A", sign, a / 100, a % 100);
    } else {
        uint32_t t = (a + 5) / 10;      // 0.1 A, rounded
        if (t > 9999) {
            t = 9999;                   // label width; the decoder cannot exceed 555.4 A
        }
        ui_str_add(s, "%s%" PRIu32 ".%" PRIu32 " A", sign, t / 10, t % 10);
    }
}

void ui_str_add_power(ui_str_t *s, int32_t power_dw)
{
    uint32_t a = abs_u32(power_dw);
    uint32_t w = (a + 5) / 10;          // 1 W, rounded
    const char *sign = (power_dw < 0 && w > 0) ? "-" : "";
    if (w < 10000) {
        ui_str_add(s, "%s%" PRIu32 " W", sign, w);
    } else {
        uint32_t t = (a + 500) / 1000;  // 0.1 kW, rounded
        if (t > 9999) {
            t = 9999;                   // label width; 655 V x 555 A is only 364 kW
        }
        ui_str_add(s, "%s%" PRIu32 ".%" PRIu32 " kW", sign, t / 10, t % 10);
    }
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

static void view_init(ui_view_t *v, const ui_model_t *m)
{
    v->m = m;
    v->d = &m->bms.data;
    v->has_data = m->bms.has_data;
    v->live = m->bms.has_data && !m->bms.offline;
}

// ===================== Status strip =====================

static lv_obj_t *dot_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t size)
{
    lv_obj_t *dot = ui_box_create(parent, x, y, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(UI_COLOR_DIM), LV_PART_MAIN);
    return dot;
}

// LVGL task context (lock held)
static void blink_cb(lv_timer_t *t)
{
    (void)t;
    lv_obj_set_hidden(s_vbat, !lv_obj_is_hidden(s_vbat));
}

static void strip_create(lv_obj_t *top)
{
    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        s_dot[i] = dot_create(top, DOT_X0 + DOT_PITCH * i, DOT_Y, DOT_SIZE);
    }
    s_link = dot_create(top, LINK_X, LINK_Y, LINK_SIZE);
    s_vbat = ui_label_create(top, UI_FONT_14, VBAT_X, 0, VBAT_W, LV_TEXT_ALIGN_RIGHT);
    ui_label_set(s_vbat, UI_NA "V");
    s_blink_timer = lv_timer_create(blink_cb, VBAT_BLINK_MS, NULL);
    if (s_blink_timer != NULL) {
        lv_timer_pause(s_blink_timer);
    }
}

static uint32_t link_color(const ui_model_t *m)
{
    if (m->bms.offline) {
        return UI_COLOR_RED;
    }
    if (!m->bms.has_data) {
        return UI_COLOR_DIM;            // connecting
    }
    int64_t age_us = m->now_us - m->bms.last_ok_us;
    return age_us < 2LL * APP_POLL_INTERVAL_MS * 1000 ? UI_COLOR_GREEN : UI_COLOR_AMBER;
}

static const char *vbat_symbol(int mv)
{
    if (mv >= 4000) {
        return LV_SYMBOL_BATTERY_FULL;
    }
    if (mv >= 3850) {
        return LV_SYMBOL_BATTERY_3;
    }
    if (mv >= 3700) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (mv >= 3550) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void vbat_update(const ui_model_t *m)
{
    if (m->vbat_mv < 0) {
        ui_label_set(s_vbat, UI_NA "V");
    } else {
        unsigned cv = ((unsigned)m->vbat_mv + 5) / 10;  // 10 mV, rounded
        ui_label_setf(s_vbat, "%s %u.%02uV", vbat_symbol(m->vbat_mv), cv / 100, cv % 100);
    }
    ui_set_text_color(s_vbat, m->vbat_low ? UI_COLOR_RED : UI_COLOR_TEXT);

    // Start/stop the blink only on a change, so the 500 ms phase keeps running
    if (m->vbat_low && !s_blinking && s_blink_timer != NULL) {
        s_blinking = true;
        lv_timer_reset(s_blink_timer);
        lv_timer_resume(s_blink_timer);
    } else if (!m->vbat_low && s_blinking) {
        s_blinking = false;
        lv_timer_pause(s_blink_timer);
        ui_set_hidden(s_vbat, false);
    }
}

static void strip_update(const ui_model_t *m)
{
    bool alarms = ui_alarms_active(m);
    unsigned count = 0;
    unsigned index = 0;
    for (int p = 0; p < UI_PAGE_COUNT; p++) {
        if (p == UI_PAGE_ALARMS && !alarms) {
            continue;
        }
        if (p == (int)s_page) {
            index = count;
        }
        count++;
    }
    for (unsigned i = 0; i < UI_PAGE_COUNT; i++) {
        ui_set_hidden(s_dot[i], i >= count);
        ui_set_bg_color(s_dot[i], LV_PART_MAIN, i == index ? UI_COLOR_TEXT : UI_COLOR_DIM);
    }
    ui_set_bg_color(s_link, LV_PART_MAIN, link_color(m));
    vbat_update(m);
}

// ===================== Overlay card =====================

static void card_hide(void)
{
    if (s_card_timer != NULL) {
        lv_timer_pause(s_card_timer);
    }
    ui_set_hidden(s_card, true);
    s_card_kind = UI_OVL_NONE;
}

// LVGL task context (lock held): one-shot toast timeout
static void card_timer_cb(lv_timer_t *t)
{
    (void)t;
    ESP_LOGD(TAG, "overlay timeout");
    card_hide();
}

static void card_create(lv_obj_t *top)
{
    s_card = ui_box_create(top, CARD_X, CARD_Y, CARD_W, CARD_H);
    lv_obj_set_style_radius(s_card, CARD_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(UI_COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_card, CARD_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_card, lv_color_hex(UI_COLOR_DIM), LV_PART_MAIN);

    // The one wrapping label: several messages need two lines even at 14 px
    s_card_text = lv_label_create(s_card);
    lv_label_set_long_mode(s_card_text, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_max_lines(s_card_text, 2);
    lv_obj_set_width(s_card_text, CARD_TEXT_W);
    lv_obj_set_style_text_align(s_card_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_card_text, UI_FONT_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_card_text, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    lv_label_set_text(s_card_text, "");
    lv_obj_align(s_card_text, LV_ALIGN_CENTER, 0, 0);

    s_card_bar = ui_bar_create(s_card, 0, 0, CARD_BAR_W, CARD_BAR_H);
    lv_obj_align(s_card_bar, LV_ALIGN_BOTTOM_MID, 0, -CARD_HOLD_SHIFT);
    lv_obj_set_hidden(s_card_bar, true);
    s_card_hold_layout = false;

    lv_obj_set_hidden(s_card, true);
    s_card_kind = UI_OVL_NONE;

    s_card_timer = lv_timer_create(card_timer_cb, 1000, NULL);
    if (s_card_timer != NULL) {
        lv_timer_pause(s_card_timer);
    }
}

static bool card_fits_one_line(const char *text, const lv_font_t *font)
{
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.y <= lv_font_get_line_height(font) && size.x <= CARD_TEXT_W;
}

// Text too long for one 14 px line: break it at the space nearest the middle,
// so the two lines are balanced (the greedy wrap would leave a lone last word).
static const char *card_balance(const char *text, char *buf, size_t cap)
{
    size_t len = strlen(text);
    if (len + 1 > cap || strchr(text, '\n') != NULL || card_fits_one_line(text, UI_FONT_14)) {
        return text;
    }
    memcpy(buf, text, len + 1);
    size_t best = 0;
    size_t best_dist = SIZE_MAX;
    for (size_t i = 1; i + 1 < len; i++) {
        size_t dist = 2 * i > len ? 2 * i - len : len - 2 * i;
        if (buf[i] == ' ' && dist <= best_dist) {
            best = i;
            best_dist = dist;
        }
    }
    if (best == 0) {
        return text;
    }
    buf[best] = '\n';
    return buf;
}

static void card_show(ui_overlay_kind_t kind, const char *text, bool error, bool hold, uint8_t pct)
{
    if (text == NULL) {
        text = "";
    }
    bool kind_changed = kind != s_card_kind;
    s_card_kind = kind;

    lv_color_t border = lv_color_hex(error ? UI_COLOR_RED : UI_COLOR_DIM);
    if (!lv_color_eq(lv_obj_get_style_border_color(s_card, LV_PART_MAIN), border)) {
        lv_obj_set_style_border_color(s_card, border, LV_PART_MAIN);
    }
    ui_set_text_color(s_card_text, error ? UI_COLOR_RED : UI_COLOR_TEXT);

    // 20 px when the text fits one line of the card, else 14 px on up to 2 lines
    char buf[96];
    const char *shown = text;
    if (card_fits_one_line(text, UI_FONT_20)) {
        ui_set_font(s_card_text, UI_FONT_20);
    } else {
        ui_set_font(s_card_text, UI_FONT_14);
        shown = card_balance(text, buf, sizeof(buf));
    }
    if (ui_label_set(s_card_text, shown) || kind_changed) {
        ESP_LOGD(TAG, "overlay %d: %s", (int)kind, text);
    }

    if (hold != s_card_hold_layout) {
        s_card_hold_layout = hold;
        lv_obj_align(s_card_text, LV_ALIGN_CENTER, 0, hold ? -CARD_HOLD_SHIFT : 0);
        ui_set_hidden(s_card_bar, !hold);
    }
    if (hold) {
        int32_t p = pct > 100 ? 100 : pct;
        lv_bar_set_value(s_card_bar, p, LV_ANIM_OFF);   // no-op when unchanged
        ui_set_bg_color(s_card_bar, LV_PART_INDICATOR, p >= 100 ? UI_COLOR_GREEN : UI_COLOR_AMBER);
    }
    ui_set_hidden(s_card, false);
}

void ui_overlay_hold(const char *text, uint8_t progress_pct)
{
    if (s_card == NULL) {
        return;
    }
    if (s_card_timer != NULL) {
        lv_timer_pause(s_card_timer);   // the hold card never times out
    }
    card_show(UI_OVL_HOLD, text, false, true, progress_pct);
}

void ui_overlay_message(ui_overlay_kind_t kind, const char *text, bool error, uint32_t duration_ms)
{
    if (s_card == NULL || kind == UI_OVL_NONE) {
        return;
    }
    if (s_card_kind > kind) {
        return;                         // a higher-priority card is visible
    }
    card_show(kind, text, error, false, 0);
    if (s_card_timer == NULL) {
        return;
    }
    if (duration_ms > 0) {
        lv_timer_set_period(s_card_timer, duration_ms);
        lv_timer_reset(s_card_timer);
        lv_timer_resume(s_card_timer);
    } else {
        lv_timer_pause(s_card_timer);
    }
}

void ui_overlay_hide(ui_overlay_kind_t max_kind)
{
    if (s_card == NULL || s_card_kind == UI_OVL_NONE || s_card_kind > max_kind) {
        return;
    }
    card_hide();
}

ui_overlay_kind_t ui_overlay_current(void)
{
    return s_card_kind;
}

// ===================== Pages =====================

static void show_page(ui_page_t page)
{
    if (page != s_page) {
        ESP_LOGD(TAG, "page %d", (int)page);
    }
    s_page = page;
    lv_screen_load(s_screen[page]);     // no animation; no-op if already active
}

void ui_build(void)
{
    if (s_screen[UI_PAGE_MAIN] != NULL) {
        ESP_LOGW(TAG, "ui_build called twice");
        return;
    }
    s_screen[UI_PAGE_MAIN] = ui_main_create();
    s_screen[UI_PAGE_CELLS] = ui_cells_create();
    s_screen[UI_PAGE_ALARMS] = ui_alarms_create();
    s_screen[UI_PAGE_DETAILS] = ui_details_create();

    lv_obj_t *top = lv_layer_top();
    lv_obj_set_scrollable(top, false);
    strip_create(top);
    card_create(top);

    // The BMS task starts before the display: show what it already has, so
    // the first rendered frame usually carries data instead of "--".
    ui_model_t m = {};
    jk_bms_get_snapshot(&m.bms);
    m.vbat_mv = -1;
    m.vbat_low = false;
    m.now_us = esp_timer_get_time();

    ui_view_t v;
    view_init(&v, &m);
    for (int p = 0; p < UI_PAGE_COUNT; p++) {
        s_page_update[p](&v);
    }
    show_page(UI_PAGE_MAIN);
    strip_update(&m);
    ESP_LOGI(TAG, "UI built (BMS data: %s)", m.bms.has_data ? "yes" : "not yet");
}

void ui_update(const ui_model_t *m)
{
    if (m == NULL || s_screen[UI_PAGE_MAIN] == NULL) {
        return;
    }
    if (s_page == UI_PAGE_ALARMS && !ui_alarms_active(m)) {
        show_page(UI_PAGE_MAIN);        // the Alarms page left the ring
    }
    ui_view_t v;
    view_init(&v, m);
    strip_update(m);
    s_page_update[s_page](&v);
}

void ui_next_page(const ui_model_t *m)
{
    if (s_screen[UI_PAGE_MAIN] == NULL) {
        return;
    }
    bool alarms = m != NULL && ui_alarms_active(m);
    int p = (int)s_page;
    do {
        p = (p + 1) % UI_PAGE_COUNT;
    } while (p == UI_PAGE_ALARMS && !alarms);
    show_page((ui_page_t)p);
    ui_update(m);
}

void ui_show_main(const ui_model_t *m)
{
    if (s_screen[UI_PAGE_MAIN] == NULL) {
        return;
    }
    show_page(UI_PAGE_MAIN);
    ui_update(m);
}

ui_page_t ui_current_page(void)
{
    return s_page;
}
