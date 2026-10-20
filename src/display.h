// Display backend: ST7789 240x240 over SPI2 + LEDC backlight (display_st7789.c),
// or SSD1309 128x64 over I2C used as 64x128 portrait (display_ssd1309.c). Both
// run LVGL through esp_lvgl_port. The build selects one (BMSD_DISPLAY).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Bring up the bus, the panel and LVGL. build_ui() runs under the LVGL lock
// before the first frame is rendered; the panel lights up after that frame.
// fast_wake (ST7789 only): the panel was left in sleep-in with VCC on and RST
// high, so only SLPOUT (plus COLMOD/RAMCTRL/INVON) is sent instead of a reset
// and full init. The OLED always does a full init.
esp_err_t display_init(bool fast_wake, void (*build_ui)(void));

// LVGL lock for any lv_* call made outside the LVGL task. timeout_ms 0 = forever.
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);

// Brightness in percent (backlight on the TFT, contrast on the OLED; 0 = off
// on the TFT). Call from the app task.
void display_set_backlight(uint8_t pct);

// Stop LVGL (the lock is taken and never released), switch the panel off
// (TFT: backlight off, DISPOFF + SLPIN; OLED: display off), then power it off
// when the LCD power switch is used. power.c then holds the pins. No way back.
void display_prepare_deep_sleep(void);

// A pin the display uses and the level it holds in deep sleep
typedef struct {
    int     pin;
    uint8_t level;
} display_pin_level_t;

// The display's pins with their deep-sleep levels, power switch first if used.
// power.c holds them in sleep and releases them at boot in this order.
// Returns the number of entries written (at most cap).
size_t display_sleep_pins(display_pin_level_t *out, size_t cap);

// Make the panel dark without the display driver (boot-loop guard, crash
// during shutdown). Runs before any driver is initialised; power.c then holds
// display_sleep_pins().
void display_park_panel(void);
