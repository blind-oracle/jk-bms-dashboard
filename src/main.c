// Boot flow and the application loop. The loop owns page navigation, the
// output-toggle flow, the inactivity/sleep state and the task watchdog; the
// BMS task and the button module only post events to it.
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"

#include "app_config.h"
#include "app_events.h"
#include "battery.h"
#include "button.h"
#include "display.h"
#include "jk_bms.h"
#include "jk_proto.h"
#include "power.h"
#include "ui.h"

static const char *TAG = "app";

// The loop never blocks longer than this between TWDT resets
#define LOOP_WAIT_MS        100
#define HOLD_WAIT_MS        40      // smoother progress bar while the hold card fills
#define UI_LOCK_MS          500
#define UI_PERIOD_US        (250 * 1000LL)
#define VBAT_PERIOD_US      ((int64_t)APP_VBAT_PERIOD_MS * 1000)
// The crash counter of the boot-loop guard is cleared when the board reaches
// deep sleep (power.c). Uptime alone counts as healthy only when sleep is
// disabled, or after the board has been kept awake well past the timeout.
#define HEALTHY_AFTER_US    ((APP_SLEEP_TIMEOUT_S > 0 ? (int64_t)APP_SLEEP_TIMEOUT_S + 60 : 60) * 1000000LL)

// A snapshot older than this is not trusted for an output toggle
#define BMS_FRESH_US        ((int64_t)APP_BMS_OFFLINE_POLLS * APP_POLL_INTERVAL_MS * 1000)

#define SLEEP_TIMEOUT_US    ((int64_t)APP_SLEEP_TIMEOUT_S * 1000000)
#define SLEEP_WARN_S        (APP_SLEEP_WARN_S < APP_SLEEP_TIMEOUT_S / 2 ? APP_SLEEP_WARN_S : APP_SLEEP_TIMEOUT_S / 2)
#define SLEEP_WARN_AT_US    (SLEEP_TIMEOUT_US - (int64_t)SLEEP_WARN_S * 1000000)

// Recovery from events lost to a full queue: neither a hold nor a command
// may keep the board awake forever
#define HOLD_STALE_US       (500 * 1000LL)      // hold active, button no longer held
#define CMD_STALE_US        (2000 * 1000LL)     // command finished, no APP_EV_DSG_RESULT
#define CMD_SLEEP_GRACE_US  (60 * 1000000LL)    // past the timeout: sleep despite a pending command

#define MSG_REASON_MS       1500
#define MSG_CANCEL_MS       1000
#define MSG_OK_MS           2000
#define MSG_ERROR_MS        4000
#define MSG_VBAT_MS         3000

typedef struct {
    bool        display_ok;         // display_init() succeeded: ui_* and backlight calls allowed
    bool        bms_ok;
    bool        battery_ok;
    int64_t     last_activity_us;

    bool        warning_active;     // dimmed, "Sleeping in N s" on the card
    int         warning_shown_s;    // N on the card, 0 = not drawn yet

    bool        hold_active;        // hold card up: releasing in the arm window toggles
    bool        hold_target;        // discharge setting frozen at HOLD_START
    const char *hold_label;
    int         hold_pct;           // progress on the card, -1 = not drawn
    int64_t     hold_stale_us;      // first time the button was seen released, 0 = held
    bool        swallowing;         // ignore button events until the next BTN_DOWN

    bool        cmd_pending;        // jk_bms_set_discharge() accepted, no result yet
    int64_t     cmd_idle_us;        // first time jk_bms reported no command, 0 = busy

    int         vbat_mv;            // -1 = unknown
    bool        vbat_low;
    bool        vbat_msg_pending;   // "Board battery low" waits for a free card
    bool        vbat_msg_done;      // shown once per wake

    bool        ui_due;
    int64_t     next_ui_us;
    int64_t     next_vbat_us;
    bool        healthy_marked;
} app_state_t;

static app_state_t s_app = {
    .vbat_mv = -1,
    .hold_pct = -1,
};

// Static: a snapshot is a few hundred bytes. Both are used by the app task only.
static jk_snapshot_t s_snap;    // for decisions
static ui_model_t s_model;      // for the UI: filled only under the LVGL lock

// ---- Helpers ----

static void bms_snapshot(jk_snapshot_t *out)
{
    if (s_app.bms_ok) {
        jk_bms_get_snapshot(out);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

static bool bms_command_pending(void)
{
    return s_app.bms_ok && jk_bms_command_pending();
}

// Call with the LVGL lock held, in case the UI keeps the pointer
static const ui_model_t *model_refresh(void)
{
    bms_snapshot(&s_model.bms);
    s_model.vbat_mv = s_app.vbat_mv;
    s_model.vbat_low = s_app.vbat_low;
    s_model.now_us = esp_timer_get_time();
    return &s_model;
}

static bool ui_lock(void)
{
    if (!s_app.display_ok) {
        return false;
    }
    if (!display_lock(UI_LOCK_MS)) {
        ESP_LOGW(TAG, "LVGL lock timeout");
        return false;
    }
    return true;
}

// display_init() runs this under the LVGL lock before the first frame, so
// that frame already shows whatever the BMS task has received.
static void build_ui(void)
{
    ui_build();
    ui_update(model_refresh());
}

// Feedback for the latest press: replaces whatever card is visible
static void show_feedback(const char *text, uint32_t duration_ms)
{
    if (ui_lock()) {
        ui_overlay_hide(UI_OVL_HOLD);
        ui_overlay_message(UI_OVL_INFO, text, false, duration_ms);
        display_unlock();
    }
}

// NULL if the output may be toggled now, otherwise the reason for the user
static const char *toggle_block_reason(const jk_snapshot_t *snap, int64_t now)
{
    if (!s_app.display_ok) {
        return "No display";    // never switch the output without the hold/arm cards
    }
    if (!snap->has_data) {
        return "No data";
    }
    if (snap->offline) {
        return "BMS offline";
    }
    if (now - snap->last_ok_us >= BMS_FRESH_US) {
        return "No fresh data";
    }
    if (bms_command_pending() || s_app.cmd_pending) {
        return "Busy";
    }
    return NULL;
}

static int hold_progress_pct(void)
{
    const int32_t span = APP_LONG_PRESS_MS - APP_HOLD_START_MS;     // >= 500, see app_config.h
    const int32_t t = (int32_t)button_held_ms() - APP_HOLD_START_MS;
    if (t <= 0) {
        return 0;
    }
    if (t >= span) {
        return 100;
    }
    return (int)(t * 100 / span);
}

static void hold_draw(int pct)
{
    if (ui_lock()) {
        ui_overlay_hold(s_app.hold_label, (uint8_t)pct);
        display_unlock();
        s_app.hold_pct = pct;
    }
}

// ---- Board battery ----

static void battery_update(void)
{
    const int mv = battery_read_mv();
    s_app.vbat_mv = mv;
    if (mv < 0) {
        return;     // unknown: keep the low flag as it is
    }
    if (!s_app.vbat_low && mv < APP_VBAT_WARN_MV) {
        s_app.vbat_low = true;
        ESP_LOGW(TAG, "Board battery low: %d mV", mv);
        if (!s_app.vbat_msg_done) {
            s_app.vbat_msg_pending = true;
            s_app.vbat_msg_done = true;
        }
    } else if (s_app.vbat_low && mv >= APP_VBAT_WARN_MV + APP_VBAT_HYSTERESIS_MV) {
        s_app.vbat_low = false;
        s_app.vbat_msg_pending = false;
        ESP_LOGI(TAG, "Board battery recovered: %d mV", mv);
    }
}

// ---- UI refresh ----

static void ui_refresh(void)
{
    if (!ui_lock()) {
        return;
    }
    const ui_model_t *m = model_refresh();
    // The Alarms page is in the ring only while alarms are active
    if (ui_current_page() == UI_PAGE_ALARMS && !(m->bms.has_data && m->bms.data.alarms != 0)) {
        ui_show_main(m);
    }
    ui_update(m);
    if (s_app.vbat_msg_pending && ui_overlay_current() == UI_OVL_NONE) {
        ui_overlay_message(UI_OVL_INFO, "Board battery low", true, MSG_VBAT_MS);
        s_app.vbat_msg_pending = false;
    }
    display_unlock();
}

// ---- Sleep warning ----

static void warning_start(void)
{
    s_app.warning_active = true;
    s_app.warning_shown_s = 0;
    ESP_LOGI(TAG, "Inactive: sleeping in %d s", SLEEP_WARN_S);
    if (s_app.display_ok) {
        display_set_backlight(APP_LCD_DIM_PCT);
    }
}

static void warning_cancel(void)
{
    s_app.warning_active = false;
    s_app.warning_shown_s = 0;
    if (!s_app.display_ok) {
        return;
    }
    display_set_backlight(APP_LCD_BRIGHTNESS_PCT);
    if (ui_lock()) {
        ui_overlay_hide(UI_OVL_INFO);
        display_unlock();
    }
}

static void warning_update(int64_t idle_us)
{
    static char text[32];   // static: valid even if the card keeps the pointer
    int left_s = (int)((SLEEP_TIMEOUT_US - idle_us + 999999) / 1000000);
    if (left_s < 1) {
        left_s = 1;         // sleep is being held off by the button or a command
    }
    if (left_s == s_app.warning_shown_s) {
        return;
    }
    if (ui_lock()) {
        snprintf(text, sizeof(text), "Sleeping in %d s", left_s);
        ui_overlay_message(UI_OVL_INFO, text, false, 0);
        display_unlock();
        s_app.warning_shown_s = left_s;
    }
}

// ---- Button gestures ----

static void on_btn_down(void)
{
    s_app.swallowing = false;
    if (!s_app.warning_active) {
        return;
    }
    // A press during the sleep warning only cancels the sleep
    ESP_LOGI(TAG, "Sleep cancelled");
    warning_cancel();
    button_swallow_current_press();
    s_app.swallowing = true;    // also drops events of this press already queued
}

static void on_btn_short(void)
{
    if (ui_lock()) {
        const ui_model_t *m = model_refresh();
        ui_next_page(m);
        ui_update(m);
        display_unlock();
    }
}

static void on_hold_start(void)
{
    bms_snapshot(&s_snap);
    const char *reason = toggle_block_reason(&s_snap, esp_timer_get_time());
    if (reason != NULL) {
        s_app.hold_active = false;
        ESP_LOGI(TAG, "Output toggle unavailable: %s", reason);
        show_feedback(reason, MSG_REASON_MS);
        return;
    }
    s_app.hold_active = true;
    s_app.hold_target = !jk_dsg_on(&s_snap.data);
    s_app.hold_stale_us = 0;
    if (s_app.hold_target) {
        s_app.hold_label = "Hold: enable output";
    } else if (jk_dsg_state(&s_snap.data) == JK_DSG_TRIP) {
        s_app.hold_label = "Hold: disable output (tripped)";
    } else {
        s_app.hold_label = "Hold: disable output";
    }
    s_app.hold_pct = -1;
    hold_draw(0);
}

static void on_armed(void)
{
    if (!s_app.hold_active) {
        return;
    }
    s_app.hold_label = s_app.hold_target ? "Release to switch output ON" : "Release to switch output OFF";
    hold_draw(100);
}

static void on_long_release(void)
{
    if (!s_app.hold_active) {
        return;
    }
    s_app.hold_active = false;
    if (s_app.hold_pct < 100) {
        // The "Release to switch output ..." card was never drawn (LVGL busy,
        // or BTN_ARMED lost): do not act on a prompt the user did not see
        ESP_LOGW(TAG, "Arm card not shown, toggle refused");
        show_feedback("Cancelled", MSG_CANCEL_MS);
        return;
    }

    bms_snapshot(&s_snap);
    const char *reason = toggle_block_reason(&s_snap, esp_timer_get_time());
    if (reason != NULL) {
        ESP_LOGI(TAG, "Output toggle cancelled: %s", reason);
        show_feedback(reason, MSG_REASON_MS);
        return;
    }
    if (!s_app.bms_ok || !jk_bms_set_discharge(s_app.hold_target)) {
        ESP_LOGW(TAG, "Discharge command refused");
        show_feedback("Busy", MSG_REASON_MS);
        return;
    }
    s_app.cmd_pending = true;
    s_app.cmd_idle_us = 0;
    ESP_LOGI(TAG, "Switching the discharge output %s", s_app.hold_target ? "ON" : "OFF");
    show_feedback("Switching output...", 0);
}

static void on_cancel(void)
{
    if (!s_app.hold_active) {
        return;
    }
    s_app.hold_active = false;
    ESP_LOGI(TAG, "Output toggle cancelled");
    show_feedback("Cancelled", MSG_CANCEL_MS);
}

static void on_button(app_event_type_t type)
{
    s_app.last_activity_us = esp_timer_get_time();
    if (type == APP_EV_BTN_DOWN) {
        on_btn_down();
        return;
    }
    if (s_app.swallowing) {
        return;     // the rest of a press that cancelled the sleep warning
    }
    switch (type) {
    case APP_EV_BTN_SHORT:
        on_btn_short();
        break;
    case APP_EV_BTN_HOLD_START:
        on_hold_start();
        break;
    case APP_EV_BTN_ARMED:
        on_armed();
        break;
    case APP_EV_BTN_LONG_RELEASE:
        on_long_release();
        break;
    case APP_EV_BTN_CANCEL:
        on_cancel();
        break;
    default:
        break;
    }
}

// ---- BMS events ----

static void on_dsg_result(const app_event_t *ev)
{
    const char *text;
    bool error = true;
    uint32_t duration_ms = MSG_ERROR_MS;

    s_app.cmd_pending = false;
    switch (ev->dsg_result) {
    case APP_DSG_RESULT_OK:
        if (!ev->dsg_target) {
            text = "Output OFF";
        } else if (ev->dsg_state == JK_DSG_TRIP) {
            text = "Enabled - blocked by BMS";
        } else {
            text = "Output ON";
        }
        error = false;
        duration_ms = MSG_OK_MS;
        break;
    case APP_DSG_RESULT_NO_REPLY:
        text = "No reply from BMS";
        break;
    case APP_DSG_RESULT_UNKNOWN:
        text = "Sent - state unknown";
        break;
    case APP_DSG_RESULT_NOT_CONFIRMED:
    default:
        // The state here was read back after the write
        if (ev->dsg_state == JK_DSG_ON) {
            text = "Not confirmed - output is ON";
        } else if (ev->dsg_state == JK_DSG_TRIP) {
            text = "Not confirmed - output still enabled";
        } else {
            text = "Not confirmed - output is OFF";
        }
        break;
    }
    ESP_LOGI(TAG, "Discharge %s: %s", ev->dsg_target ? "ON" : "OFF", text);
    if (ui_lock()) {
        ui_overlay_message(UI_OVL_RESULT, text, error, duration_ms);
        display_unlock();
    }
    s_app.ui_due = true;
}

static void handle_event(const app_event_t *ev)
{
    switch (ev->type) {
    case APP_EV_BTN_DOWN:
    case APP_EV_BTN_SHORT:
    case APP_EV_BTN_HOLD_START:
    case APP_EV_BTN_CANCEL:
    case APP_EV_BTN_ARMED:
    case APP_EV_BTN_LONG_RELEASE:
        on_button(ev->type);
        break;
    case APP_EV_BTN_STUCK:
        ESP_LOGW(TAG, "Wake press held for %d s: button stuck, sleeping", APP_BTN_STUCK_MS / 1000);
        power_enter_deep_sleep();
        break;
    case APP_EV_BMS_DATA:
        s_app.ui_due = true;
        break;
    case APP_EV_DSG_RESULT:
        on_dsg_result(ev);
        break;
    default:
        ESP_LOGD(TAG, "Unknown event %d", (int)ev->type);
        break;
    }
}

// ---- Periodic work ----

static void check_stale_state(int64_t now)
{
    if (s_app.hold_active) {
        const button_state_t bs = button_get_state();
        if (bs == BTN_STATE_HOLD || bs == BTN_STATE_ARMED) {
            s_app.hold_stale_us = 0;
        } else if (s_app.hold_stale_us == 0) {
            s_app.hold_stale_us = now;
        } else if (now - s_app.hold_stale_us >= HOLD_STALE_US) {
            ESP_LOGW(TAG, "Hold ended without a release event");
            s_app.hold_active = false;
            show_feedback("Cancelled", MSG_CANCEL_MS);
        }
    }
    if (s_app.cmd_pending) {
        if (bms_command_pending()) {
            s_app.cmd_idle_us = 0;
        } else if (s_app.cmd_idle_us == 0) {
            s_app.cmd_idle_us = now;
        } else if (now - s_app.cmd_idle_us >= CMD_STALE_US) {
            ESP_LOGW(TAG, "Discharge command ended without a result event");
            s_app.cmd_pending = false;
            s_app.ui_due = true;
            if (ui_lock()) {
                ui_overlay_hide(UI_OVL_INFO);
                display_unlock();
            }
        }
    }
}

static void sleep_check(void)
{
    if (APP_SLEEP_TIMEOUT_S <= 0) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (APP_NO_SLEEP_WITH_USB_HOST && usb_serial_jtag_is_connected()) {
        s_app.last_activity_us = now;   // no warning and no sleep while a host is attached
        if (s_app.warning_active) {
            warning_cancel();
        }
        return;
    }

    const int64_t idle = now - s_app.last_activity_us;
    if (s_app.warning_active && idle < SLEEP_WARN_AT_US) {
        warning_cancel();               // a press restarted the timer
    }
    if (SLEEP_WARN_S > 0 && idle >= SLEEP_WARN_AT_US && !s_app.warning_active && !s_app.hold_active) {
        warning_start();
    }
    if (s_app.warning_active) {
        warning_update(idle);
    }

    if (idle < SLEEP_TIMEOUT_US || s_app.hold_active) {
        return;
    }
    const button_state_t bs = button_get_state();
    if (bs != BTN_STATE_IDLE && bs != BTN_STATE_WAIT_RELEASE) {
        return;
    }
    if (s_app.cmd_pending || bms_command_pending()) {
        if (idle < SLEEP_TIMEOUT_US + CMD_SLEEP_GRACE_US) {
            return;                     // let the command finish
        }
        ESP_LOGW(TAG, "Discharge command still pending, sleeping anyway");
    }
    ESP_LOGI(TAG, "No activity for %d s: deep sleep", (int)(idle / 1000000));
    power_enter_deep_sleep();
}

static void periodic(void)
{
    const int64_t now = esp_timer_get_time();

    check_stale_state(now);

    // Only forward: a release racing the state read must not pull the bar back
    if (s_app.hold_active && button_get_state() == BTN_STATE_HOLD) {
        const int pct = hold_progress_pct();
        if (pct > s_app.hold_pct) {
            hold_draw(pct);
        }
    }

    if (s_app.battery_ok && now >= s_app.next_vbat_us) {
        s_app.next_vbat_us = now + VBAT_PERIOD_US;
        battery_update();
    }

    if (s_app.ui_due || now >= s_app.next_ui_us) {
        s_app.ui_due = false;
        s_app.next_ui_us = now + UI_PERIOD_US;
        ui_refresh();
    }

    if (!s_app.healthy_marked && now >= HEALTHY_AFTER_US) {
        s_app.healthy_marked = true;
        power_mark_healthy();
    }

    sleep_check();      // may not return
}

// ---- Boot ----

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "other";
    }
}

static void log_boot(const power_boot_info_t *boot)
{
    const char *wake = boot->ext1_wake ? "button"
                       : boot->deep_sleep_wake ? "deep sleep, not the button" : "none";
    ESP_LOGI(TAG, "Reset: %s, wake: %s (causes 0x%08" PRIx32 ")",
             reset_reason_name(esp_reset_reason()), wake, esp_sleep_get_wakeup_causes());
    if (APP_SLEEP_TIMEOUT_S > 0) {
        ESP_LOGI(TAG, "Sleep after %d s idle, warning %d s before; no sleep with USB host: %d",
                 APP_SLEEP_TIMEOUT_S, SLEEP_WARN_S, APP_NO_SLEEP_WITH_USB_HOST);
    } else {
        ESP_LOGI(TAG, "Sleep on inactivity disabled");
    }
    ESP_LOGI(TAG, "BMS: poll %d ms, offline after %d failed polls, frame source 0x%02X, demo mode %d",
             APP_POLL_INTERVAL_MS, APP_BMS_OFFLINE_POLLS, (unsigned)APP_JK_FRAME_SOURCE, APP_DEMO_MODE);
    ESP_LOGI(TAG, "Button: hold %d ms, arm %d ms, window %d ms; LCD fast wake %d, LCD power GPIO %d",
             APP_HOLD_START_MS, APP_LONG_PRESS_MS, APP_ARM_WINDOW_MS, APP_LCD_FAST_WAKE, APP_PIN_LCD_PWR);
}

void app_main(void)
{
    power_boot_info_t boot = {0};
    power_early_boot(&boot);    // stuck button or boot loop: back to sleep, no return
    power_release_holds();
    log_boot(&boot);

    // BMS and button first, so the first frame usually has data already
    app_events_init();
    esp_err_t err = jk_bms_start();
    s_app.bms_ok = (err == ESP_OK);
    if (!s_app.bms_ok) {
        ESP_LOGE(TAG, "BMS task not started: %s", esp_err_to_name(err));
    }
    err = button_start(boot.ext1_wake);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Button not started: %s", esp_err_to_name(err));
    }

    // Fast wake relies on the panel keeping its state in sleep-in: not with the power switch
    const bool fast_wake = boot.ext1_wake && APP_LCD_FAST_WAKE && !APP_LCD_PWR_USED;
    err = display_init(fast_wake, build_ui);
    s_app.display_ok = (err == ESP_OK);
    if (!s_app.display_ok) {
        // Carry on without a UI: the board must still go to sleep on timeout
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(err));
    }

    err = battery_init();
    s_app.battery_ok = (err == ESP_OK);
    if (s_app.battery_ok) {
        battery_update();
    } else {
        ESP_LOGE(TAG, "Battery ADC init failed: %s", esp_err_to_name(err));
    }

    err = esp_task_wdt_add(NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TWDT subscribe failed: %s", esp_err_to_name(err));
    }

    const int64_t start = esp_timer_get_time();
    s_app.last_activity_us = start;
    s_app.next_ui_us = start;
    s_app.next_vbat_us = start + VBAT_PERIOD_US;
    ESP_LOGI(TAG, "Ready at %d ms (%s display init, board battery %d mV)",
             (int)(start / 1000), fast_wake ? "fast" : "full", s_app.vbat_mv);

    for (;;) {
        app_event_t ev = {};
        const bool hold_filling = s_app.hold_active && s_app.hold_pct < 100;
        const bool got = app_wait_event(&ev, hold_filling ? HOLD_WAIT_MS : LOOP_WAIT_MS);
        esp_task_wdt_reset();
        if (got) {
            handle_event(&ev);
        }
        periodic();
    }
}
