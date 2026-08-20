idf_component_register(
    SRCS
        "src/EDP_OTP_LUT_demo.cpp"
        "src/EDP_SPI.cpp"
    INCLUDE_DIRS "include"
    REQUIRES m5unified esp_driver_gpio esp_driver_spi esp_timer
)
