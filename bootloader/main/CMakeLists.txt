idf_component_register(SRCS main.c hosted_sram.c core1_trampoline.S
                       REQUIRES freertos heap spi_flash esp_partition esp_psram esp_hw_support esp_rom hal esp_mm esp_hal_wdt vfs
                       INCLUDE_DIRS "." "../../shared" "${LINUX_MMAP_HEADER_DIR}")
