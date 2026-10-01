// Dashboard UI (LVGL 9.6). Every function here must be called with the LVGL
// lock held (display_lock), except ui_build which display_init calls under it.
#pragma once

#include <stdbool.h>
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
