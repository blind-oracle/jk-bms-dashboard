// Single momentary button (to GND, pull-up) with short press, hold, arm and
// release-to-execute gestures. Polled every 10 ms from an esp_timer; the
// callback only posts APP_EV_BTN_* events to the app queue.
//
//   IDLE --press--> PRESSED (BTN_DOWN)
//   PRESSED --release < HOLD_START--> IDLE (BTN_SHORT)
//   PRESSED --HOLD_START--> HOLD (BTN_HOLD_START)
//   HOLD --release < LONG--> IDLE (BTN_CANCEL)
//   HOLD --LONG--> ARMED (BTN_ARMED)
//   ARMED --release within ARM_WINDOW--> IDLE (BTN_LONG_RELEASE)
//   ARMED --held past ARM_WINDOW--> WAIT_RELEASE (BTN_CANCEL)
//   WAIT_RELEASE --released for 50 ms--> IDLE (no event)
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    BTN_STATE_IDLE = 0,
    BTN_STATE_PRESSED,
    BTN_STATE_HOLD,
    BTN_STATE_ARMED,
    BTN_STATE_WAIT_RELEASE,
} button_state_t;

// Configure the pin (input, pull-up; this also undoes the EXT1 RTC routing)
// and start polling. wake_press: the board was woken by this button, so the
// current press is swallowed (start in WAIT_RELEASE). If that press lasts
// APP_BTN_STUCK_MS, APP_EV_BTN_STUCK is posted once.
esp_err_t button_start(bool wake_press);

button_state_t button_get_state(void);
// Milliseconds since the current press started, 0 when released
uint32_t button_held_ms(void);
// Swallow the current press: no further events until it is released
void button_swallow_current_press(void);
// Stop the polling timer (before deep sleep)
void button_stop(void);
// Raw pin read: true if the button is pressed now (no debouncing)
bool button_read_raw(void);
