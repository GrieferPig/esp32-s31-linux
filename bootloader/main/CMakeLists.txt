idf_component_register(SRCS main.c
                       REQUIRES freertos fatfs esp_adc esp_driver_i2s esp_driver_sdmmc sdmmc spi_flash esp_partition esp_psram esp_hw_support esp_rom hal spi_flash esp_mm esp_hal_wdt
                       INCLUDE_DIRS "." "${LINUX_MMAP_HEADER_DIR}")
