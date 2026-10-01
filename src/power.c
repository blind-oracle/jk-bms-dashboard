// Deep sleep entry, wake-up handling, pad holds and the boot-loop guard.
//
// In deep sleep every display pin is held at a fixed level (the display stays
// dark and is never back-fed) and both UART lines are held high-impedance (a
// possibly unpowered BMS is never driven). The only wake source is EXT1 on the
// button: on a press normally, or on release if the button was still pressed
// at sleep entry (s_btn_stuck), which power_early_boot() handles without UI.
#include "power.h"

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "app_config.h"
#include "button.h"
#include "display.h"
#include "jk_bms.h"

static const char *TAG = "power";

#define CRASH_MAGIC         0x424D5344u // "BMSD"
#define SHUTDOWN_MAGIC      0x534C5050u // "SLPP": power_enter_deep_sleep() in progress
#define CRASH_LIMIT         3           // consecutive crash/brownout resets before parking
#define CRASH_COUNT_MAX     255
#define EARLY_SAMPLE_MS     30          // early-boot button check
#define LCD_RST_PULSE_MS    10
#define RELEASE_POLL_MS     5
#define LOG_FLUSH_MS        10
#define FALLBACK_WAKE_US    (60LL * 1000 * 1000)
#define DISPLAY_PIN_MAX     7

typedef struct {
    gpio_num_t pin;
    uint32_t   level;
} pin_level_t;

typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t shutdown;  // SHUTDOWN_MAGIC while entering deep sleep
} crash_info_t;

// The button was still pressed when the board went to sleep, so EXT1 waits for
// its release. RTC data: 0 after power-on and any non-deep-sleep reset.
static RTC_DATA_ATTR uint8_t s_btn_stuck = 0;
// Consecutive crash resets. Survives every reset but is random after power-on,
// hence the magic.
static RTC_NOINIT_ATTR crash_info_t s_crash;

// ---- Pins ----

// Display pins with the levels they hold in deep sleep, backlight first.
// Panel left powered in SLPIN: CS, DC and RES high, clock and data low. With
// the power switch every line is low, so the unpowered panel is not back-fed.
static size_t display_sleep_pins(pin_level_t out[DISPLAY_PIN_MAX])
{
    const bool sw = APP_LCD_PWR_USED;
    size_t n = 0;
    if (sw) {
        out[n++] = (pin_level_t){ .pin = (gpio_num_t)APP_PIN_LCD_PWR, .level = 1 - APP_LCD_PWR_ON_LEVEL };
    }
    out[n++] = (pin_level_t){ .pin = (gpio_num_t)APP_PIN_LCD_BL,   .level = sw ? 0 : APP_LCD_BL_OFF_LEVEL };
    out[n++] = (pin_level_t){ .pin = (gpio_num_t)APP_PIN_LCD_CS,   .level = sw ? 0 : 1 };
    out[n++] = (pin_level_t){ .pin = (gpio_num_t)APP_PIN_LCD_DC,   .level = sw ? 0 : 1 };
    out[n++] = (pin_level_t){ .pin = (gpio_num_t)APP_PIN_LCD_RST,  .level = sw ? 0 : 1 };
    out[n++] = (pin_level_t){ .pin = (gpio_num_t)APP_PIN_LCD_SCLK, .level = 0 };
    out[n++] = (pin_level_t){ .pin = (gpio_num_t)APP_PIN_LCD_MOSI, .level = 0 };
    return n;
}

// Plain GPIO output at level, detached from any SPI/LEDC/UART signal, with
// any pad hold released. No gpio_config: it would reserve the pin and clash
// with the drivers that own it while awake.
static void drive_output(gpio_num_t pin, uint32_t level)
{
    gpio_set_level(pin, level);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_hold_dis(pin);
}

static void hold_output(gpio_num_t pin, uint32_t level)
{
    drive_output(pin, level);
    gpio_hold_en(pin);
}

// High impedance: output, input buffer and pulls off
static void hold_hiz(gpio_num_t pin)
{
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    gpio_set_direction(pin, GPIO_MODE_DISABLE);
    gpio_hold_dis(pin);
    gpio_hold_en(pin);
}

static void hold_display_pins(void)
{
    pin_level_t pins[DISPLAY_PIN_MAX];
    const size_t n = display_sleep_pins(pins);
    for (size_t i = 0; i < n; i++) {
        hold_output(pins[i].pin, pins[i].level);
    }
}

static void hold_uart_pins(void)
{
    hold_hiz((gpio_num_t)APP_PIN_BMS_TX);
    hold_hiz((gpio_num_t)APP_PIN_BMS_RX);
}

// Boot-loop guard: dark display without the display driver. A RES pulse puts
// the ST7789 in its reset state (sleep-in, display off); CS is already high
// by then. With the power switch the panel is simply left unpowered.
static void park_display(void)
{
    pin_level_t pins[DISPLAY_PIN_MAX];
    const size_t n = display_sleep_pins(pins);
    for (size_t i = 0; i < n; i++) {
        if (!APP_LCD_PWR_USED && pins[i].pin == APP_PIN_LCD_RST) {
            continue;   // pulsed below
        }
        hold_output(pins[i].pin, pins[i].level);
    }
    if (!APP_LCD_PWR_USED) {
        drive_output((gpio_num_t)APP_PIN_LCD_RST, 0);
        esp_rom_delay_us(LCD_RST_PULSE_MS * 1000);
        hold_output((gpio_num_t)APP_PIN_LCD_RST, 1);
    }
}

void power_release_holds(void)
{
    pin_level_t pins[DISPLAY_PIN_MAX];
    const size_t n = display_sleep_pins(pins);
    for (size_t i = 0; i < n; i++) {
        gpio_set_level(pins[i].pin, pins[i].level);
        gpio_set_direction(pins[i].pin, GPIO_MODE_OUTPUT);
        gpio_hold_dis(pins[i].pin);
    }

    const gpio_num_t tx = (gpio_num_t)APP_PIN_BMS_TX;
    const gpio_num_t rx = (gpio_num_t)APP_PIN_BMS_RX;
    if (APP_DEMO_MODE) {
        // The UART is never started: TX stays floating, as in deep sleep
        gpio_set_pull_mode(tx, GPIO_FLOATING);
        gpio_set_direction(tx, GPIO_MODE_DISABLE);
    } else {
        gpio_set_level(tx, 1);                      // UART idle level until the driver takes over
        gpio_set_direction(tx, GPIO_MODE_OUTPUT);
    }
    gpio_hold_dis(tx);
    gpio_set_direction(rx, GPIO_MODE_INPUT);
    gpio_hold_dis(rx);
}

// ---- Button and EXT1 ----

static void config_button_input(void)
{
    gpio_hold_dis((gpio_num_t)APP_PIN_BUTTON);
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(APP_PIN_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

// Early boot: true if the button reads released for the whole window (1 ms steps)
static bool sample_button_released(uint32_t window_ms)
{
    config_button_input();
    esp_rom_delay_us(100);  // pull-up settling
    for (uint32_t i = 0; i < window_ms; i++) {
        if (button_read_raw()) {
            return false;
        }
        esp_rom_delay_us(1000);
    }
    return !button_read_raw();
}

// Up to APP_BTN_STUCK_MS for APP_BTN_RELEASE_STABLE_MS of continuous release
static bool wait_button_release(void)
{
    const int64_t start_us = esp_timer_get_time();
    int64_t released_us = -1;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (button_read_raw()) {
            released_us = -1;
        } else if (released_us < 0) {
            released_us = now;
        }
        if (released_us >= 0 && now - released_us >= (int64_t)APP_BTN_RELEASE_STABLE_MS * 1000) {
            return true;
        }
        if (now - start_us >= (int64_t)APP_BTN_STUCK_MS * 1000) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(RELEASE_POLL_MS));
    }
}

static void arm_ext1(esp_sleep_ext1_wakeup_mode_t mode)
{
    const gpio_num_t btn = (gpio_num_t)APP_PIN_BUTTON;
    gpio_hold_dis(btn);
    rtc_gpio_init(btn);
    rtc_gpio_set_direction(btn, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(btn);
    rtc_gpio_pullup_en(btn);

    // The button is the only wake source
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_disable_ext1_wakeup_io(0);
    const esp_err_t err = esp_sleep_enable_ext1_wakeup_io(BIT64(APP_PIN_BUTTON), mode);
    if (err != ESP_OK) {
        // Never sleep without a way back
        ESP_LOGE(TAG, "EXT1 wake-up on GPIO%d failed (%s), waking by timer instead",
                 APP_PIN_BUTTON, esp_err_to_name(err));
        esp_sleep_enable_timer_wakeup(FALLBACK_WAKE_US);
    }
}

static void __attribute__((noreturn)) go_to_sleep(void)
{
    vTaskDelay(pdMS_TO_TICKS(LOG_FLUSH_MS));    // let the last log line reach the USB host
    esp_deep_sleep_disable_rom_logging();
    esp_deep_sleep_start();
}

// released: wake on the next press. Otherwise the button is stuck: wake on
// its release and handle that wake in power_early_boot() without UI.
static void __attribute__((noreturn)) sleep_until_button(bool released)
{
    if (released) {
        s_btn_stuck = 0;
        arm_ext1(ESP_EXT1_WAKEUP_ANY_LOW);
    } else {
        s_btn_stuck = 1;
        arm_ext1(ESP_EXT1_WAKEUP_ANY_HIGH);
    }
    go_to_sleep();
}

// ---- Boot ----

// Brownouts count too: a nearly flat cell that collapses whenever the
// backlight comes on would otherwise reboot forever instead of sleeping
static bool is_crash_reset(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_CPU_LOCKUP:
    case ESP_RST_BROWNOUT:
        return true;
    default:
        return false;
    }
}

// Resets a person triggered on purpose: they end a crash series
static bool is_manual_reset(esp_reset_reason_t reason)
{
    return reason == ESP_RST_POWERON || reason == ESP_RST_EXT || reason == ESP_RST_USB ||
           reason == ESP_RST_JTAG;
}

void power_early_boot(power_boot_info_t *info)
{
    power_boot_info_t local;
    if (info == NULL) {
        info = &local;
    }
    const uint32_t causes = esp_sleep_get_wakeup_causes();
    const esp_reset_reason_t reason = esp_reset_reason();
    info->ext1_wake = (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) != 0;
    info->deep_sleep_wake = reason == ESP_RST_DEEPSLEEP;

    // (1) Release of a button that was stuck at sleep entry. Startup released
    // only the button's pad hold, so the display and UART pins are still held.
    if (info->ext1_wake && s_btn_stuck) {
        const bool released = sample_button_released(EARLY_SAMPLE_MS);
        ESP_LOGI(TAG, "stuck-button wake: %s",
                 released ? "released, sleeping until the next press" : "still pressed, sleeping until release");
        sleep_until_button(released);
    }

    // (2) Boot-loop guard
    if (s_crash.magic != CRASH_MAGIC || is_manual_reset(reason)) {
        s_crash.magic = CRASH_MAGIC;
        s_crash.count = 0;
        s_crash.shutdown = 0;
    }
    const bool crashed_in_shutdown = s_crash.shutdown == SHUTDOWN_MAGIC;
    s_crash.shutdown = 0;
    if (is_crash_reset(reason)) {
        if (s_crash.count < CRASH_COUNT_MAX) {
            s_crash.count++;
        }
        ESP_LOGW(TAG, "crash reset (reason %d), %u in a row", (int)reason, (unsigned)s_crash.count);
    }
    if (info->ext1_wake) {
        s_crash.count = 0;
    }
    // A crash while entering deep sleep: finish the job without bringing the UI back
    if (is_crash_reset(reason) && crashed_in_shutdown) {
        park_display();
        hold_uart_pins();
        ESP_LOGW(TAG, "crash while entering deep sleep: display parked, sleeping until the button is pressed");
        sleep_until_button(sample_button_released(EARLY_SAMPLE_MS));
    }
    if (s_crash.count >= CRASH_LIMIT) {
        park_display();
        hold_uart_pins();
        ESP_LOGW(TAG, "%u crash resets in a row: display parked, sleeping until the button is pressed",
                 (unsigned)s_crash.count);
        // A button held right now would wake the board at once: wait for its release instead
        sleep_until_button(sample_button_released(EARLY_SAMPLE_MS));
    }
}

void power_mark_healthy(void)
{
    if (s_crash.magic == CRASH_MAGIC && s_crash.count != 0) {
        ESP_LOGI(TAG, "running fine, crash counter cleared");
    }
    s_crash.magic = CRASH_MAGIC;
    s_crash.count = 0;
}

static void twdt_feed(bool subscribed)
{
    if (subscribed) {
        esp_task_wdt_reset();
    }
}

void power_enter_deep_sleep(void)
{
    // The TWDT keeps guarding jk_bms_stop() (<= 1 s) and
    // display_prepare_deep_sleep() (<= about 2.5 s), fed before each: a hang
    // there resets the board, and the shutdown marker then makes the next
    // boot finish the job without UI. It is dropped before the button wait.
    const bool twdt = esp_task_wdt_status(NULL) == ESP_OK;
    s_crash.shutdown = SHUTDOWN_MAGIC;
    twdt_feed(twdt);
    jk_bms_stop();
    twdt_feed(twdt);
    button_stop();
    twdt_feed(twdt);
    display_prepare_deep_sleep();
    twdt_feed(twdt);
    hold_display_pins();
    hold_uart_pins();
    if (twdt) {
        esp_task_wdt_delete(NULL);
    }

    // button.c left the pad as an input with pull-up; re-applying it is harmless
    config_button_input();
    const bool released = wait_button_release();
    ESP_LOGI(TAG, "deep sleep, wake on button %s",
             released ? "press" : "release (button stuck)");
    // Reaching deep sleep ends any crash series
    s_crash.count = 0;
    s_crash.shutdown = 0;
    sleep_until_button(released);
}
