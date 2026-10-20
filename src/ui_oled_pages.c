// OLED pages (64x128, unscii_8 lines of 8 characters): Main, Cells (sub-pages
// of up to 12 cells), Alarms and Details. Stale data (BMS offline) keeps its
// last values: the strip shows "x" and Main shows OFFLINE.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "ui_oled_internal.h"

static const char *TAG = "ui_oled";

// |v|, also for INT32_MIN
static uint32_t abs_u32(int32_t v)
{
    return v < 0 ? (uint32_t)0 - (uint32_t)v : (uint32_t)v;
}

// ===================== Main =====================

// Rows, top to bottom. A unscii_8 line is 9 px: a blank row, 7 rows of
// capitals, a descender row. Groups are 1 px further apart.
#define SOC_Y           9       // unscii_16, "87%" centred
#define BAR_Y           26
#define BAR_H           6       // 1 px outline around a 4 px fill (LVGL's minimum)
#define VOLT_Y          33
#define CURR_Y          42
#define POWER_Y         51
#define HI_Y            61
#define LO_Y            70
#define DIFF_Y          79
#define MOS_Y           89
#define TEMP_Y          98
#define FLAG_Y          108     // CH DS BL: plain off, inverted on, outlined tripped
#define FLAG_W          18
#define FLAG_PITCH      23
#define BANNER_Y        118     // to the bottom edge; 10 px keeps descenders off the frame
#define BANNER_H        10

#define MAIN_FLAGS      3       // CH DS BL

static struct {
    lv_obj_t *soc;
    lv_obj_t *bar;
    lv_obj_t *volt;
    lv_obj_t *current;
    lv_obj_t *power;
    lv_obj_t *hi;
    lv_obj_t *lo;
    lv_obj_t *diff;
    lv_obj_t *mos;
    lv_obj_t *temps;
    lv_obj_t *flag[MAIN_FLAGS];
    lv_obj_t *banner;
    lv_obj_t *banner_text;
} s_main;

lv_obj_t *oled_main_create(void)
{
    lv_obj_t *scr = ui_screen_create();

    s_main.soc = ui_label_create(scr, OLED_FONT_BIG, 0, SOC_Y, OLED_W, LV_TEXT_ALIGN_CENTER);

    s_main.bar = lv_bar_create(scr);
    lv_obj_remove_style_all(s_main.bar);
    lv_obj_set_pos(s_main.bar, 0, BAR_Y);
    lv_obj_set_size(s_main.bar, OLED_W, BAR_H);
    lv_bar_set_range(s_main.bar, 0, 100);
    lv_bar_set_value(s_main.bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_border_width(s_main.bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_main.bar, lv_color_hex(OLED_WHITE), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_main.bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_main.bar, lv_color_hex(OLED_WHITE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_main.bar, LV_OPA_COVER, LV_PART_INDICATOR);

    s_main.volt = oled_text_create(scr, 0, VOLT_Y, OLED_W, LV_TEXT_ALIGN_RIGHT);
    s_main.current = oled_text_create(scr, 0, CURR_Y, OLED_W, LV_TEXT_ALIGN_RIGHT);
    s_main.power = oled_text_create(scr, 0, POWER_Y, OLED_W, LV_TEXT_ALIGN_RIGHT);
    s_main.hi = oled_text_create(scr, 0, HI_Y, OLED_W, LV_TEXT_ALIGN_LEFT);
    s_main.lo = oled_text_create(scr, 0, LO_Y, OLED_W, LV_TEXT_ALIGN_LEFT);
    s_main.diff = oled_text_create(scr, 0, DIFF_Y, OLED_W, LV_TEXT_ALIGN_LEFT);
    s_main.mos = oled_text_create(scr, 0, MOS_Y, OLED_W, LV_TEXT_ALIGN_LEFT);
    s_main.temps = oled_text_create(scr, 0, TEMP_Y, OLED_W, LV_TEXT_ALIGN_LEFT);

    static const char *const names[MAIN_FLAGS] = {"CH", "DS", "BL"};
    for (int i = 0; i < MAIN_FLAGS; i++) {
        s_main.flag[i] = oled_tag_create(scr, FLAG_PITCH * i, FLAG_Y, FLAG_W, OLED_LINE);
        ui_label_set(oled_tag_label(s_main.flag[i]), names[i]);
    }

    s_main.banner = oled_tag_create(scr, 0, BANNER_Y, OLED_W, BANNER_H);
    s_main.banner_text = oled_tag_label(s_main.banner);
    lv_obj_set_hidden(s_main.banner, true);
    return scr;
}

static void soc_update(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    bool ok = v->has_data && (d->present & JK_HAS_SOC);
    if (ok) {
        ui_label_setf(s_main.soc, "%u%%", (unsigned)d->soc_pct);
    } else {
        ui_label_set(s_main.soc, UI_NA "%");
    }
    int32_t soc = ok ? (d->soc_pct > 100 ? 100 : d->soc_pct) : 0;
    lv_bar_set_value(s_main.bar, soc, LV_ANIM_OFF);     // no-op when unchanged
}

static void electrical_update(const ui_view_t *v)
{
    const jk_data_t *d = v->d;

    if (v->has_data && (d->present & JK_HAS_VOLTAGE)) {
        ui_label_setf(s_main.volt, "%u.%02uV", (unsigned)(d->voltage_10mv / 100), (unsigned)(d->voltage_10mv % 100));
    } else {
        ui_label_set(s_main.volt, UI_NA "." UI_NA "V");
    }

    if (!v->has_data || !d->current_valid) {
        ui_label_set(s_main.current, UI_NA "." UI_NA "A");
        ui_label_set(s_main.power, UI_NA "W");
        return;
    }

    // "-12.34A" below 100 A, "-175.0A" from 100 A
    uint32_t a = abs_u32(d->current_10ma);
    const char *sign = d->current_10ma < 0 ? "-" : "";
    if (a < 10000) {
        ui_label_setf(s_main.current, "%s%" PRIu32 ".%02" PRIu32 "A", sign, a / 100, a % 100);
    } else {
        uint32_t t = (a + 5) / 10;      // 0.1 A, rounded
        if (t > 9999) {
            t = 9999;
        }
        ui_label_setf(s_main.current, "%s%" PRIu32 ".%" PRIu32 "A", sign, t / 10, t % 10);
    }

    // "-652W", "-17.4kW" from 10 kW
    uint32_t p = abs_u32(d->power_dw);
    uint32_t w = (p + 5) / 10;          // 1 W, rounded
    sign = (d->power_dw < 0 && w > 0) ? "-" : "";
    if (w < 10000) {
        ui_label_setf(s_main.power, "%s%" PRIu32 "W", sign, w);
    } else {
        uint32_t t = (p + 500) / 1000;  // 0.1 kW, rounded
        if (t > 9999) {
            t = 9999;
        }
        ui_label_setf(s_main.power, "%s%" PRIu32 ".%" PRIu32 "kW", sign, t / 10, t % 10);
    }
}

static void cells_temps_update(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    char value[16];
    char line[24];
    ui_str_t str;

    bool cells = v->has_data && (d->present & JK_HAS_CELLS) && d->cells_valid > 0;
    if (cells) {
        ui_str_init(&str, value, sizeof(value));
        ui_str_add_mv(&str, d->cell_max_mv);
        oled_kv(line, sizeof(line), "Hi", value);
        ui_label_set(s_main.hi, line);
        ui_str_init(&str, value, sizeof(value));
        ui_str_add_mv(&str, d->cell_min_mv);
        oled_kv(line, sizeof(line), "Lo", value);
        ui_label_set(s_main.lo, line);
        oled_kv_u32(line, sizeof(line), "Df", d->cell_delta_mv, "mV");
        ui_label_set(s_main.diff, line);
    } else {
        oled_kv(line, sizeof(line), "Hi", UI_NA);
        ui_label_set(s_main.hi, line);
        oled_kv(line, sizeof(line), "Lo", UI_NA);
        ui_label_set(s_main.lo, line);
        oled_kv(line, sizeof(line), "Df", UI_NA);
        ui_label_set(s_main.diff, line);
    }

    int t;
    if (v->has_data && ui_temp_get(d, 0, &t)) {
        snprintf(value, sizeof(value), "%dC", t);
        oled_kv(line, sizeof(line), "MOS", value);
    } else {
        oled_kv(line, sizeof(line), "MOS", UI_NA);
    }
    ui_label_set(s_main.mos, line);
    oled_temps_line(line, sizeof(line), v);
    ui_label_set(s_main.temps, line);
}

// Off (or not live), on, tripped
static void flags_update(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    bool st = v->live && (d->present & JK_HAS_STATUS);
    oled_tag_set(s_main.flag[0], st && (d->status & JK_STATUS_CHG_MOS) ? OLED_TAG_INVERTED : OLED_TAG_PLAIN);
    oled_tag_set(s_main.flag[2], st && (d->status & JK_STATUS_BALANCING) ? OLED_TAG_INVERTED : OLED_TAG_PLAIN);

    oled_tag_style_t dsg = OLED_TAG_PLAIN;
    if (v->live) {
        jk_dsg_state_t state = jk_dsg_state(d);
        if (state == JK_DSG_ON) {
            dsg = OLED_TAG_INVERTED;
        } else if (state == JK_DSG_TRIP) {
            dsg = OLED_TAG_OUTLINED;
        }
    }
    oled_tag_set(s_main.flag[1], dsg);
}

// Offline (inverted) > waiting for the first frame (outlined) > top alarm
// (inverted; outlined if warnings only) > hidden
static void banner_update(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    char name_buf[12];
    const char *text = NULL;
    bool inverted = true;

    if (v->m->bms.offline) {
        text = "OFFLINE";
    } else if (!v->has_data) {
        text = "Waiting";
        inverted = false;
    } else if ((d->present & JK_HAS_ALARMS) && d->alarms != 0) {
        int top = ui_alarm_top(d->alarms);  // >= 0 here
        text = oled_alarm_name((unsigned)top, name_buf, sizeof(name_buf));
        inverted = !ui_alarm_is_warning((unsigned)top);
    }

    if (text == NULL) {
        if (ui_label_set(s_main.banner_text, "")) {
            ESP_LOGI(TAG, "banner cleared");
        }
        ui_set_hidden(s_main.banner, true);
        return;
    }
    oled_tag_set(s_main.banner, inverted ? OLED_TAG_INVERTED : OLED_TAG_OUTLINED);
    if (ui_label_set(s_main.banner_text, text)) {
        ESP_LOGI(TAG, "banner: %s", text);
    }
    ui_set_hidden(s_main.banner, false);
}

void oled_main_update(const ui_view_t *v)
{
    soc_update(v);
    electrical_update(v);
    cells_temps_update(v);
    flags_update(v);
    banner_update(v);
}

// ===================== Cells =====================

#define CELLS_HDR_Y     10      // "Df  31mV"; while balancing "Df31" + inverted "BAL"
#define CELLS_BAL_X     40
#define CELLS_BAL_W     24
#define CELLS_ROW_Y0    20      // "01 3.281"; "^" max and "v" min replace the space
#define CELLS_EMPTY_Y   60
#define MARK_MIN_DELTA_MV 5     // mark min/max only from this spread

static struct {
    lv_obj_t *header;
    lv_obj_t *bal;
    lv_obj_t *row[OLED_CELL_ROWS];
    lv_obj_t *empty;
} s_cells;

static unsigned cell_count(const ui_view_t *v)
{
    if (!v->has_data || !(v->d->present & JK_HAS_CELLS)) {
        return 0;
    }
    return v->d->cell_count > JK_MAX_CELLS ? JK_MAX_CELLS : v->d->cell_count;
}

unsigned oled_cells_pages(const ui_view_t *v)
{
    unsigned n = cell_count(v);
    return n == 0 ? 1 : (n + OLED_CELL_ROWS - 1) / OLED_CELL_ROWS;
}

lv_obj_t *oled_cells_create(void)
{
    lv_obj_t *scr = ui_screen_create();
    s_cells.header = oled_text_create(scr, 0, CELLS_HDR_Y, OLED_W, LV_TEXT_ALIGN_LEFT);
    s_cells.bal = oled_text_create(scr, CELLS_BAL_X, CELLS_HDR_Y, CELLS_BAL_W, LV_TEXT_ALIGN_CENTER);
    ui_label_set(s_cells.bal, "BAL");
    oled_set_inverted(s_cells.bal, true);
    lv_obj_set_hidden(s_cells.bal, true);
    for (int r = 0; r < OLED_CELL_ROWS; r++) {
        s_cells.row[r] = oled_text_create(scr, 0, CELLS_ROW_Y0 + OLED_LINE * r, OLED_W, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_hidden(s_cells.row[r], true);
    }
    s_cells.empty = oled_text_create(scr, 0, CELLS_EMPTY_Y, OLED_W, LV_TEXT_ALIGN_CENTER);
    ui_label_set(s_cells.empty, "No cells");
    return scr;
}

void oled_cells_update(const ui_view_t *v, unsigned sub_page)
{
    const jk_data_t *d = v->d;
    unsigned n = cell_count(v);
    unsigned pages = oled_cells_pages(v);
    if (sub_page >= pages) {
        sub_page = pages - 1;
    }
    // Balanced: 16 cells are 8 + 8, not 12 + 4
    unsigned per = (n + pages - 1) / pages;
    unsigned first = sub_page * per;
    unsigned count = first < n ? n - first : 0;
    if (count > per) {
        count = per;
    }

    bool range = n > 0 && d->cells_valid > 0;
    bool bal = v->live && (d->present & JK_HAS_STATUS) && (d->status & JK_STATUS_BALANCING);
    char value[16];
    char line[24];
    if (!range) {
        oled_kv(line, sizeof(line), "Df", UI_NA);
    } else if (bal) {
        // Left of the BAL tag: 5 characters
        if (d->cell_delta_mv < 1000) {
            snprintf(line, sizeof(line), "Df%u", (unsigned)d->cell_delta_mv);
        } else {
            snprintf(line, sizeof(line), "%u", (unsigned)d->cell_delta_mv);
        }
    } else {
        snprintf(value, sizeof(value), "%umV", (unsigned)d->cell_delta_mv);
        oled_kv(line, sizeof(line), "Df", value);
    }
    ui_label_set(s_cells.header, line);
    ui_set_hidden(s_cells.bal, !(bal && range));

    bool mark = v->live && range && d->cell_delta_mv >= MARK_MIN_DELTA_MV;
    for (unsigned r = 0; r < OLED_CELL_ROWS; r++) {
        bool visible = r < count;
        ui_set_hidden(s_cells.row[r], !visible);
        if (!visible) {
            continue;
        }
        unsigned i = first + r;
        unsigned mv = d->cell_mv[i];
        char marker = ' ';
        if (mark && mv != 0 && i == d->cell_max_idx) {
            marker = '^';
        } else if (mark && mv != 0 && i == d->cell_min_idx) {
            marker = 'v';
        }
        if (mv == 0) {
            ui_label_setf(s_cells.row[r], "%02u " UI_NA, i + 1);
        } else {
            ui_label_setf(s_cells.row[r], "%02u%c%u.%03u", i + 1, marker, mv / 1000, mv % 1000);
        }
    }
    ui_set_hidden(s_cells.empty, n > 0);
}

// ===================== Alarms =====================

#define ALARM_HDR_Y     10      // "Alarms 3", inverted
#define ALARM_ROW_Y0    20
#define ALARM_ROWS      12

static lv_obj_t *s_alarm_hdr;
static lv_obj_t *s_alarm_row[ALARM_ROWS];
static uint16_t s_alarms_shown;     // bitmask on screen, for the log

lv_obj_t *oled_alarms_create(void)
{
    lv_obj_t *scr = ui_screen_create();
    s_alarm_hdr = oled_text_create(scr, 0, ALARM_HDR_Y, OLED_W, LV_TEXT_ALIGN_LEFT);
    oled_set_inverted(s_alarm_hdr, true);
    for (int r = 0; r < ALARM_ROWS; r++) {
        s_alarm_row[r] = oled_text_create(scr, 0, ALARM_ROW_Y0 + OLED_LINE * r, OLED_W, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_hidden(s_alarm_row[r], true);
    }
    return scr;
}

void oled_alarms_update(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    uint16_t alarms = (v->has_data && (d->present & JK_HAS_ALARMS)) ? d->alarms : 0;
    if (alarms != s_alarms_shown) {
        ESP_LOGD(TAG, "alarms 0x%04x", (unsigned)alarms);
        s_alarms_shown = alarms;
    }

    unsigned total = ui_alarm_count(alarms);
    char value[8];
    char line[24];
    snprintf(value, sizeof(value), "%u", total);
    oled_kv(line, sizeof(line), "Alarms", value);
    ui_label_set(s_alarm_hdr, line);

    // With more alarms than rows, the last row says how many did not fit
    unsigned slots = total > ALARM_ROWS ? ALARM_ROWS - 1 : ALARM_ROWS;
    unsigned r = 0;
    for (int pass = 0; pass < 2; pass++) {          // protections, then warnings
        for (unsigned bit = 0; bit < UI_ALARM_BITS && r < slots; bit++) {
            if (!(alarms & (1u << bit)) || ui_alarm_is_warning(bit) != (pass == 1)) {
                continue;
            }
            char buf[12];
            ui_label_set(s_alarm_row[r], oled_alarm_name(bit, buf, sizeof(buf)));
            ui_set_hidden(s_alarm_row[r], false);
            r++;
        }
    }
    if (total > r && r < ALARM_ROWS) {
        ui_label_setf(s_alarm_row[r], "+%u more", total - r);
        ui_set_hidden(s_alarm_row[r], false);
        r++;
    }
    for (; r < ALARM_ROWS; r++) {
        ui_set_hidden(s_alarm_row[r], true);
    }
}

// ===================== Details =====================

#define DET_ROW_Y0      10

enum {
    DET_MOS = 0,
    DET_TEMPS,
    DET_CYCLES,
    DET_CYCLE_AH,
    DET_NOMINAL,
    DET_REMAINING,
    DET_ACTUAL,
    DET_CHG,
    DET_DSG,
    DET_BAL,
    DET_SW,
    DET_UPTIME,
    DET_ALARMS,
    DET_ROWS,
};
static_assert(DET_ROWS * OLED_LINE + DET_ROW_Y0 <= OLED_H, "details rows fit the screen");

static lv_obj_t *s_det[DET_ROWS];

lv_obj_t *oled_details_create(void)
{
    lv_obj_t *scr = ui_screen_create();
    for (int r = 0; r < DET_ROWS; r++) {
        s_det[r] = oled_text_create(scr, 0, DET_ROW_Y0 + OLED_LINE * r, OLED_W, LV_TEXT_ALIGN_LEFT);
    }
    return scr;
}

// key + value, or key + "--" when !known
static void det_set(int row, const char *key, bool known, const char *value)
{
    char line[24];
    oled_kv(line, sizeof(line), key, known ? value : UI_NA);
    ui_label_set(s_det[row], line);
}

static void det_u32(int row, const char *key, bool known, uint32_t value, const char *unit)
{
    char line[24];
    if (known) {
        oled_kv_u32(line, sizeof(line), key, value, unit);
    } else {
        oled_kv(line, sizeof(line), key, UI_NA);
    }
    ui_label_set(s_det[row], line);
}

// CHG/DSG: MOSFET on; off with the setting on = a protection tripped it; off
static const char *mos_state(bool mos_on, bool has_setting, bool setting_on)
{
    if (mos_on) {
        return "ON";
    }
    return has_setting && setting_on ? "TRIP" : "OFF";
}

// "11.XW_S11.26" -> "11.26": the part after the last '_', without a leading
// 'S'/'V', the last 6 characters
static void sw_tail(char *out, size_t cap, const char *version)
{
    const char *tail = version;
    for (const char *p = version; *p != '\0'; p++) {
        if (*p == '_' && p[1] != '\0') {
            tail = p + 1;
        }
    }
    if ((tail[0] == 'S' || tail[0] == 'V' || tail[0] == 'v') && tail[1] >= '0' && tail[1] <= '9') {
        tail++;
    }
    size_t len = strlen(tail);
    if (len > 6) {
        tail += len - 6;
    }
    snprintf(out, cap, "%s", tail);
}

void oled_details_update(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    const uint32_t p = v->has_data ? d->present : 0;
    const bool st = p & JK_HAS_STATUS;
    char buf[24];

    int t;
    bool mos = v->has_data && ui_temp_get(d, 0, &t);
    snprintf(buf, sizeof(buf), "%dC", mos ? t : 0);
    det_set(DET_MOS, "MOS", mos, buf);
    oled_temps_line(buf, sizeof(buf), v);
    ui_label_set(s_det[DET_TEMPS], buf);

    det_u32(DET_CYCLES, "Cyc", p & JK_HAS_CYCLES, d->cycles, "");
    det_u32(DET_CYCLE_AH, "CyAh", p & JK_HAS_CYCLE_AH, d->cycle_capacity_ah, "");
    det_u32(DET_NOMINAL, "Nom", p & JK_HAS_CAPACITY, d->capacity_ah, "Ah");
    // Remaining = nominal * SOC / 100, rounded
    bool rem_ok = (p & JK_HAS_CAPACITY) && (p & JK_HAS_SOC);
    uint32_t rem = rem_ok ? (uint32_t)(((uint64_t)d->capacity_ah * d->soc_pct + 50) / 100) : 0;
    det_u32(DET_REMAINING, "Rem", rem_ok, rem, "Ah");
    det_u32(DET_ACTUAL, "Act", p & JK_HAS_ACTUAL_AH, d->actual_capacity_ah, "Ah");

    det_set(DET_CHG, "CHG", st,
            mos_state(d->status & JK_STATUS_CHG_MOS, p & JK_HAS_CHG_SWITCH, d->charge_switch));
    det_set(DET_DSG, "DSG", st,
            mos_state(d->status & JK_STATUS_DSG_MOS, p & JK_HAS_DSG_SWITCH, d->discharge_switch));
    // Balancing now, enabled but idle, or disabled
    const char *bal = UI_NA;
    if ((p & JK_HAS_BAL_SWITCH) && !d->balancer_switch) {
        bal = "OFF";
    } else if (st) {
        bal = (d->status & JK_STATUS_BALANCING) ? "ON" : "IDLE";
    }
    det_set(DET_BAL, "BAL", true, bal);

    char text[sizeof(d->sw_version)];
    char tail[8] = "";
    size_t n = (p & JK_HAS_SW_VERSION) ? ui_copy_printable(text, sizeof(text), d->sw_version, sizeof(d->sw_version)) : 0;
    if (n > 0) {
        sw_tail(tail, sizeof(tail), text);
    }
    det_set(DET_SW, "SW", n > 0, tail);

    if (p & JK_HAS_UPTIME) {
        uint32_t min = d->uptime_min;
        uint32_t days = min / 1440;
        unsigned hours = (unsigned)(min % 1440 / 60);
        if (days >= 100) {
            snprintf(buf, sizeof(buf), "Up %" PRIu32 "d", days);
        } else if (days > 0) {
            snprintf(buf, sizeof(buf), "Up%2" PRIu32 "d%02uh", days, hours);
        } else {
            snprintf(buf, sizeof(buf), "Up%2uh%02um", hours, (unsigned)(min % 60));
        }
        ui_label_set(s_det[DET_UPTIME], buf);
    } else {
        det_set(DET_UPTIME, "Up", false, NULL);
    }

    // Inverted while a protection is active
    bool alarms_known = p & JK_HAS_ALARMS;
    det_u32(DET_ALARMS, "Alarms", alarms_known, alarms_known ? ui_alarm_count(d->alarms) : 0, "");
    bool protection = alarms_known && d->alarms != 0 && !ui_alarm_is_warning((unsigned)ui_alarm_top(d->alarms));
    oled_set_inverted(s_det[DET_ALARMS], protection);
}
