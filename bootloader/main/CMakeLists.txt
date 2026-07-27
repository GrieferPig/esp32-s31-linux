# Source files common to all configurations
set(srcs main.c hosted_sram.c core1_trampoline.S)

# Optional Wi-Fi component
if(CONFIG_S31_HOSTED_WIFI_ENABLE)
    list(APPEND srcs hosted_wifi.c)
endif()

idf_component_register(SRCS ${srcs}
                       REQUIRES freertos heap spi_flash esp_partition esp_psram esp_hw_support esp_rom hal esp_mm esp_hal_wdt vfs
                       INCLUDE_DIRS "." "../../shared" "${LINUX_MMAP_HEADER_DIR}")
