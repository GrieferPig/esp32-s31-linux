# Source files common to all configurations
set(srcs main.c hosted_sram.c core1_trampoline.S)

if(CONFIG_S31_AUDIO_ENABLE)
    list(APPEND srcs s31_audio.c s31_audio_hw.c s31_audio_asrc.c)
endif()

# Optional Wi-Fi component
if(CONFIG_S31_HOSTED_WIFI_ENABLE)
    list(APPEND srcs hosted_wifi.c)
endif()

idf_component_register(SRCS ${srcs}
                       REQUIRES freertos heap spi_flash esp_partition esp_psram esp_hw_support esp_rom esp_pm hal esp_mm esp_hal_wdt esp_timer vfs esp_hosted_slave nvs_flash esp_driver_uart esp_driver_i2s esp_driver_i2c esp_asrc_adapter esp_hal_asrc esp_codec_dev
                       INCLUDE_DIRS "." "../../shared" "${LINUX_MMAP_HEADER_DIR}")
