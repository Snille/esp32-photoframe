idf_component_register(
    SRCS "src/driver_ws4in_e6.c"
    INCLUDE_DIRS ""
    REQUIRES epaper driver esp_driver_gpio esp_driver_spi esp_timer freertos
)
