// ST7789 240x240 over SPI2 + LVGL (esp_lvgl_port) + LEDC backlight.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Bring up SPI, the panel and LVGL. fast_wake: the panel was left in sleep-in
// with VCC on and RST high, so only SLPOUT (plus COLMOD/RAMCTRL/INVON) is sent
// instead of a reset and full init. build_ui() runs under the LVGL lock before
// the first frame is rendered; the backlight comes on after that frame.
esp_err_t display_init(bool fast_wake, void (*build_ui)(void));

// LVGL lock for any lv_* call made outside the LVGL task. timeout_ms 0 = forever.
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);

// Backlight brightness in percent (0 = off). Call from the app task.
void display_set_backlight(uint8_t pct);

// Stop LVGL (the lock is taken and never released), switch the backlight off
// (LEDC stopped), then DISPOFF + SLPIN, or power the panel off when the LCD
// power switch is used. power.c then holds the pins. No way back.
void display_prepare_deep_sleep(void);
