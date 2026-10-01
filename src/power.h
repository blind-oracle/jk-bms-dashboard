// Deep sleep, wake-up and pad holds.
#pragma once

#include <stdbool.h>

typedef struct {
    bool ext1_wake;     // woken by the button
    bool deep_sleep_wake; // any wake from deep sleep (pads may still be held)
} power_boot_info_t;

// First thing in app_main. Handles, before any pad hold is released:
//  - the stuck-button path (EXT1 wake with the stuck flag): re-sleep without UI;
//  - a crash while entering deep sleep: park the display and sleep;
//  - the boot-loop guard (3 watchdog/panic/brownout resets in a row without
//    reaching deep sleep): park the display and sleep until the button is pressed.
// Any of these may enter deep sleep and not return.
void power_early_boot(power_boot_info_t *info);

// Release the pad holds of the display and UART pins: each pin is first driven
// to its hold level as a GPIO, then gpio_hold_dis(). Call before initialising
// SPI, UART and LEDC.
void power_release_holds(void);

// Clears the crash counter. Reaching deep sleep clears it as well; main.c calls
// this only after a long healthy run without sleep.
void power_mark_healthy(void);

// Full deep-sleep entry from the app task (never from an esp_timer callback):
// jk_bms_stop, button_stop, display_prepare_deep_sleep (TWDT fed before each),
// pad holds, TWDT unsubscribe, wait for the button release (stuck -> wake on
// release), EXT1 arm, esp_deep_sleep_start.
void power_enter_deep_sleep(void) __attribute__((noreturn));
