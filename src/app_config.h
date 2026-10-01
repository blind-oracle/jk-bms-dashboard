// Kconfig -> compile-time constants for the application.
//
// Every Kconfig bool is mapped to a 0/1 constant so that code can use
// `if (APP_X)` instead of `#if`: every configuration then compiles and
// type-checks every branch (IDF 6.1 builds with -Werror), and the optimizer
// still drops the dead branch.
#pragma once

#include <assert.h>
#include "sdkconfig.h"

// --- Pins ---
#define APP_PIN_LCD_SCLK   CONFIG_BMSD_PIN_LCD_SCLK
#define APP_PIN_LCD_MOSI   CONFIG_BMSD_PIN_LCD_MOSI
#define APP_PIN_LCD_CS     CONFIG_BMSD_PIN_LCD_CS
#define APP_PIN_LCD_DC     CONFIG_BMSD_PIN_LCD_DC
#define APP_PIN_LCD_RST    CONFIG_BMSD_PIN_LCD_RST
#define APP_PIN_LCD_BL     CONFIG_BMSD_PIN_LCD_BL
#define APP_PIN_LCD_PWR    CONFIG_BMSD_PIN_LCD_PWR      // -1 = not used
#define APP_PIN_BUTTON     CONFIG_BMSD_PIN_BUTTON
#define APP_PIN_BMS_RX     CONFIG_BMSD_PIN_BMS_RX
#define APP_PIN_BMS_TX     CONFIG_BMSD_PIN_BMS_TX
#define APP_PIN_VBAT_ADC   CONFIG_BMSD_PIN_VBAT_ADC

#ifdef CONFIG_BMSD_LCD_PWR_ACTIVE_HIGH
#define APP_LCD_PWR_ON_LEVEL 1
#else
#define APP_LCD_PWR_ON_LEVEL 0
#endif
#define APP_LCD_PWR_USED (APP_PIN_LCD_PWR >= 0)

// --- Timing ---
#define APP_SLEEP_TIMEOUT_S     CONFIG_BMSD_SLEEP_TIMEOUT_S   // 0 = never sleep
#define APP_SLEEP_WARN_S        CONFIG_BMSD_SLEEP_WARN_S
#define APP_POLL_INTERVAL_MS    CONFIG_BMSD_POLL_INTERVAL_MS
#define APP_BMS_RX_TIMEOUT_MS   CONFIG_BMSD_BMS_RX_TIMEOUT_MS
#define APP_BMS_OFFLINE_POLLS   CONFIG_BMSD_BMS_OFFLINE_POLLS

// --- Button ---
#define APP_HOLD_START_MS       CONFIG_BMSD_HOLD_START_MS
#define APP_LONG_PRESS_MS       CONFIG_BMSD_LONG_PRESS_MS
#define APP_ARM_WINDOW_MS       CONFIG_BMSD_ARM_WINDOW_MS
#define APP_BTN_DEBOUNCE_MS     30
#define APP_BTN_RELEASE_STABLE_MS 50
#define APP_BTN_STUCK_MS        10000   // wake press held this long = stuck button

// --- Display ---
#define APP_LCD_H_RES           240
#define APP_LCD_V_RES           240
#define APP_LCD_SPI_HZ          (CONFIG_BMSD_LCD_SPI_MHZ * 1000 * 1000)
#define APP_LCD_SPI_MODE        CONFIG_BMSD_LCD_SPI_MODE
#define APP_LCD_GAP_X           CONFIG_BMSD_LCD_GAP_X
#define APP_LCD_GAP_Y           CONFIG_BMSD_LCD_GAP_Y
#define APP_LCD_BRIGHTNESS_PCT  CONFIG_BMSD_LCD_BRIGHTNESS_PCT
#define APP_LCD_DIM_PCT         CONFIG_BMSD_LCD_DIM_PCT

#ifdef CONFIG_BMSD_LCD_INVERT
#define APP_LCD_INVERT 1
#else
#define APP_LCD_INVERT 0
#endif

#ifdef CONFIG_BMSD_LCD_BGR
#define APP_LCD_BGR 1
#else
#define APP_LCD_BGR 0
#endif

#ifdef CONFIG_BMSD_LCD_BL_ACTIVE_HIGH
#define APP_LCD_BL_ON_LEVEL 1
#else
#define APP_LCD_BL_ON_LEVEL 0
#endif
#define APP_LCD_BL_OFF_LEVEL (1 - APP_LCD_BL_ON_LEVEL)

#ifdef CONFIG_BMSD_LCD_FAST_WAKE
#define APP_LCD_FAST_WAKE 1
#else
#define APP_LCD_FAST_WAKE 0
#endif

// Rotation -> esp_lvgl_port rotation flags. The gap defaults in Kconfig follow
// this table: 180 deg needs y=80 and 270 deg needs x=80 on 240x240 glass
// mounted on the 240x320 ST7789 RAM.
#if defined(CONFIG_BMSD_LCD_ROTATION_90)
#define APP_LCD_SWAP_XY  1
#define APP_LCD_MIRROR_X 1
#define APP_LCD_MIRROR_Y 0
#elif defined(CONFIG_BMSD_LCD_ROTATION_180)
#define APP_LCD_SWAP_XY  0
#define APP_LCD_MIRROR_X 1
#define APP_LCD_MIRROR_Y 1
#elif defined(CONFIG_BMSD_LCD_ROTATION_270)
#define APP_LCD_SWAP_XY  1
#define APP_LCD_MIRROR_X 0
#define APP_LCD_MIRROR_Y 1
#else
#define APP_LCD_SWAP_XY  0
#define APP_LCD_MIRROR_X 0
#define APP_LCD_MIRROR_Y 0
#endif

// --- BMS ---
#ifdef CONFIG_BMSD_JK_FRAME_SOURCE_PC
#define APP_JK_FRAME_SOURCE 0x03
#else
#define APP_JK_FRAME_SOURCE 0x02
#endif
#define APP_JK_WRITE_DELAY_MS   CONFIG_BMSD_JK_WRITE_DELAY_MS

#ifdef CONFIG_BMSD_JK_SEND_ACTIVATION
#define APP_JK_SEND_ACTIVATION 1
#else
#define APP_JK_SEND_ACTIVATION 0
#endif

// --- Board battery ---
#define APP_VBAT_DIVIDER_X1000  CONFIG_BMSD_VBAT_DIVIDER_X1000
#define APP_VBAT_OFFSET_MV      CONFIG_BMSD_VBAT_OFFSET_MV
#define APP_VBAT_WARN_MV        CONFIG_BMSD_VBAT_WARN_MV
#define APP_VBAT_HYSTERESIS_MV  50
#define APP_VBAT_SAMPLES        CONFIG_BMSD_VBAT_SAMPLES
#define APP_VBAT_PERIOD_MS      5000

// --- Development ---
#ifdef CONFIG_BMSD_DEMO_MODE
#define APP_DEMO_MODE 1
#else
#define APP_DEMO_MODE 0
#endif

#ifdef CONFIG_BMSD_NO_SLEEP_WITH_USB_HOST
#define APP_NO_SLEEP_WITH_USB_HOST 1
#else
#define APP_NO_SLEEP_WITH_USB_HOST 0
#endif

// --- Sanity checks ---
static_assert(APP_PIN_BUTTON <= 7, "The button must be on an LP GPIO (0-7) to wake from deep sleep");

// Every pin in use, as a bit mask (the optional LCD power pin only if set)
#define APP_PIN_BIT(p)  ((p) >= 0 ? (1ULL << (p)) : 0ULL)
#define APP_PINS_SUM    (APP_PIN_BIT(APP_PIN_LCD_SCLK) + APP_PIN_BIT(APP_PIN_LCD_MOSI) + \
                         APP_PIN_BIT(APP_PIN_LCD_CS) + APP_PIN_BIT(APP_PIN_LCD_DC) + \
                         APP_PIN_BIT(APP_PIN_LCD_RST) + APP_PIN_BIT(APP_PIN_LCD_BL) + \
                         APP_PIN_BIT(APP_PIN_LCD_PWR) + APP_PIN_BIT(APP_PIN_BUTTON) + \
                         APP_PIN_BIT(APP_PIN_BMS_RX) + APP_PIN_BIT(APP_PIN_BMS_TX) + \
                         APP_PIN_BIT(APP_PIN_VBAT_ADC))
#define APP_PINS_MASK   (APP_PIN_BIT(APP_PIN_LCD_SCLK) | APP_PIN_BIT(APP_PIN_LCD_MOSI) | \
                         APP_PIN_BIT(APP_PIN_LCD_CS) | APP_PIN_BIT(APP_PIN_LCD_DC) | \
                         APP_PIN_BIT(APP_PIN_LCD_RST) | APP_PIN_BIT(APP_PIN_LCD_BL) | \
                         APP_PIN_BIT(APP_PIN_LCD_PWR) | APP_PIN_BIT(APP_PIN_BUTTON) | \
                         APP_PIN_BIT(APP_PIN_BMS_RX) | APP_PIN_BIT(APP_PIN_BMS_TX) | \
                         APP_PIN_BIT(APP_PIN_VBAT_ADC))
// GPIO12/13 are USB D-/D+, GPIO24-30 are the ESP32-C6FH4's in-package flash
#define APP_PINS_FORBIDDEN  ((1ULL << 12) | (1ULL << 13) | (0x7FULL << 24))
static_assert(APP_PINS_SUM == APP_PINS_MASK, "Two BMSD_PIN_* options use the same GPIO");
static_assert((APP_PINS_MASK & APP_PINS_FORBIDDEN) == 0,
              "A BMSD_PIN_* option uses GPIO12/13 (USB) or GPIO24-30 (flash)");
static_assert(APP_LONG_PRESS_MS >= APP_HOLD_START_MS + 500,
              "BMSD_LONG_PRESS_MS must be at least 500 ms longer than BMSD_HOLD_START_MS");
