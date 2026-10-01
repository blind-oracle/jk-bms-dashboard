// Button: 10 ms esp_timer poll, debouncing and the gesture state machine
// documented in button.h. The callback never blocks: it only samples the pin
// and posts APP_EV_BTN_* events to the app queue.
//
// Timed transitions (HOLD, ARMED, arm-window cancel, stuck) only fire while
// the latest sample still reads pressed, so a release is judged by the moment
// the pin went high, not by when the debounce accepted it.
#include "button.h"

#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "app_config.h"
#include "app_events.h"

static const char *TAG = "button";

#define POLL_PERIOD_US  (10 * 1000)
#define MS_TO_US(ms)    ((int64_t)(ms) * 1000)

// Shared with the app task, guarded by s_mux. Only the callback changes the
// state; the app task only raises s_swallow_req.
static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static button_state_t s_state = BTN_STATE_IDLE;
static int64_t        s_press_us;       // start of the current press (state != IDLE)
static bool           s_swallow_req;

// Owned by the callback (set by button_start before the timer runs)
static bool    s_raw;                   // last sample, true = pressed
static int64_t s_raw_since_us;          // when the sampled level last changed
static int64_t s_armed_us;              // when ARMED was entered
static bool    s_wake_press;            // the current press woke (or was held at) boot
static bool    s_stuck_posted;

static esp_timer_handle_t s_timer;

static void post(app_event_type_t type)
{
    if (!app_post_simple(type)) {
        ESP_LOGW(TAG, "event queue full, button event %d dropped", (int)type);
    }
}

static bool is_gesture_state(button_state_t st)
{
    return st == BTN_STATE_PRESSED || st == BTN_STATE_HOLD || st == BTN_STATE_ARMED;
}

static void poll_cb(void *arg)
{
    (void)arg;
    const int64_t now = esp_timer_get_time();
    const bool raw = gpio_get_level(APP_PIN_BUTTON) == 0;

    if (raw != s_raw) {
        s_raw = raw;
        s_raw_since_us = now;
    }
    const int64_t stable_us = now - s_raw_since_us;
    const bool debounced = stable_us >= MS_TO_US(APP_BTN_DEBOUNCE_MS);

    portENTER_CRITICAL(&s_mux);
    button_state_t st = s_state;
    int64_t press_us = s_press_us;
    const bool swallow = s_swallow_req;
    s_swallow_req = false;
    portEXIT_CRITICAL(&s_mux);

    // Applied before the sample is evaluated, so a pending release of the
    // swallowed press produces no event
    if (swallow && is_gesture_state(st)) {
        st = BTN_STATE_WAIT_RELEASE;
        s_wake_press = false;
    }

    switch (st) {
    case BTN_STATE_IDLE:
        if (raw && debounced) {
            st = BTN_STATE_PRESSED;
            press_us = s_raw_since_us;
            post(APP_EV_BTN_DOWN);
        }
        break;

    case BTN_STATE_PRESSED:
        if (!raw) {
            if (debounced) {
                st = BTN_STATE_IDLE;
                post(APP_EV_BTN_SHORT);
            }
        } else if (now - press_us >= MS_TO_US(APP_HOLD_START_MS)) {
            st = BTN_STATE_HOLD;
            post(APP_EV_BTN_HOLD_START);
        }
        break;

    case BTN_STATE_HOLD:
        if (!raw) {
            if (debounced) {
                st = BTN_STATE_IDLE;
                post(APP_EV_BTN_CANCEL);
            }
        } else if (now - press_us >= MS_TO_US(APP_LONG_PRESS_MS)) {
            st = BTN_STATE_ARMED;
            s_armed_us = now;
            post(APP_EV_BTN_ARMED);
        }
        break;

    case BTN_STATE_ARMED:
        if (!raw) {
            if (debounced) {
                st = BTN_STATE_IDLE;
                post(APP_EV_BTN_LONG_RELEASE);
            }
        } else if (now - s_armed_us >= MS_TO_US(APP_ARM_WINDOW_MS)) {
            // Held past the window: a stuck button never toggles the output
            st = BTN_STATE_WAIT_RELEASE;
            post(APP_EV_BTN_CANCEL);
        }
        break;

    case BTN_STATE_WAIT_RELEASE:
        if (!raw) {
            if (stable_us >= MS_TO_US(APP_BTN_RELEASE_STABLE_MS)) {
                st = BTN_STATE_IDLE;
                s_wake_press = false;
            }
        } else if (s_wake_press && !s_stuck_posted && now - press_us >= MS_TO_US(APP_BTN_STUCK_MS)) {
            // Retried on the next tick if the queue is full
            s_stuck_posted = app_post_simple(APP_EV_BTN_STUCK);
        }
        break;
    }

    portENTER_CRITICAL(&s_mux);
    s_state = st;
    s_press_us = press_us;
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t button_start(bool wake_press)
{
    ESP_RETURN_ON_FALSE(s_timer == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");

    // A pad hold left over from deep sleep would freeze the configuration below
    gpio_hold_dis(APP_PIN_BUTTON);
    // gpio_config also routes the pad back from the LP (RTC) IO MUX used by EXT1
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(APP_PIN_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "GPIO%d config failed", APP_PIN_BUTTON);
    esp_rom_delay_us(100);  // let the pull-up settle before the first sample

    const int64_t now = esp_timer_get_time();
    const bool pressed = button_read_raw();
    s_raw = pressed;
    s_raw_since_us = now;
    s_armed_us = 0;
    s_wake_press = wake_press || pressed;
    s_stuck_posted = false;

    portENTER_CRITICAL(&s_mux);
    s_state = s_wake_press ? BTN_STATE_WAIT_RELEASE : BTN_STATE_IDLE;
    s_press_us = now;
    s_swallow_req = false;
    portEXIT_CRITICAL(&s_mux);

    const esp_timer_create_args_t args = {
        .callback = poll_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "button",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &timer), TAG, "timer create failed");
    const esp_err_t err = esp_timer_start_periodic(timer, POLL_PERIOD_US);
    if (err != ESP_OK) {
        esp_timer_delete(timer);
        ESP_LOGE(TAG, "timer start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_timer = timer;

    ESP_LOGI(TAG, "GPIO%d ready%s", APP_PIN_BUTTON,
             s_wake_press ? ", ignoring the current press until it is released" : "");
    return ESP_OK;
}

button_state_t button_get_state(void)
{
    portENTER_CRITICAL(&s_mux);
    const button_state_t st = s_state;
    portEXIT_CRITICAL(&s_mux);
    return st;
}

uint32_t button_held_ms(void)
{
    portENTER_CRITICAL(&s_mux);
    const button_state_t st = s_state;
    const int64_t press_us = s_press_us;
    portEXIT_CRITICAL(&s_mux);

    if (st == BTN_STATE_IDLE) {
        return 0;
    }
    const int64_t held_us = esp_timer_get_time() - press_us;
    return held_us > 0 ? (uint32_t)(held_us / 1000) : 0;
}

void button_swallow_current_press(void)
{
    portENTER_CRITICAL(&s_mux);
    s_swallow_req = true;
    portEXIT_CRITICAL(&s_mux);
}

void button_stop(void)
{
    if (s_timer == NULL) {
        return;
    }
    // Also waits for a callback that is running right now
    esp_err_t err = esp_timer_stop_blocking(s_timer, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "timer stop: %s", esp_err_to_name(err));
    }
    err = esp_timer_delete(s_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "timer delete: %s", esp_err_to_name(err));
    }
    s_timer = NULL;
}

bool button_read_raw(void)
{
    return gpio_get_level(APP_PIN_BUTTON) == 0;
}
