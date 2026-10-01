// Alarms page (in the ring only while alarms are active): protections in red
// first, warnings (low capacity) in amber last, long names, up to 14 rows.
#include "esp_log.h"
#include "ui_internal.h"

static const char *TAG = "ui_alarms";

#define ROWS        14
#define ROW_X       4
#define ROW_Y0      16
#define ROW_PITCH   16
#define ROW_W       232

static lv_obj_t *s_row[ROWS];
static uint16_t s_shown;        // bitmask on screen, for the log

lv_obj_t *ui_alarms_create(void)
{
    lv_obj_t *scr = ui_screen_create();
    for (int r = 0; r < ROWS; r++) {
        s_row[r] = ui_label_create(scr, UI_FONT_14, ROW_X, ROW_Y0 + ROW_PITCH * r, ROW_W, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_hidden(s_row[r], true);
    }
    return scr;
}

void ui_alarms_update(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    uint16_t alarms = (v->has_data && (d->present & JK_HAS_ALARMS)) ? d->alarms : 0;
    if (alarms != s_shown) {
        ESP_LOGD(TAG, "alarms 0x%04x", (unsigned)alarms);
        s_shown = alarms;
    }

    unsigned total = ui_alarm_count(alarms);
    // With more alarms than rows, the last row says how many did not fit
    unsigned slots = total > ROWS ? ROWS - 1 : ROWS;

    unsigned r = 0;
    for (int pass = 0; pass < 2; pass++) {          // protections, then warnings
        for (unsigned bit = 0; bit < UI_ALARM_BITS && r < slots; bit++) {
            if (!(alarms & (1u << bit)) || ui_alarm_is_warning(bit) != (pass == 1)) {
                continue;
            }
            char buf[16];
            ui_label_set(s_row[r], ui_alarm_name(bit, false, buf, sizeof(buf)));
            ui_set_text_color(s_row[r], ui_live_color(v, pass == 1 ? UI_COLOR_AMBER : UI_COLOR_RED));
            ui_set_hidden(s_row[r], false);
            r++;
        }
    }
    if (total > r && r < ROWS) {
        ui_label_setf(s_row[r], "... +%u more", total - r);
        ui_set_text_color(s_row[r], ui_live_color(v, UI_COLOR_RED));
        ui_set_hidden(s_row[r], false);
        r++;
    }
    for (; r < ROWS; r++) {
        ui_set_hidden(s_row[r], true);
    }
}
