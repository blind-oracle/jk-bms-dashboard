# Sources are listed explicitly: PlatformIO re-runs CMake when this file
# changes, but not when files are added to src/.
set(srcs
    "main.c"
    "app_events.c"
    "jk_proto.c"
    "jk_bms.c"
    "button.c"
    "battery.c"
    "power.c"
    "ui_common.c"
)
# One display backend and its UI per build (BMSD_DISPLAY in menuconfig)
if(CONFIG_BMSD_DISPLAY_SSD1309)
    list(APPEND srcs
        "display_ssd1309.c"
        "ui_oled.c"
        "ui_oled_pages.c"
    )
else()
    list(APPEND srcs
        "display_st7789.c"
        "ui.c"
        "ui_main.c"
        "ui_cells.c"
        "ui_alarms.c"
        "ui_details.c"
    )
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    # PlatformIO registers src/ as a component named "src" (not "main"), so it
    # does not implicitly depend on every component. lvgl and esp_lvgl_port
    # are added from idf_component.yml.
    PRIV_REQUIRES
        esp_lcd
        esp_driver_spi
        esp_driver_i2c
        esp_driver_gpio
        esp_driver_uart
        esp_driver_ledc
        esp_driver_usb_serial_jtag
        esp_adc
        esp_timer
)
