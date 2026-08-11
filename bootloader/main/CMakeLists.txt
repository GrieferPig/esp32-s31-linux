set(srcs main.c core1_trampoline.S)
set(reqs
    freertos heap spi_flash esp_partition esp_psram esp_hw_support esp_rom
    esp_pm hal esp_mm esp_hal_wdt esp_driver_uart)

idf_component_register(SRCS ${srcs}
                       REQUIRES ${reqs}
                       INCLUDE_DIRS "." "../../shared" "${LINUX_MMAP_HEADER_DIR}")
