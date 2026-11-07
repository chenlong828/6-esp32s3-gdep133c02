idf_component_register(
    SRCS "main.c" "sd_card.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_sdspi
        esp_driver_spi
        sdmmc
        fatfs
        esp_rom
        esp_timer
        esp_hw_support
        esp_psram
)
