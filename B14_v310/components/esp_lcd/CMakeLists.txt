# Project-local ESP-IDF esp_lcd override for Nissan B14 SPI ILI9488.
#
# ESP32-S3 advertises RGB LCD capability, so the stock ESP-IDF 5.4.1
# esp_lcd component automatically compiles rgb/esp_lcd_panel_rgb.c even when
# the application uses only an SPI panel. On some Windows/GCC 14.2 builds that
# file triggers a known compiler ICE (during RTL pass: ira / Segmentation fault).
#
# This project uses ILI9488 over SPI only. Keep the standard ESP-IDF SPI LCD
# core, panel IO and panel operation implementation, and intentionally omit the
# unused RGB panel source. No framework package is modified globally.

set(IDF_LCD_DIR "$ENV{IDF_PATH}/components/esp_lcd")

set(srcs
    "${IDF_LCD_DIR}/src/esp_lcd_common.c"
    "${IDF_LCD_DIR}/src/esp_lcd_panel_io.c"
    "${IDF_LCD_DIR}/src/esp_lcd_panel_ops.c"
    "${IDF_LCD_DIR}/spi/esp_lcd_panel_io_spi.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "${IDF_LCD_DIR}/include"
        "${IDF_LCD_DIR}/interface"
    PRIV_INCLUDE_DIRS
        "${IDF_LCD_DIR}/priv_include"
    PRIV_REQUIRES
        esp_mm
        esp_psram
        esp_pm
        esp_driver_i2s
    REQUIRES
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
)

message(STATUS "Nissan B14: using project esp_lcd SPI-only override (RGB source excluded)")
