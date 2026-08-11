idf_component_register(
    SRCS "main.c" "dashboard.c" "rotary_encoder.c" "ftdi_bridge.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_timer driver esp_lcd
)
