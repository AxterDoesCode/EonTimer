idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        nvs_flash
        driver
        esp_driver_gpio
        esp_driver_uart
        esp_timer
        cjson
)
