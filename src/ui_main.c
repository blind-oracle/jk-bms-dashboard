// Main page: SOC with switch flags, SOC bar, pack voltage and power, current,
// cell spread, temperatures and the status/alarm banner.
#include <stdio.h>
#include "esp_log.h"
#include "ui_internal.h"

static const char *TAG = "ui_main";

// Layout (y ranges from the plan). The left column (SOC, voltage, current) is
// right-aligned to x=134, the right column (flags, power) to x=236.
#define SOC_X       4
#define SOC_Y       18
#define SOC_W       130
#define FLAG_X      186
#define FLAG_W      50
#define FLAG_Y      24          // CHG, DSG, BAL stacked at a 16 px pitch (y 24-71)
#define FLAG_PITCH  16
#define BAR_X       4
#define BAR_Y       73
#define BAR_W       232
#define BAR_H       8
#define VALUE_X     4
#define VALUE_W     130
#define VOLT_Y      84
#define CURR_Y      116
#define POWER_X     134         // up to "-999.9 kW" (101 px)
#define POWER_W     102
#define POWER_Y     91          // shares the baseline of the 28 px voltage (y 109)
#define LINE_X      4
#define LINE_W      232
#define CELLS_Y     150
#define TEMPS_Y     174
#define BANNER_X    4
#define BANNER_Y    198
#define BANNER_W    232
#define BANNER_H    20
#define BANNER_PAD  4

#define SOC_RED_BELOW   20
#define SOC_AMBER_BELOW 40

static struct {
    lv_obj_t *soc;
    lv_obj_t *chg;
    lv_obj_t *dsg;
    lv_obj_t *bal;
    lv_obj_t *bar;
    lv_obj_t *volt;
    lv_obj_t *power;
    lv_obj_t *current;
    lv_obj_t *cells;
    lv_obj_t *temps;
    lv_obj_t *banner;
    lv_obj_t *banner_text;
} s;

lv_obj_t *ui_main_create(void)
{
    lv_obj_t *scr = ui_screen_create();

    s.soc = ui_label_create(scr, UI_FONT_48, SOC_X, SOC_Y, SOC_W, LV_TEXT_ALIGN_RIGHT);
    s.chg = ui_label_create(scr, UI_FONT_14, FLAG_X, FLAG_Y, FLAG_W, LV_TEXT_ALIGN_RIGHT);
    s.dsg = ui_label_create(scr, UI_FONT_14, FLAG_X, FLAG_Y + FLAG_PITCH, FLAG_W, LV_TEXT_ALIGN_RIGHT);
    s.bal = ui_label_create(scr, UI_FONT_14, FLAG_X, FLAG_Y + 2 * FLAG_PITCH, FLAG_W, LV_TEXT_ALIGN_RIGHT);
    ui_label_set(s.chg, "CHG");
    ui_label_set(s.dsg, "DSG");
    ui_label_set(s.bal, "BAL");

    s.bar = ui_bar_create(scr, BAR_X, BAR_Y, BAR_W, BAR_H);

    s.volt = ui_label_create(scr, UI_FONT_28, VALUE_X, VOLT_Y, VALUE_W, LV_TEXT_ALIGN_RIGHT);
    s.power = ui_label_create(scr, UI_FONT_20, POWER_X, POWER_Y, POWER_W, LV_TEXT_ALIGN_RIGHT);
    s.current = ui_label_create(scr, UI_FONT_28, VALUE_X, CURR_Y, VALUE_W, LV_TEXT_ALIGN_RIGHT);
    s.cells = ui_label_create(scr, UI_FONT_20, LINE_X, CELLS_Y, LINE_W, LV_TEXT_ALIGN_LEFT);
    s.temps = ui_label_create(scr, UI_FONT_20, LINE_X, TEMPS_Y, LINE_W, LV_TEXT_ALIGN_LEFT);

    s.banner = ui_box_create(scr, BANNER_X, BANNER_Y, BANNER_W, BANNER_H);
    lv_obj_set_style_radius(s.banner, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s.banner, lv_color_hex(UI_COLOR_DIM), LV_PART_MAIN);
    s.banner_text = ui_label_create(s.banner, UI_FONT_14, BANNER_PAD,
                                    (BANNER_H - lv_font_get_line_height(UI_FONT_14)) / 2,
                                    BANNER_W - 2 * BANNER_PAD, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_hidden(s.banner, true);
    return scr;
}

static void soc_update(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    bool ok = v->has_data && (d->present & JK_HAS_SOC);
    if (ok) {
        ui_label_setf(s.soc, "%u%%", (unsigned)d->soc_pct);
    } else {
        ui_label_set(s.soc, UI_NA "%");
    }
    ui_set_text_color(s.soc, ok ? ui_live_color(v, UI_COLOR_TEXT) : UI_COLOR_DIM);

    int32_t soc = ok ? (d->soc_pct > 100 ? 100 : d->soc_pct) : 0;
    lv_bar_set_value(s.bar, soc, LV_ANIM_OFF);  // no-op when unchanged
    uint32_t color = UI_COLOR_GREEN;
    if (soc < SOC_RED_BELOW) {
        color = UI_COLOR_RED;
    } else if (soc < SOC_AMBER_BELOW) {
        color = UI_COLOR_AMBER;
    }
    ui_set_bg_color(s.bar, LV_PART_INDICATOR, ui_live_color(v, color));
}

static void flags_update(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    bool st = v->live && (d->present & JK_HAS_STATUS);
    ui_set_text_color(s.chg, st && (d->status & JK_STATUS_CHG_MOS) ? UI_COLOR_GREEN : UI_COLOR_DIM);
    ui_set_text_color(s.bal, st && (d->status & JK_STATUS_BALANCING) ? UI_COLOR_BLUE : UI_COLOR_DIM);

    uint32_t dsg = UI_COLOR_DIM;
    if (v->live) {
        jk_dsg_state_t state = jk_dsg_state(d);
        if (state == JK_DSG_ON) {
            dsg = UI_COLOR_GREEN;
        } else if (state == JK_DSG_TRIP) {
            dsg = UI_COLOR_AMBER;
        }
    }
    ui_set_text_color(s.dsg, dsg);
}

static void electrical_update(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    char buf[32];
    ui_str_t str;

    if (v->has_data && (d->present & JK_HAS_VOLTAGE)) {
        ui_label_setf(s.volt, "%u.%02u V", (unsigned)(d->voltage_10mv / 100), (unsigned)(d->voltage_10mv % 100));
        ui_set_text_color(s.volt, ui_live_color(v, UI_COLOR_TEXT));
    } else {
        ui_label_set(s.volt, UI_NA "." UI_NA " V");
        ui_set_text_color(s.volt, UI_COLOR_DIM);
    }

    if (!v->has_data || !d->current_valid) {
        ui_label_set(s.current, UI_NA "." UI_NA " A");
        ui_set_text_color(s.current, UI_COLOR_DIM);
        ui_label_set(s.power, UI_NA " W");
        ui_set_text_color(s.power, UI_COLOR_DIM);
        return;
    }

    ui_str_init(&str, buf, sizeof(buf));
    ui_str_add_current(&str, d->current_10ma);
    ui_label_set(s.current, buf);
    uint32_t color = UI_COLOR_TEXT;
    if (d->current_10ma > UI_CURRENT_IDLE_10MA) {
        color = UI_COLOR_GREEN;         // charging
    } else if (d->current_10ma < -UI_CURRENT_IDLE_10MA) {
        color = UI_COLOR_AMBER;         // discharging
    }
    ui_set_text_color(s.current, ui_live_color(v, color));

    ui_str_init(&str, buf, sizeof(buf));
    ui_str_add_power(&str, d->power_dw);
    ui_label_set(s.power, buf);
    ui_set_text_color(s.power, ui_live_color(v, UI_COLOR_TEXT));
}

static void cells_temps_update(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    char buf[64];
    ui_str_t str;

    // "3.281-3.312 (31 mV)"
    ui_str_init(&str, buf, sizeof(buf));
    if (v->has_data && ui_str_add_cell_range(&str, d)) {
        ui_str_add(&str, " (%u mV)", (unsigned)d->cell_delta_mv);
        ui_label_set(s.cells, buf);
        ui_set_text_color(s.cells, ui_live_color(v, UI_COLOR_TEXT));
    } else {
        ui_label_set(s.cells, UI_NA);
        ui_set_text_color(s.cells, UI_COLOR_DIM);
    }

    // "MOS 25° / 24° / 23°", only the sensors the BMS reports
    ui_str_init(&str, buf, sizeof(buf));
    if (v->has_data) {
        for (unsigned i = 0; i < UI_TEMP_SENSORS; i++) {
            int t;
            if (!ui_temp_get(d, i, &t)) {
                continue;
            }
            ui_str_add(&str, "%s%s%d" UI_DEG, str.len > 0 ? " / " : "", i == 0 ? "MOS " : "", t);
        }
    }
    if (str.len > 0) {
        ui_label_set(s.temps, buf);
        ui_set_text_color(s.temps, ui_live_color(v, UI_COLOR_TEXT));
    } else {
        ui_label_set(s.temps, UI_NA);
        ui_set_text_color(s.temps, UI_COLOR_DIM);
    }
}

// Offline (red) > connecting (grey) > top alarm (red, amber if warnings only) > hidden
static void banner_update(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    char buf[64];
    char name_buf[16];
    const char *text = NULL;
    const char *what = NULL;            // for the log
    uint32_t bg = UI_COLOR_RED;
    uint32_t fg = UI_COLOR_TEXT;
    unsigned more = 0;

    if (v->m->bms.offline) {
        text = what = "BMS offline";
    } else if (!v->has_data) {
        text = what = "Connecting...";
        bg = UI_COLOR_DIM;
        fg = UI_COLOR_BLACK;
    } else if ((d->present & JK_HAS_ALARMS) && d->alarms != 0) {
        int top = ui_alarm_top(d->alarms);  // >= 0 here
        what = ui_alarm_name((unsigned)top, true, name_buf, sizeof(name_buf));
        unsigned count = ui_alarm_count(d->alarms);
        more = count > 1 ? count - 1 : 0;
        if (more > 0) {
            snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " %s +%u", what, more);
        } else {
            snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " %s", what);
        }
        text = buf;
        if (ui_alarm_is_warning((unsigned)top)) {
            bg = UI_COLOR_AMBER;        // the top alarm is a warning: no protection active
            fg = UI_COLOR_BLACK;
        }
    }

    if (text == NULL) {
        if (ui_label_set(s.banner_text, "")) {
            ESP_LOGI(TAG, "banner cleared");
        }
        ui_set_hidden(s.banner, true);
        return;
    }
    ui_set_bg_color(s.banner, LV_PART_MAIN, bg);
    ui_set_text_color(s.banner_text, fg);
    if (ui_label_set(s.banner_text, text)) {
        ESP_LOGI(TAG, "banner: %s (+%u)", what, more);
    }
    ui_set_hidden(s.banner, false);
}

void ui_main_update(const ui_view_t *v)
{
    soc_update(v);
    flags_update(v);
    electrical_update(v);
    cells_temps_update(v);
    banner_update(v);
}
