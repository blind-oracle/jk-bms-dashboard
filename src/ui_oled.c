// OLED UI core (64x128, 1 bit): page ring, status strip, overlay card, wording
// and the helpers shared by the pages. Every page is its own screen, created
// once; the strip and the card live on lv_layer_top() so screen loads never
// delete them. The cells sub-pages share one screen.
//
// Ring: Main -> Cells 1..k -> [Alarms] -> Details. ui_current_page() reports
// UI_PAGE_CELLS on any cells sub-page, so main.c sees the same four pages as
// with the TFT.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "app_config.h"
#include "jk_bms.h"
#include "ui_oled_internal.h"

static const char *TAG = "ui";

// ---- Status strip on lv_layer_top(), y 0-8: "2/5", link marker, "3.92" ----
#define PAGE_W          24
#define LINK_X          26      // 5x5 box: filled = fresh data, outline = stale
#define LINK_Y          2
#define LINK_SIZE       5
#define LINK_TXT_X      24      // instead of the box: "?" connecting, "x" offline
#define LINK_TXT_W      8
#define VBAT_X          32      // board battery; inverted and blinking when low
#define VBAT_W          32
#define VBAT_BLINK_MS   500

// ---- Overlay card: full width, centred, up to CARD_LINES_MAX wrapped lines ----
// Rows: border, 2 px, lines x 9 px, 2 px, border; the hold card adds the bar
// and 2 px below it. Normal: black on white. Error: white on black.
#define CARD_LINES_MAX  5
#define CARD_TEXT_Y     1       // below the top border; the font's blank top row adds 1 px
#define CARD_PAD        4       // card height = lines * OLED_LINE + CARD_PAD
#define CARD_BAR_X      4       // the bar is CARD_TEXT_Y + 1 below the text
#define CARD_BAR_W      56
#define CARD_BAR_H      4
#define CARD_HOLD_EXTRA 6       // bar + 2 px

static lv_obj_t *s_screen[UI_PAGE_COUNT];
static ui_page_t s_page = UI_PAGE_MAIN;
static unsigned s_sub;          // cells sub-page

static lv_obj_t *s_page_label;
static lv_obj_t *s_link_box;
static lv_obj_t *s_link_text;
static lv_obj_t *s_vbat;
static lv_timer_t *s_blink_timer;
static bool s_blinking;

static lv_obj_t *s_card;
static lv_obj_t *s_card_text;
static lv_obj_t *s_card_bar;
static lv_timer_t *s_card_timer;
static ui_overlay_kind_t s_card_kind = UI_OVL_NONE;

// ===================== Widgets =====================

lv_obj_t *oled_text_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, lv_text_align_t align)
{
    return ui_label_create(parent, OLED_FONT, x, y, w, align);
}

void oled_set_inverted(lv_obj_t *obj, bool inverted)
{
    lv_opa_t opa = inverted ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) != opa) {
        lv_obj_set_style_bg_opa(obj, opa, LV_PART_MAIN);
    }
    ui_set_bg_color(obj, LV_PART_MAIN, OLED_WHITE);
    ui_set_text_color(obj, inverted ? OLED_BLACK : OLED_WHITE);
}

lv_obj_t *oled_tag_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *tag = ui_box_create(parent, x, y, w, h);
    lv_obj_set_style_bg_color(tag, lv_color_hex(OLED_WHITE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tag, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(tag, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(tag, lv_color_hex(OLED_WHITE), LV_PART_MAIN);
    lv_obj_set_style_border_opa(tag, LV_OPA_TRANSP, LV_PART_MAIN);
    // Child positions start inside the border: (-1, -1) is the tag's corner
    oled_text_create(tag, -1, -1, w, LV_TEXT_ALIGN_CENTER);
    return tag;
}

lv_obj_t *oled_tag_label(lv_obj_t *tag)
{
    return lv_obj_get_child(tag, 0);
}

void oled_tag_set(lv_obj_t *tag, oled_tag_style_t style)
{
    lv_opa_t bg = style == OLED_TAG_INVERTED ? LV_OPA_COVER : LV_OPA_TRANSP;
    lv_opa_t frame = style == OLED_TAG_OUTLINED ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (lv_obj_get_style_bg_opa(tag, LV_PART_MAIN) != bg) {
        lv_obj_set_style_bg_opa(tag, bg, LV_PART_MAIN);
    }
    if (lv_obj_get_style_border_opa(tag, LV_PART_MAIN) != frame) {
        lv_obj_set_style_border_opa(tag, frame, LV_PART_MAIN);
    }
    ui_set_text_color(oled_tag_label(tag), style == OLED_TAG_INVERTED ? OLED_BLACK : OLED_WHITE);
}

static void set_y(lv_obj_t *obj, int32_t y)
{
    if (lv_obj_get_y(obj) != y) {
        lv_obj_set_y(obj, y);
    }
}

static void set_height(lv_obj_t *obj, int32_t h)
{
    if (lv_obj_get_height(obj) != h) {
        lv_obj_set_height(obj, h);
    }
}

// ===================== Formatting =====================

void oled_kv(char *buf, size_t cap, const char *key, const char *value)
{
    int pad = OLED_CHARS - (int)strlen(key);
    if (pad < (int)strlen(value)) {
        pad = 0;                        // too long: the label clips it
    }
    snprintf(buf, cap, "%s%*s", key, pad, value);
}

void oled_kv_u32(char *buf, size_t cap, const char *key, uint32_t value, const char *unit)
{
    static const struct {
        uint32_t div;
        const char *suffix;
    } steps[] = {{1000, "k"}, {1000000, "M"}, {1000000000, "G"}};
    const size_t room = OLED_CHARS - strlen(key);
    char num[16];
    snprintf(num, sizeof(num), "%" PRIu32 "%s", value, unit);
    if (strlen(num) > room) {
        snprintf(num, sizeof(num), "%" PRIu32, value);
    }
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]) && strlen(num) > room; i++) {
        snprintf(num, sizeof(num), "%" PRIu32 "%s", (uint32_t)(((uint64_t)value + steps[i].div / 2) / steps[i].div),
                 steps[i].suffix);
    }
    oled_kv(buf, cap, key, num);
}

bool oled_temps_line(char *buf, size_t cap, const ui_view_t *v)
{
    char value[24];
    ui_str_t str;
    ui_str_init(&str, value, sizeof(value));
    for (unsigned i = 1; i < UI_TEMP_SENSORS && v->has_data; i++) {
        int t;
        if (ui_temp_get(v->d, i, &t)) {
            ui_str_add(&str, "%s%d", str.len > 0 ? "/" : "", t);
        }
    }
    if (str.len == 0) {
        oled_kv(buf, cap, "T", UI_NA);
        return false;
    }
    if (str.len + 2 <= OLED_CHARS) {    // "T" + value + "C"
        ui_str_add(&str, "C");
    }
    oled_kv(buf, cap, "T", value);
    return true;
}

static const char *const k_alarm_names[JK_ALARM_BITS] = {
    [0]  = "Low cap",
    [1]  = "MOS hot",
    [2]  = "Chg OV",
    [3]  = "Dsg UV",
    [4]  = "Bat hot",
    [5]  = "Chg OC",
    [6]  = "Dsg OC",
    [7]  = "Cell dif",
    [8]  = "Box hot",
    [9]  = "Bat cold",
    [10] = "Cell OV",
    [11] = "Cell UV",
    [12] = "309_A",
    [13] = "309_B",
};

const char *oled_alarm_name(unsigned bit, char *buf, size_t cap)
{
    if (bit < JK_ALARM_BITS && k_alarm_names[bit] != NULL) {
        return k_alarm_names[bit];
    }
    snprintf(buf, cap, "Bit %u", bit);
    return buf;
}

// ===================== Ring =====================

static unsigned ring_count(unsigned cell_pages, bool alarms)
{
    return 1 + cell_pages + (alarms ? 1 : 0) + 1;
}

// 0-based position of the visible page in the ring
static unsigned ring_index(unsigned cell_pages, bool alarms)
{
    switch (s_page) {
    case UI_PAGE_MAIN:
        return 0;
    case UI_PAGE_CELLS:
        return 1 + s_sub;
    case UI_PAGE_ALARMS:
        return 1 + cell_pages;
    default:
        return 1 + cell_pages + (alarms ? 1 : 0);
    }
}

// ===================== Status strip =====================

typedef enum {
    LINK_CONNECTING = 0,
    LINK_FRESH,
    LINK_STALE,
    LINK_OFFLINE,
} link_t;

// LVGL task context (lock held)
static void blink_cb(lv_timer_t *t)
{
    (void)t;
    lv_obj_set_hidden(s_vbat, !lv_obj_is_hidden(s_vbat));
}

static void strip_create(lv_obj_t *top)
{
    s_page_label = oled_text_create(top, 0, 0, PAGE_W, LV_TEXT_ALIGN_LEFT);

    s_link_box = ui_box_create(top, LINK_X, LINK_Y, LINK_SIZE, LINK_SIZE);
    lv_obj_set_style_bg_color(s_link_box, lv_color_hex(OLED_WHITE), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_link_box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_link_box, lv_color_hex(OLED_WHITE), LV_PART_MAIN);
    lv_obj_set_hidden(s_link_box, true);
    s_link_text = oled_text_create(top, LINK_TXT_X, 0, LINK_TXT_W, LV_TEXT_ALIGN_CENTER);

    s_vbat = oled_text_create(top, VBAT_X, 0, VBAT_W, LV_TEXT_ALIGN_RIGHT);
    ui_label_set(s_vbat, "-." UI_NA);
    s_blink_timer = lv_timer_create(blink_cb, VBAT_BLINK_MS, NULL);
    if (s_blink_timer != NULL) {
        lv_timer_pause(s_blink_timer);
    }
}

static link_t link_state(const ui_model_t *m)
{
    if (m->bms.offline) {
        return LINK_OFFLINE;
    }
    if (!m->bms.has_data) {
        return LINK_CONNECTING;
    }
    int64_t age_us = m->now_us - m->bms.last_ok_us;
    return age_us < 2LL * APP_POLL_INTERVAL_MS * 1000 ? LINK_FRESH : LINK_STALE;
}

static void link_update(const ui_model_t *m)
{
    link_t link = link_state(m);
    bool box = link == LINK_FRESH || link == LINK_STALE;
    ui_set_hidden(s_link_box, !box);
    if (box) {
        lv_opa_t opa = link == LINK_FRESH ? LV_OPA_COVER : LV_OPA_TRANSP;
        if (lv_obj_get_style_bg_opa(s_link_box, LV_PART_MAIN) != opa) {
            lv_obj_set_style_bg_opa(s_link_box, opa, LV_PART_MAIN);
        }
    }
    ui_label_set(s_link_text, link == LINK_OFFLINE ? "x" : link == LINK_CONNECTING ? "?" : "");
}

static void vbat_update(const ui_model_t *m)
{
    if (m->vbat_mv < 0) {
        ui_label_set(s_vbat, "-." UI_NA);
    } else {
        unsigned cv = ((unsigned)m->vbat_mv + 5) / 10;  // 10 mV, rounded
        if (cv > 999) {
            cv = 999;                   // 4 characters
        }
        ui_label_setf(s_vbat, "%u.%02u", cv / 100, cv % 100);
    }
    oled_set_inverted(s_vbat, m->vbat_low);

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

static void strip_update(const ui_model_t *m, unsigned cell_pages, bool alarms)
{
    ui_label_setf(s_page_label, "%u/%u", ring_index(cell_pages, alarms) + 1, ring_count(cell_pages, alarms));
    link_update(m);
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
    s_card = ui_box_create(top, 0, 0, OLED_W, OLED_LINE + CARD_PAD);
    lv_obj_set_style_border_width(s_card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(s_card, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);

    // Full width: 8 characters per line. Words of the OLED wording are at most 8.
    s_card_text = lv_label_create(s_card);
    lv_label_set_long_mode(s_card_text, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_max_lines(s_card_text, CARD_LINES_MAX);
    lv_obj_set_pos(s_card_text, 0, CARD_TEXT_Y);
    lv_obj_set_size(s_card_text, OLED_W, OLED_LINE);
    lv_obj_set_style_text_align(s_card_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_card_text, OLED_FONT, LV_PART_MAIN);
    lv_label_set_text(s_card_text, "");

    // Black outline on the white card, filled black as the hold progresses
    s_card_bar = lv_bar_create(s_card);
    lv_obj_remove_style_all(s_card_bar);
    lv_obj_set_pos(s_card_bar, CARD_BAR_X, 0);
    lv_obj_set_size(s_card_bar, CARD_BAR_W, CARD_BAR_H);
    lv_bar_set_range(s_card_bar, 0, 100);
    lv_bar_set_value(s_card_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_border_width(s_card_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_card_bar, lv_color_hex(OLED_BLACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_card_bar, lv_color_hex(OLED_BLACK), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_card_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_hidden(s_card_bar, true);

    lv_obj_set_hidden(s_card, true);
    s_card_kind = UI_OVL_NONE;

    s_card_timer = lv_timer_create(card_timer_cb, 1000, NULL);
    if (s_card_timer != NULL) {
        lv_timer_pause(s_card_timer);
    }
}

static int32_t card_lines(const char *text)
{
    lv_point_t size;
    lv_text_get_size(&size, text, OLED_FONT, 0, 0, OLED_W, LV_TEXT_FLAG_NONE);
    int32_t lines = size.y / OLED_LINE;
    if (lines < 1) {
        lines = 1;
    }
    return lines > CARD_LINES_MAX ? CARD_LINES_MAX : lines;
}

static void card_show(ui_overlay_kind_t kind, const char *text, bool error, bool hold, uint8_t pct)
{
    if (text == NULL) {
        text = "";
    }
    bool kind_changed = kind != s_card_kind;
    s_card_kind = kind;

    // Normal: black on white with black edges; error: white on black with white edges
    ui_set_bg_color(s_card, LV_PART_MAIN, error ? OLED_BLACK : OLED_WHITE);
    lv_color_t edge = lv_color_hex(error ? OLED_WHITE : OLED_BLACK);
    if (!lv_color_eq(lv_obj_get_style_border_color(s_card, LV_PART_MAIN), edge)) {
        lv_obj_set_style_border_color(s_card, edge, LV_PART_MAIN);
    }
    ui_set_text_color(s_card_text, error ? OLED_WHITE : OLED_BLACK);
    if (ui_label_set(s_card_text, text) || kind_changed) {
        ESP_LOGD(TAG, "overlay %d: %s", (int)kind, text);
    }

    int32_t text_h = card_lines(text) * OLED_LINE;
    int32_t card_h = text_h + CARD_PAD + (hold ? CARD_HOLD_EXTRA : 0);
    set_height(s_card_text, text_h);
    set_height(s_card, card_h);
    set_y(s_card, (OLED_H - card_h) / 2);
    ui_set_hidden(s_card_bar, !hold);
    if (hold) {
        set_y(s_card_bar, CARD_TEXT_Y + text_h + 1);
        lv_bar_set_value(s_card_bar, pct > 100 ? 100 : pct, LV_ANIM_OFF);  // no-op when unchanged
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

static void show_page(ui_page_t page, unsigned sub)
{
    if (page != s_page || sub != s_sub) {
        ESP_LOGD(TAG, "page %d.%u", (int)page, sub);
    }
    s_page = page;
    s_sub = sub;
    lv_screen_load(s_screen[page]);     // no animation; no-op if already active
}

static void page_update(const ui_view_t *v)
{
    switch (s_page) {
    case UI_PAGE_MAIN:
        oled_main_update(v);
        break;
    case UI_PAGE_CELLS:
        oled_cells_update(v, s_sub);
        break;
    case UI_PAGE_ALARMS:
        oled_alarms_update(v);
        break;
    default:
        oled_details_update(v);
        break;
    }
}

void ui_build(void)
{
    if (s_screen[UI_PAGE_MAIN] != NULL) {
        ESP_LOGW(TAG, "ui_build called twice");
        return;
    }
    s_screen[UI_PAGE_MAIN] = oled_main_create();
    s_screen[UI_PAGE_CELLS] = oled_cells_create();
    s_screen[UI_PAGE_ALARMS] = oled_alarms_create();
    s_screen[UI_PAGE_DETAILS] = oled_details_create();

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
    ui_view_init(&v, &m);
    oled_main_update(&v);
    oled_cells_update(&v, 0);
    oled_alarms_update(&v);
    oled_details_update(&v);
    show_page(UI_PAGE_MAIN, 0);
    strip_update(&m, oled_cells_pages(&v), ui_alarms_active(&m));
    ESP_LOGI(TAG, "OLED UI built (BMS data: %s)", m.bms.has_data ? "yes" : "not yet");
}

void ui_update(const ui_model_t *m)
{
    if (m == NULL || s_screen[UI_PAGE_MAIN] == NULL) {
        return;
    }
    ui_view_t v;
    ui_view_init(&v, m);
    bool alarms = ui_alarms_active(m);
    unsigned cell_pages = oled_cells_pages(&v);
    if (s_page == UI_PAGE_ALARMS && !alarms) {
        show_page(UI_PAGE_MAIN, 0);     // the Alarms page left the ring
    } else if (s_page == UI_PAGE_CELLS && s_sub >= cell_pages) {
        show_page(UI_PAGE_CELLS, cell_pages - 1);   // fewer cells than before
    }
    strip_update(m, cell_pages, alarms);
    page_update(&v);
}

void ui_next_page(const ui_model_t *m)
{
    if (m == NULL || s_screen[UI_PAGE_MAIN] == NULL) {
        return;
    }
    ui_view_t v;
    ui_view_init(&v, m);
    switch (s_page) {
    case UI_PAGE_MAIN:
        show_page(UI_PAGE_CELLS, 0);
        break;
    case UI_PAGE_CELLS:
        if (s_sub + 1 < oled_cells_pages(&v)) {
            show_page(UI_PAGE_CELLS, s_sub + 1);
        } else {
            show_page(ui_alarms_active(m) ? UI_PAGE_ALARMS : UI_PAGE_DETAILS, 0);
        }
        break;
    case UI_PAGE_ALARMS:
        show_page(UI_PAGE_DETAILS, 0);
        break;
    default:
        show_page(UI_PAGE_MAIN, 0);
        break;
    }
    ui_update(m);
}

void ui_show_main(const ui_model_t *m)
{
    if (s_screen[UI_PAGE_MAIN] == NULL) {
        return;
    }
    show_page(UI_PAGE_MAIN, 0);
    ui_update(m);
}

ui_page_t ui_current_page(void)
{
    return s_page;
}

// ---- Wording: 8 characters per line, so no word is longer than 8 ----

static const char *const k_texts[UI_TXT_COUNT] = {
    [UI_TXT_NO_DISPLAY]        = "No display",
    [UI_TXT_NO_DATA]           = "No data",
    [UI_TXT_BMS_OFFLINE]       = "BMS offline",
    [UI_TXT_NO_FRESH]          = "No fresh data",
    [UI_TXT_BUSY]              = "Busy",
    [UI_TXT_HOLD_ENABLE]       = "Hold to enable output",
    [UI_TXT_HOLD_DISABLE]      = "Hold to disable output",
    [UI_TXT_HOLD_DISABLE_TRIP] = "Tripped. Hold to disable output",
    [UI_TXT_RELEASE_ON]        = "Release to turn ON",
    [UI_TXT_RELEASE_OFF]       = "Release to turn OFF",
    [UI_TXT_SWITCHING]         = "Sending",
    [UI_TXT_CANCELLED]         = "Canceled",
    [UI_TXT_OUTPUT_ON]         = "Output ON",
    [UI_TXT_OUTPUT_OFF]        = "Output OFF",
    [UI_TXT_BLOCKED]           = "ON but blocked by BMS",
    [UI_TXT_NO_REPLY]          = "No reply from BMS",
    [UI_TXT_STATE_UNKNOWN]     = "Sent, state unknown",
    [UI_TXT_NOT_CONF_ON]       = "Failed, output is ON",
    [UI_TXT_NOT_CONF_TRIP]     = "Failed, output enabled",
    [UI_TXT_NOT_CONF_OFF]      = "Failed, output is OFF",
    [UI_TXT_VBAT_LOW]          = "Board battery low",
};

const char *ui_text(ui_text_t id)
{
    return ((unsigned)id < UI_TXT_COUNT && k_texts[id] != NULL) ? k_texts[id] : "";
}

void ui_text_sleeping(char *buf, size_t cap, int seconds)
{
    snprintf(buf, cap, "Sleep in %ds", seconds);
}
