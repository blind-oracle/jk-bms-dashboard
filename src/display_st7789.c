// ST7789 240x240 over SPI2 + LVGL (esp_lvgl_port) + LEDC backlight.
// Built when BMSD_DISPLAY_ST7789 is selected.
//
// Init order: backlight PWM at duty 0 -> optional panel power -> SPI bus,
// panel IO, ST7789 -> full init (reset) or fast wake (SLPOUT only) ->
// invert/rotation/gap -> LVGL, UI, first frame -> DISPON -> backlight on.
// After lvgl_port_add_disp every esp_lcd call is made under the LVGL lock:
// the SPI panel IO is not thread-safe against the LVGL flush.
#include "display.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_common.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "app_config.h"

static const char *TAG = "display";

#define LCD_HOST                SPI2_HOST
#define LCD_DRAW_LINES          40      // height of each LVGL partial buffer
#define LCD_DRAW_BUF_PX         (APP_LCD_H_RES * LCD_DRAW_LINES)
#define LCD_TRANS_QUEUE_DEPTH   10

// Registers that esp_lcd_panel_init() writes, re-sent on a fast wake
#define ST7789_CMD_RAMCTRL      0xB0
#define ST7789_COLMOD_RGB565    0x55

// ST7789 timing
#define LCD_PWR_SETTLE_MS       50      // panel supply after the power switch turns on
#define LCD_RESET_SETTLE_MS     120     // no SLPOUT within 120 ms of a reset
#define LCD_SLPOUT_WAIT_MS      10      // >= 5 ms after SLPOUT before the next command
#define LCD_SLPOUT_TO_SLPIN_MS  120     // no SLPIN within 120 ms of SLPOUT

#define LVGL_TASK_STACK         8192    // port default 7168; LVGL log printf runs in it
// The LVGL task holds the lock for at most one refresh (about 100 ms at the
// 10 MHz minimum SPI clock); this wait plus DISPOFF/SLPIN stays well inside
// the 5 s task watchdog, which power.c feeds just before.
#define LVGL_SLEEP_LOCK_MS      2000

// Backlight PWM. The output is inverted for an active-low backlight, so duty 0
// means off for both polarities. The LEDC idle level goes through the same
// GPIO-matrix inversion, so the "off" idle level is 0 as well.
#define BL_MODE                 LEDC_LOW_SPEED_MODE
#define BL_TIMER                LEDC_TIMER_0
#define BL_CHANNEL              LEDC_CHANNEL_0
#define BL_RESOLUTION           LEDC_TIMER_10_BIT
#define BL_FREQ_HZ              5000
#define BL_DUTY_MAX             1023
#define BL_OUTPUT_INVERT        (!APP_LCD_BL_ON_LEVEL)
#define BL_IDLE_LEVEL_OFF       ((uint32_t)(APP_LCD_BL_OFF_LEVEL ^ BL_OUTPUT_INVERT))

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;    // set once lvgl_port_add_disp succeeded
static bool s_init_called;
static bool s_lvgl_ready;       // lvgl_port_init succeeded
static bool s_bl_ready;         // LEDC configured (duty 0)
static bool s_ready;            // display_init completed: the backlight may be switched on
static bool s_sleeping;         // display_prepare_deep_sleep was called
static int64_t s_slpout_us;     // esp_timer time of the last SLPOUT, 0 = none sent

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = BL_MODE,
        .duty_resolution = BL_RESOLUTION,
        .timer_num = BL_TIMER,
        .freq_hz = BL_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");

    const ledc_channel_config_t channel = {
        .gpio_num = APP_PIN_LCD_BL,
        .speed_mode = BL_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags = {
            .output_invert = BL_OUTPUT_INVERT,
        },
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel");
    s_bl_ready = true;
    return ESP_OK;
}

// Optional load switch in the panel's VCC line (the pin hold was released by power.c)
static esp_err_t panel_power(bool on)
{
    const gpio_num_t pin = (gpio_num_t)APP_PIN_LCD_PWR;
    const uint32_t level = on ? APP_LCD_PWR_ON_LEVEL : 1 - APP_LCD_PWR_ON_LEVEL;
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, level), TAG, "LCD power level");
    ESP_RETURN_ON_ERROR(gpio_set_direction(pin, GPIO_MODE_OUTPUT), TAG, "LCD power pin");
    if (on) {
        vTaskDelay(pdMS_TO_TICKS(LCD_PWR_SETTLE_MS));
    }
    return ESP_OK;
}

static esp_err_t panel_create(void)
{
    // Unused data lines must be -1 (0 would mean GPIO0)
    const spi_bus_config_t bus = {
        .sclk_io_num = APP_PIN_LCD_SCLK,
        .mosi_io_num = APP_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = LCD_DRAW_BUF_PX * 2,     // one RGB565 partial buffer per transfer
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SPI bus");

    // on_color_trans_done stays NULL: esp_lvgl_port registers its own
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = (gpio_num_t)APP_PIN_LCD_CS,
        .dc_gpio_num = (gpio_num_t)APP_PIN_LCD_DC,
        .spi_mode = APP_LCD_SPI_MODE,
        .pclk_hz = APP_LCD_SPI_HZ,
        .trans_queue_depth = LCD_TRANS_QUEUE_DEPTH,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io),
                        TAG, "panel IO");

    // Configures RST as an output without changing its level, so a fast wake keeps it high
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = (gpio_num_t)APP_PIN_LCD_RST,
        .rgb_ele_order = APP_LCD_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel), TAG, "ST7789 panel");
    return ESP_OK;
}

static esp_err_t panel_init_full(void)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    vTaskDelay(pdMS_TO_TICKS(LCD_RESET_SETTLE_MS));
    // SLPOUT, 100 ms, MADCTL, COLMOD, RAMCTRL
    s_slpout_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    return ESP_OK;
}

// The panel kept its registers and GRAM in sleep-in (VCC on, RST high): leave
// sleep and re-send the pixel format and RAM control in case they were reset.
static esp_err_t panel_wake_fast(void)
{
    uint8_t colmod[] = {ST7789_COLMOD_RGB565};
    uint8_t ramctrl[] = {0x00, 0xF0};   // big-endian RGB565, as the driver sets it

    s_slpout_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_SLPOUT, NULL, 0), TAG, "SLPOUT");
    vTaskDelay(pdMS_TO_TICKS(LCD_SLPOUT_WAIT_MS));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_COLMOD, colmod, sizeof(colmod)), TAG, "COLMOD");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, ST7789_CMD_RAMCTRL, ramctrl, sizeof(ramctrl)), TAG,
                        "RAMCTRL");
    return ESP_OK;
}

// Both init paths. swap_xy/mirror (re)send MADCTL, including the BGR bit.
static esp_err_t panel_configure(void)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, APP_LCD_INVERT), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, APP_LCD_SWAP_XY), TAG, "swap_xy");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, APP_LCD_MIRROR_X, APP_LCD_MIRROR_Y), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, APP_LCD_GAP_X, APP_LCD_GAP_Y), TAG, "gap");
    return ESP_OK;
}

static esp_err_t lvgl_start(void (*build_ui)(void))
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = LVGL_TASK_STACK;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL port");
    s_lvgl_ready = true;

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .buffer_size = LCD_DRAW_BUF_PX,
        .double_buffer = true,
        .hres = APP_LCD_H_RES,
        .vres = APP_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        // Same as already applied to the panel: add_disp sends MADCTL again
        .rotation = {
            .swap_xy = APP_LCD_SWAP_XY,
            .mirror_x = APP_LCD_MIRROR_X,
            .mirror_y = APP_LCD_MIRROR_Y,
        },
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 1,    // LVGL RGB565 is little-endian, the panel takes big-endian
        },
    };

    // The lock is recursive. Holding it from add_disp to DISPON keeps the LVGL
    // task from rendering the empty default screen before the UI exists.
    lvgl_port_lock(0);
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_disp == NULL) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_ERR_NO_MEM;
    }
    if (build_ui != NULL) {
        build_ui();
    }
    // The first frame goes to GRAM while the display is still off. DISPON
    // waits for the queued pixel transfers before it is sent.
    lv_refr_now(s_disp);
    const esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, true);
    lvgl_port_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "DISPON");
    return ESP_OK;
}

esp_err_t display_init(bool fast_wake, void (*build_ui)(void))
{
    if (s_init_called) {
        ESP_LOGE(TAG, "display_init called twice");
        return ESP_ERR_INVALID_STATE;
    }
    s_init_called = true;
    const int64_t t0 = esp_timer_get_time();
    // With the power switch the panel was unpowered: always do the full init.
    // LCD_FAST_WAKE=n is the escape hatch for panels that misbehave, so honour it here too.
    const bool fast = fast_wake && APP_LCD_FAST_WAKE && !APP_LCD_PWR_USED;

    // The PWM (duty 0) owns the backlight pin before the panel gets power
    esp_err_t err = backlight_init();
    if (err == ESP_OK && APP_LCD_PWR_USED) {
        err = panel_power(true);
    }
    if (err == ESP_OK) {
        err = panel_create();
    }
    if (err == ESP_OK) {
        err = fast ? panel_wake_fast() : panel_init_full();
    }
    if (err == ESP_OK) {
        err = panel_configure();
    }
    if (err == ESP_OK) {
        err = lvgl_start(build_ui);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s init failed: %s", fast ? "fast" : "full", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    display_set_backlight(APP_LCD_BRIGHTNESS_PCT);
    ESP_LOGI(TAG, "%s init done in %d ms", fast ? "fast" : "full", (int)((esp_timer_get_time() - t0) / 1000));
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

void display_set_backlight(uint8_t pct)
{
    if (!s_ready || s_sleeping) {
        return;
    }
    if (pct > 100) {
        pct = 100;
    }
    const uint32_t duty = (uint32_t)pct * BL_DUTY_MAX / 100;
    esp_err_t err = ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(BL_MODE, BL_CHANNEL);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight %u%%: %s", (unsigned)pct, esp_err_to_name(err));
    }
}

void display_prepare_deep_sleep(void)
{
    if (s_sleeping) {
        return;
    }
    s_sleeping = true;  // display_lock() and display_set_backlight() are refused from now on
    esp_err_t err;

    // Backlight off first, whatever state LVGL is in
    if (s_bl_ready) {
        err = ledc_stop(BL_MODE, BL_CHANNEL, BL_IDLE_LEVEL_OFF);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ledc_stop: %s", esp_err_to_name(err));
        }
    }

    // Park LVGL: the lock is never released, so no flush can follow SLPIN
    if (s_lvgl_ready) {
        if (!lvgl_port_lock(LVGL_SLEEP_LOCK_MS)) {
            ESP_LOGE(TAG, "LVGL lock timeout, shutting the panel down anyway");
        }
        err = lvgl_port_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "lvgl_port_stop: %s", esp_err_to_name(err));
        }
    }

    if (APP_LCD_PWR_USED) {
        // Always safe; power.c then holds the pin at this level
        panel_power(false);
        return;
    }
    if (s_panel == NULL) {
        return;
    }
    if (s_slpout_us != 0) {
        const int64_t since_ms = (esp_timer_get_time() - s_slpout_us) / 1000;
        if (since_ms < LCD_SLPOUT_TO_SLPIN_MS) {
            vTaskDelay(pdMS_TO_TICKS(LCD_SLPOUT_TO_SLPIN_MS - since_ms) + 1);
        }
    }
    // DISPOFF first waits for any pixel transfer still in flight
    err = esp_lcd_panel_disp_on_off(s_panel, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DISPOFF: %s", esp_err_to_name(err));
    }
    // SLPIN; the driver then waits 100 ms for the sleep-in sequence
    err = esp_lcd_panel_disp_sleep(s_panel, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SLPIN: %s", esp_err_to_name(err));
    }
}

// ---- Deep-sleep pins and driver-free parking (used by power.c) ----

#define LCD_RST_PULSE_MS        10

// Panel left powered in SLPIN: CS, DC and RES high, clock and data low, backlight
// off. With the power switch every line has the same level: low for a switch in
// VCC, so the unpowered panel is not back-fed; high for a switch in GND, so the
// panel finds no ground through its pins. Either is harmless for the backlight
// while the panel has no supply.
size_t display_sleep_pins(display_pin_level_t *out, size_t cap)
{
    const bool sw = APP_LCD_PWR_USED;
    const uint8_t off = APP_LCD_PWR_OFF_LINE_LEVEL;
    const display_pin_level_t pins[] = {
        { .pin = APP_PIN_LCD_PWR,  .level = 1 - APP_LCD_PWR_ON_LEVEL },     // only if used
        { .pin = APP_PIN_LCD_BL,   .level = sw ? off : APP_LCD_BL_OFF_LEVEL },
        { .pin = APP_PIN_LCD_CS,   .level = sw ? off : 1 },
        { .pin = APP_PIN_LCD_DC,   .level = sw ? off : 1 },
        { .pin = APP_PIN_LCD_RST,  .level = sw ? off : 1 },
        { .pin = APP_PIN_LCD_SCLK, .level = sw ? off : 0 },
        { .pin = APP_PIN_LCD_MOSI, .level = sw ? off : 0 },
    };
    size_t n = 0;
    for (size_t i = sw ? 0 : 1; i < sizeof(pins) / sizeof(pins[0]) && n < cap; i++) {
        out[n++] = pins[i];
    }
    return n;
}

static void park_drive(int pin, uint32_t level)
{
    gpio_set_level((gpio_num_t)pin, level);
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_FLOATING);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    gpio_hold_dis((gpio_num_t)pin);
}

// A RES pulse puts the ST7789 in its reset state (sleep-in, display off), with
// the backlight off and CS high first. With the power switch the panel is simply
// left unpowered by the sleep pin levels.
void display_park_panel(void)
{
    if (APP_LCD_PWR_USED) {
        return;
    }
    park_drive(APP_PIN_LCD_BL, APP_LCD_BL_OFF_LEVEL);
    park_drive(APP_PIN_LCD_CS, 1);
    park_drive(APP_PIN_LCD_RST, 0);
    esp_rom_delay_us(LCD_RST_PULSE_MS * 1000);
    park_drive(APP_PIN_LCD_RST, 1);
}
