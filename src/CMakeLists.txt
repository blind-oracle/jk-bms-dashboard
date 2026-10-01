# Sources are listed explicitly: PlatformIO re-runs CMake when this file
# changes, but not when files are added to src/.
idf_component_register(
    SRCS
        "main.c"
        "app_events.c"
        "jk_proto.c"
        "jk_bms.c"
        "button.c"
        "battery.c"
        "display.c"
        "power.c"
        "ui.c"
        "ui_main.c"
        "ui_cells.c"
        "ui_alarms.c"
        "ui_details.c"
    INCLUDE_DIRS "."
    # PlatformIO registers src/ as a component named "src" (not "main"), so it
    # does not implicitly depend on every component. lvgl and esp_lvgl_port
    # are added from idf_component.yml.
    PRIV_REQUIRES
        esp_lcd
        esp_driver_spi
        esp_driver_gpio
        esp_driver_uart
        esp_driver_ledc
        esp_driver_usb_serial_jtag
        esp_adc
        esp_timer
)
