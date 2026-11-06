idf_component_register(
    SRCS "main.c" "sd_card.c" "bridge.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_usb_serial_jtag
        esp_driver_sdspi
        esp_driver_spi
        sdmmc
        fatfs
        esp_hw_support
        esp_timer
)
