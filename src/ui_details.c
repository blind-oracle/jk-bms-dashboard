// Details page: 12 rows of secondary data (14 px). Missing fields show "--".
#include <inttypes.h>
#include "esp_log.h"
#include "ui_internal.h"

static const char *TAG = "ui_details";

#define ROWS        12
#define ROW_X       4
#define ROW_Y0      19
#define ROW_PITCH   17
#define ROW_W       232

enum {
    ROW_TEMPS = 0,
    ROW_CYCLES,
    ROW_CYCLE_CAP,
    ROW_NOMINAL,
    ROW_CAPACITY,
    ROW_CHG,
    ROW_DSG,
    ROW_BALANCER,
    ROW_SW,
    ROW_MANUFACTURER,
    ROW_UPTIME,
    ROW_ALARMS,
};
static_assert(ROW_ALARMS + 1 == ROWS, "one label per row");

static lv_obj_t *s_row[ROWS];

lv_obj_t *ui_details_create(void)
{
    lv_obj_t *scr = ui_screen_create();
    for (int r = 0; r < ROWS; r++) {
        s_row[r] = ui_label_create(scr, UI_FONT_14, ROW_X, ROW_Y0 + ROW_PITCH * r, ROW_W, LV_TEXT_ALIGN_LEFT);
    }
    return scr;
}

static const char *on_off(bool on)
{
    return on ? "ON" : "OFF";
}

// known: the row has at least one value (else it is drawn dim)
static bool row_set(const ui_view_t *v, int row, const char *text, bool known, uint32_t color)
{
    ui_set_text_color(s_row[row], known ? ui_live_color(v, color) : UI_COLOR_DIM);
    return ui_label_set(s_row[row], text);
}

static void temps_row(const ui_view_t *v)
{
    static const char *const names[UI_TEMP_SENSORS] = {"MOS", "T1", "T2"};
    char buf[64];
    ui_str_t str;
    ui_str_init(&str, buf, sizeof(buf));
    bool known = false;
    for (unsigned i = 0; i < UI_TEMP_SENSORS; i++) {
        int t;
        const char *sep = i > 0 ? "  " : "";
        if (v->has_data && ui_temp_get(v->d, i, &t)) {
            ui_str_add(&str, "%s%s %d" UI_DEG "C", sep, names[i], t);
            known = true;
        } else {
            ui_str_add(&str, "%s%s " UI_NA, sep, names[i]);
        }
    }
    row_set(v, ROW_TEMPS, buf, known, UI_COLOR_TEXT);
}

static void capacity_rows(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    const uint32_t p = v->has_data ? d->present : 0;
    char buf[64];
    ui_str_t str;

    ui_str_init(&str, buf, sizeof(buf));
    if (p & JK_HAS_CYCLES) {
        ui_str_add(&str, "Cycles %u", (unsigned)d->cycles);
    } else {
        ui_str_add(&str, "Cycles " UI_NA);
    }
    row_set(v, ROW_CYCLES, buf, p & JK_HAS_CYCLES, UI_COLOR_TEXT);

    ui_str_init(&str, buf, sizeof(buf));
    if (p & JK_HAS_CYCLE_AH) {
        ui_str_add(&str, "Cycle cap %" PRIu32 " Ah", d->cycle_capacity_ah);
    } else {
        ui_str_add(&str, "Cycle cap " UI_NA);
    }
    row_set(v, ROW_CYCLE_CAP, buf, p & JK_HAS_CYCLE_AH, UI_COLOR_TEXT);

    ui_str_init(&str, buf, sizeof(buf));
    if (p & JK_HAS_CAPACITY) {
        ui_str_add(&str, "Nominal %" PRIu32 " Ah", d->capacity_ah);
    } else {
        ui_str_add(&str, "Nominal " UI_NA);
    }
    row_set(v, ROW_NOMINAL, buf, p & JK_HAS_CAPACITY, UI_COLOR_TEXT);

    // Remaining = nominal * SOC / 100, rounded
    bool rem_ok = (p & JK_HAS_CAPACITY) && (p & JK_HAS_SOC);
    bool act_ok = p & JK_HAS_ACTUAL_AH;
    ui_str_init(&str, buf, sizeof(buf));
    if (rem_ok) {
        uint32_t rem = (uint32_t)(((uint64_t)d->capacity_ah * d->soc_pct + 50) / 100);
        ui_str_add(&str, "Rem %" PRIu32 " Ah", rem);
    } else {
        ui_str_add(&str, "Rem " UI_NA);
    }
    if (act_ok) {
        ui_str_add(&str, " / Act %" PRIu32 " Ah", d->actual_capacity_ah);
    } else {
        ui_str_add(&str, " / Act " UI_NA);
    }
    row_set(v, ROW_CAPACITY, buf, rem_ok || act_ok, UI_COLOR_TEXT);
}

static void switch_rows(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    const uint32_t p = v->has_data ? d->present : 0;
    const bool st = p & JK_HAS_STATUS;
    char buf[64];
    ui_str_t str;

    // Setting (0xAB/0xAC/0x9D) / actual state (0x8C)
    ui_str_init(&str, buf, sizeof(buf));
    ui_str_add(&str, "CHG set %s / MOS %s",
               (p & JK_HAS_CHG_SWITCH) ? on_off(d->charge_switch) : UI_NA,
               st ? on_off(d->status & JK_STATUS_CHG_MOS) : UI_NA);
    row_set(v, ROW_CHG, buf, (p & JK_HAS_CHG_SWITCH) || st, UI_COLOR_TEXT);

    bool trip = v->has_data && jk_dsg_state(d) == JK_DSG_TRIP;
    ui_str_init(&str, buf, sizeof(buf));
    ui_str_add(&str, "DSG set %s / MOS %s%s",
               (p & JK_HAS_DSG_SWITCH) ? on_off(d->discharge_switch) : UI_NA,
               st ? on_off(d->status & JK_STATUS_DSG_MOS) : UI_NA,
               trip ? " (prot.)" : "");
    row_set(v, ROW_DSG, buf, (p & JK_HAS_DSG_SWITCH) || st, trip ? UI_COLOR_AMBER : UI_COLOR_TEXT);

    ui_str_init(&str, buf, sizeof(buf));
    ui_str_add(&str, "Balancer set %s / %s",
               (p & JK_HAS_BAL_SWITCH) ? on_off(d->balancer_switch) : UI_NA,
               st ? ((d->status & JK_STATUS_BALANCING) ? "active" : "idle") : UI_NA);
    row_set(v, ROW_BALANCER, buf, (p & JK_HAS_BAL_SWITCH) || st, UI_COLOR_TEXT);
}

static void info_rows(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    const uint32_t p = v->has_data ? d->present : 0;
    char text[sizeof(d->manufacturer)];
    char buf[64];
    ui_str_t str;

    // BMS strings: printable ASCII only (the fonts have nothing else)
    size_t n = (p & JK_HAS_SW_VERSION) ? ui_copy_printable(text, sizeof(text), d->sw_version, sizeof(d->sw_version)) : 0;
    ui_str_init(&str, buf, sizeof(buf));
    ui_str_add(&str, "SW %s", n > 0 ? text : UI_NA);
    if (row_set(v, ROW_SW, buf, n > 0, UI_COLOR_TEXT) && n > 0) {
        ESP_LOGI(TAG, "BMS software %s", text);
    }

    n = (p & JK_HAS_MANUFACTURER) ? ui_copy_printable(text, sizeof(text), d->manufacturer, sizeof(d->manufacturer)) : 0;
    row_set(v, ROW_MANUFACTURER, n > 0 ? text : UI_NA, n > 0, UI_COLOR_TEXT);

    ui_str_init(&str, buf, sizeof(buf));
    if (p & JK_HAS_UPTIME) {
        uint32_t min = d->uptime_min;
        ui_str_add(&str, "Uptime %" PRIu32 "d %02uh %02um", min / 1440, (unsigned)(min % 1440 / 60),
                   (unsigned)(min % 60));
    } else {
        ui_str_add(&str, "Uptime " UI_NA);
    }
    row_set(v, ROW_UPTIME, buf, p & JK_HAS_UPTIME, UI_COLOR_TEXT);

    // Red with a protection, amber with warnings only
    uint32_t color = UI_COLOR_TEXT;
    ui_str_init(&str, buf, sizeof(buf));
    if (!(p & JK_HAS_ALARMS)) {
        ui_str_add(&str, "Alarms: " UI_NA);
    } else if (d->alarms == 0) {
        ui_str_add(&str, "No alarms");
    } else {
        ui_str_add(&str, "Alarms: %u", ui_alarm_count(d->alarms));
        color = ui_alarm_is_warning((unsigned)ui_alarm_top(d->alarms)) ? UI_COLOR_AMBER : UI_COLOR_RED;
    }
    row_set(v, ROW_ALARMS, buf, p & JK_HAS_ALARMS, color);
}

void ui_details_update(const ui_view_t *v)
{
    temps_row(v);
    capacity_rows(v);
    switch_rows(v);
    info_rows(v);
}
