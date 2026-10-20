// SSD1309 128x64 OLED over I2C, used as a 64x128 portrait display, + LVGL.
// Built when BMSD_DISPLAY_SSD1309 is selected.
//
// esp_lvgl_port runs the LVGL task, tick and lock, but the display is created
// here: a 64x128 LV_COLOR_FORMAT_I1 display in full render mode whose flush
// transposes the frame into the SSD1309's 8 pages x 128 columns. IDF's SSD1306
// panel object is used only for draw_bitmap: its init sends the SSD1306 charge
// pump command, which the SSD1309 lacks, so the init sequence is ours.
//
// Power-up: (power switch on) -> 20 ms for the module's LDO and RC reset ->
// I2C bus, panel IO -> init with the display off -> LVGL, UI, first frame ->
// display on once VCC has had 100 ms -> contrast.
#include "display.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "app_config.h"

static const char *TAG = "display";

#define OLED_PHYS_W             128
#define OLED_PHYS_H             64
#define OLED_PAGE_BYTES         (OLED_PHYS_W * OLED_PHYS_H / 8)

// LVGL I1 buffer: an 8-byte palette, then 1-bpp rows (MSB first, 1 = light)
#define I1_PALETTE_BYTES        8
#define I1_STRIDE               (APP_OLED_W / 8)
#define LV_BUF_BYTES            (I1_PALETTE_BYTES + I1_STRIDE * APP_OLED_H)

// SSD1309 commands
#define CMD_DISPLAY_OFF         0xAE
#define CMD_DISPLAY_ON          0xAF
#define CMD_CONTRAST            0x81
#define CMD_UNLOCK              0xFD
#define CMD_START_LINE_0        0x40
#define CMD_SEG_REMAP_OFF       0xA0
#define CMD_SEG_REMAP_ON        0xA1
#define CMD_MULTIPLEX           0xA8
#define CMD_COM_SCAN_UP         0xC0
#define CMD_COM_SCAN_DOWN       0xC8
#define CMD_DISPLAY_OFFSET      0xD3
#define CMD_CLOCK_DIV           0xD5
#define CMD_PRECHARGE           0xD9
#define CMD_COM_PINS            0xDA
#define CMD_VCOMH               0xDB
#define CMD_ADDR_MODE           0x20
#define CMD_RESUME_RAM          0xA4
#define CMD_NORMAL              0xA6

// Timing
#define OLED_PWR_SETTLE_MS      20      // module LDO + 10k/100nF reset RC
#define OLED_VCC_SETTLE_MS      100     // SSD1309: VCC stable before display on
#define OLED_RST_PULSE_MS       10
#define I2C_TIMEOUT_MS          100

#define LVGL_TASK_STACK         8192
// One full refresh holds the lock for about 30 ms (1 KB at 400 kHz)
#define LVGL_SLEEP_LOCK_MS      2000

static i2c_master_bus_handle_t s_bus;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static bool s_init_called;
static bool s_lvgl_ready;       // lvgl_port_init succeeded
static bool s_ready;            // display_init completed: contrast may be changed
static bool s_sleeping;         // display_prepare_deep_sleep was called
static int64_t s_power_on_us;   // when the module got power (or init started)

static uint8_t s_lv_buf[LV_BUF_BYTES] __attribute__((aligned(LV_DRAW_BUF_ALIGN)));
static uint8_t s_page_buf[OLED_PAGE_BYTES];

static uint8_t pct_to_contrast(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    return (uint8_t)((uint32_t)pct * 255 / 100);
}

// Optional load switch in the module's VCC line (the pin hold was released by power.c)
static esp_err_t panel_power(bool on)
{
    const gpio_num_t pin = (gpio_num_t)APP_PIN_LCD_PWR;
    const uint32_t level = on ? APP_LCD_PWR_ON_LEVEL : 1 - APP_LCD_PWR_ON_LEVEL;
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, level), TAG, "OLED power level");
    ESP_RETURN_ON_ERROR(gpio_set_direction(pin, GPIO_MODE_OUTPUT), TAG, "OLED power pin");
    if (on) {
        s_power_on_us = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(OLED_PWR_SETTLE_MS));
    }
    return ESP_OK;
}

static esp_err_t bus_create(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = (gpio_num_t)APP_PIN_OLED_SDA,
        .scl_io_num = (gpio_num_t)APP_PIN_OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = 1,    // the module has 4.7k pull-ups; this is a fallback
        },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "I2C bus");

    // Control byte 0x00 = command, 0x40 = data (D/C# is bit 6)
    const esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = APP_OLED_I2C_ADDR,
        .scl_speed_hz = APP_OLED_I2C_HZ,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .transaction_timeout_ms = I2C_TIMEOUT_MS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_bus, &io_cfg, &s_io), TAG, "panel IO");

    // Only draw_bitmap is used; the init below is SSD1309-specific
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = (gpio_num_t)APP_PIN_OLED_RST,
        .bits_per_pixel = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(s_io, &panel_cfg, &s_panel), TAG, "SSD1306 panel object");
    return ESP_OK;
}

static esp_err_t cmd(uint8_t command, const uint8_t *params, size_t len)
{
    return esp_lcd_panel_io_tx_param(s_io, command, params, len);
}

// The LCD wiki's 2.42" SSD1309 sequence (MC242GX) without its final display-on,
// plus horizontal addressing for the driver's column/page windowing. The panel
// stays off until the first frame is in GDDRAM. BMSD_OLED_FLIP turns the picture
// by 180 degrees with the segment and COM remaps.
static esp_err_t panel_init(void)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");   // no-op without RES

    const uint8_t contrast = pct_to_contrast(APP_LCD_BRIGHTNESS_PCT);
    esp_err_t err = cmd(CMD_DISPLAY_OFF, NULL, 0);
    if (err == ESP_OK) err = cmd(CMD_UNLOCK, (const uint8_t[]){0x12}, 1);
    if (err == ESP_OK) err = cmd(CMD_START_LINE_0, NULL, 0);
    if (err == ESP_OK) err = cmd(CMD_CONTRAST, &contrast, 1);
    if (err == ESP_OK) err = cmd(APP_OLED_FLIP ? CMD_SEG_REMAP_OFF : CMD_SEG_REMAP_ON, NULL, 0);
    if (err == ESP_OK) err = cmd(CMD_MULTIPLEX, (const uint8_t[]){OLED_PHYS_H - 1}, 1);
    if (err == ESP_OK) err = cmd(APP_OLED_FLIP ? CMD_COM_SCAN_UP : CMD_COM_SCAN_DOWN, NULL, 0);
    if (err == ESP_OK) err = cmd(CMD_DISPLAY_OFFSET, (const uint8_t[]){0x00}, 1);
    if (err == ESP_OK) err = cmd(CMD_CLOCK_DIV, (const uint8_t[]){0xA0}, 1);
    if (err == ESP_OK) err = cmd(CMD_PRECHARGE, (const uint8_t[]){0xF1}, 1);
    if (err == ESP_OK) err = cmd(CMD_COM_PINS, (const uint8_t[]){0x12}, 1);
    if (err == ESP_OK) err = cmd(CMD_VCOMH, (const uint8_t[]){0x34}, 1);
    if (err == ESP_OK) err = cmd(CMD_ADDR_MODE, (const uint8_t[]){0x00}, 1);
    if (err == ESP_OK) err = cmd(CMD_RESUME_RAM, NULL, 0);
    if (err == ESP_OK) err = cmd(CMD_NORMAL, NULL, 0);
    ESP_RETURN_ON_ERROR(err, TAG, "SSD1309 init (no ACK from 0x%02X?)", (unsigned)APP_OLED_I2C_ADDR);
    return ESP_OK;
}

// Logical 64x128 portrait -> physical 128x64 landscape: column = y, row = 63 - x.
// Page buffer: byte (row / 8) * 128 + column, bit row % 8 (LSB = top row).
static void oled_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;     // full render mode: always the whole screen
    const uint8_t *src = px_map + I1_PALETTE_BYTES;
    memset(s_page_buf, 0, sizeof(s_page_buf));
    for (int y = 0; y < APP_OLED_H; y++) {
        const uint8_t *row = src + y * I1_STRIDE;
        for (int x = 0; x < APP_OLED_W; x++) {
            if (row[x >> 3] & (0x80 >> (x & 7))) {
                const int prow = OLED_PHYS_H - 1 - x;
                s_page_buf[(prow >> 3) * OLED_PHYS_W + y] |= (uint8_t)(1u << (prow & 7));
            }
        }
    }
    // The I2C transfer is synchronous: the buffer is free when this returns
    const esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, OLED_PHYS_W, OLED_PHYS_H, s_page_buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flush: %s", esp_err_to_name(err));
    }
    lv_display_flush_ready(disp);
}

// The port wakes its task for its own displays; do the same for this one
static void wake_lvgl_task(lv_event_t *e)
{
    (void)e;
    lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
}

static esp_err_t lvgl_start(void (*build_ui)(void))
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = LVGL_TASK_STACK;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL port");
    s_lvgl_ready = true;

    // Holding the lock until the first frame keeps the LVGL task from
    // rendering the empty default screen first
    lvgl_port_lock(0);
    s_disp = lv_display_create(APP_OLED_W, APP_OLED_H);
    if (s_disp == NULL) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "lv_display_create failed");
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(s_disp, s_lv_buf, NULL, sizeof(s_lv_buf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_disp, oled_flush);
    lv_display_add_event_cb(s_disp, wake_lvgl_task, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(s_disp, wake_lvgl_task, LV_EVENT_REFR_REQUEST, NULL);
    if (build_ui != NULL) {
        build_ui();
    }
    lv_refr_now(s_disp);    // the first frame goes to GDDRAM while the panel is off
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t display_init(bool fast_wake, void (*build_ui)(void))
{
    (void)fast_wake;    // always a full init: the module is powered off in sleep
    if (s_init_called) {
        ESP_LOGE(TAG, "display_init called twice");
        return ESP_ERR_INVALID_STATE;
    }
    s_init_called = true;
    const int64_t t0 = esp_timer_get_time();
    s_power_on_us = t0;

    esp_err_t err = ESP_OK;
    if (APP_LCD_PWR_USED) {
        err = panel_power(true);
    }
    if (err == ESP_OK) {
        err = bus_create();
    }
    if (err == ESP_OK) {
        err = panel_init();
    }
    if (err == ESP_OK) {
        err = lvgl_start(build_ui);
    }
    if (err == ESP_OK) {
        // SSD1309 power-up sequence: VCC stable for 100 ms before display on
        const int64_t on_at = s_power_on_us + (int64_t)OLED_VCC_SETTLE_MS * 1000;
        const int64_t now = esp_timer_get_time();
        if (APP_LCD_PWR_USED && now < on_at) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)((on_at - now) / 1000)) + 1);
        }
        lvgl_port_lock(0);
        err = cmd(CMD_DISPLAY_ON, NULL, 0);
        lvgl_port_unlock();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OLED init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "OLED init done in %d ms", (int)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

bool display_lock(uint32_t timeout_ms)
{
    // lvgl_port_lock() asserts before lvgl_port_init(); without a display there is nothing to draw
    if (s_disp == NULL || s_sleeping) {
        return false;
    }
    return lvgl_port_lock(timeout_ms);
}

void display_unlock(void)
{
    if (s_disp == NULL || s_sleeping) {
        return;
    }
    lvgl_port_unlock();
}

// Contrast. The I2C panel IO is shared with the flush, so it is sent under the lock.
void display_set_backlight(uint8_t pct)
{
    if (!s_ready || s_sleeping) {
        return;
    }
    const uint8_t contrast = pct_to_contrast(pct);
    lvgl_port_lock(0);
    const esp_err_t err = cmd(CMD_CONTRAST, &contrast, 1);
    lvgl_port_unlock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "contrast %u%%: %s", (unsigned)pct, esp_err_to_name(err));
    }
}

void display_prepare_deep_sleep(void)
{
    if (s_sleeping) {
        return;
    }
    s_sleeping = true;  // display_lock() and display_set_backlight() are refused from now on

    // Park LVGL: the lock is never released, so no flush can follow
    if (s_lvgl_ready) {
        if (!lvgl_port_lock(LVGL_SLEEP_LOCK_MS)) {
            ESP_LOGE(TAG, "LVGL lock timeout, shutting the panel down anyway");
        }
        const esp_err_t err = lvgl_port_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "lvgl_port_stop: %s", esp_err_to_name(err));
        }
    }

    // Display off before VCC goes (SSD1309 power-down order)
    if (s_io != NULL) {
        const esp_err_t err = cmd(CMD_DISPLAY_OFF, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "display off: %s", esp_err_to_name(err));
        }
    }
    if (APP_LCD_PWR_USED) {
        panel_power(false);     // power.c then holds the pin at this level
    }
}

// ---- Deep-sleep pins and driver-free parking (used by power.c) ----

// With the power switch the module is unpowered in sleep. Switch in VCC: every
// line low, so the module is not back-fed through its 4.7k pull-ups. Switch in
// GND: every line high, so the module finds no ground through its pins. Without
// a switch the lines stay high, so no current flows through those pull-ups.
size_t display_sleep_pins(display_pin_level_t *out, size_t cap)
{
    const bool sw = APP_LCD_PWR_USED;
    const uint8_t line = sw ? APP_LCD_PWR_OFF_LINE_LEVEL : 1;
    size_t n = 0;
    if (sw && n < cap) {
        out[n++] = (display_pin_level_t){ .pin = APP_PIN_LCD_PWR, .level = 1 - APP_LCD_PWR_ON_LEVEL };
    }
    if (n < cap) {
        out[n++] = (display_pin_level_t){ .pin = APP_PIN_OLED_SDA, .level = line };
    }
    if (n < cap) {
        out[n++] = (display_pin_level_t){ .pin = APP_PIN_OLED_SCL, .level = line };
    }
    if (APP_PIN_OLED_RST >= 0 && n < cap) {
        out[n++] = (display_pin_level_t){ .pin = APP_PIN_OLED_RST, .level = line };
    }
    return n;
}

// With the power switch the sleep pin levels cut the module's power. Without it
// the panel is reset (RES wired) or told to switch off over a temporary bus.
void display_park_panel(void)
{
    if (APP_LCD_PWR_USED) {
        return;
    }
    if (APP_PIN_OLED_RST >= 0) {
        const gpio_num_t rst = (gpio_num_t)APP_PIN_OLED_RST;
        gpio_hold_dis(rst);
        gpio_set_level(rst, 0);
        gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        esp_rom_delay_us(OLED_RST_PULSE_MS * 1000);
        gpio_set_level(rst, 1);     // after a reset the SSD1309 is in display-off mode
        return;
    }
    gpio_hold_dis((gpio_num_t)APP_PIN_OLED_SDA);
    gpio_hold_dis((gpio_num_t)APP_PIN_OLED_SCL);
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = (gpio_num_t)APP_PIN_OLED_SDA,
        .scl_io_num = (gpio_num_t)APP_PIN_OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        return;
    }
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = APP_OLED_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) == ESP_OK) {
        const uint8_t off[] = {0x00, CMD_DISPLAY_OFF};    // control byte: command
        i2c_master_transmit(dev, off, sizeof(off), I2C_TIMEOUT_MS);
        i2c_master_bus_rm_device(dev);
    }
    i2c_del_master_bus(bus);
}
