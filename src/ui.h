// Dashboard UI (LVGL 9.6): the 240x240 colour TFT UI (ui.c, ui_main.c, ...) or
// the 64x128 monochrome OLED UI (ui_oled.c, ui_oled_pages.c), one per build.
// Every function here must be called with the LVGL lock held (display_lock),
// except ui_build, which display_init calls under it, and the ui_text helpers.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "jk_bms.h"

typedef enum {
    UI_PAGE_MAIN = 0,
    UI_PAGE_CELLS,
    UI_PAGE_ALARMS,     // in the ring only while alarms are active
    UI_PAGE_DETAILS,
    UI_PAGE_COUNT,
} ui_page_t;

typedef struct {
    jk_snapshot_t bms;
    int     vbat_mv;    // board battery, -1 = unknown
    bool    vbat_low;   // below the warning threshold (with hysteresis)
    int64_t now_us;     // esp_timer_get_time()
} ui_model_t;

// Create all screens, the status strip and the overlay card; load Main.
void ui_build(void);
// Refresh the status strip and the visible page from the model.
void ui_update(const ui_model_t *m);
// Go to the next page in the ring Main -> Cells -> [Alarms] -> Details.
void ui_next_page(const ui_model_t *m);
// Show Main (after wake, or when the current page left the ring).
void ui_show_main(const ui_model_t *m);
ui_page_t ui_current_page(void);

// ---- Overlay card: one slot on lv_layer_top(), priority HOLD > RESULT > INFO ----
typedef enum {
    UI_OVL_NONE = 0,
    UI_OVL_INFO,
    UI_OVL_RESULT,
    UI_OVL_HOLD,
} ui_overlay_kind_t;

// Hold card with a progress bar (0-100 %). Replaces any toast.
void ui_overlay_hold(const char *text, uint8_t progress_pct);
// Toast. Ignored if a higher-priority card is visible. duration_ms 0 = until hidden.
void ui_overlay_message(ui_overlay_kind_t kind, const char *text, bool error, uint32_t duration_ms);
// Hide the card if its kind is <= max_kind.
void ui_overlay_hide(ui_overlay_kind_t max_kind);
ui_overlay_kind_t ui_overlay_current(void);

// ---- Wording: each UI picks texts that fit its screen (no lock needed) ----
typedef enum {
    UI_TXT_NO_DISPLAY = 0,      // toggle refused: display not working
    UI_TXT_NO_DATA,             // toggle refused: no frame yet
    UI_TXT_BMS_OFFLINE,         // toggle refused: link down
    UI_TXT_NO_FRESH,            // toggle refused: data too old
    UI_TXT_BUSY,                // toggle refused: command running
    UI_TXT_HOLD_ENABLE,         // hold card, target ON
    UI_TXT_HOLD_DISABLE,        // hold card, target OFF
    UI_TXT_HOLD_DISABLE_TRIP,   // hold card, target OFF while tripped
    UI_TXT_RELEASE_ON,          // armed, target ON
    UI_TXT_RELEASE_OFF,         // armed, target OFF
    UI_TXT_SWITCHING,           // command sent, waiting for the result
    UI_TXT_CANCELLED,
    UI_TXT_OUTPUT_ON,
    UI_TXT_OUTPUT_OFF,
    UI_TXT_BLOCKED,             // enabled, but a protection keeps the MOSFET off
    UI_TXT_NO_REPLY,
    UI_TXT_STATE_UNKNOWN,       // acked, nothing read back
    UI_TXT_NOT_CONF_ON,         // not confirmed, output is ON
    UI_TXT_NOT_CONF_TRIP,       // not confirmed, setting still on (tripped)
    UI_TXT_NOT_CONF_OFF,        // not confirmed, output is OFF
    UI_TXT_VBAT_LOW,            // board battery low
    UI_TXT_COUNT,
} ui_text_t;

const char *ui_text(ui_text_t id);
// The sleep-warning countdown, e.g. "Sleeping in 9 s"
void ui_text_sleeping(char *buf, size_t cap, int seconds);
