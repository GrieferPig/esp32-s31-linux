set(srcs main.c core1_trampoline.S)
set(reqs
    freertos heap spi_flash esp_partition esp_psram esp_hw_support esp_rom
    esp_pm hal esp_mm esp_hal_wdt esp_driver_uart)

# A separate, build-only configuration supplies the closed IDF archives and
# generated sdkconfig used by radio_firmware.  Keep these components out of
# the factory loader itself: their system-init hooks would claim RF on hart0.
if(DEFINED ENV{S31_RADIO_DEPS})
    list(APPEND srcs native_wifi_probe.c)
    list(APPEND reqs
         bt esp_wifi esp_phy esp_coex esp_event esp_netif nvs_flash
         wpa_supplicant mbedtls)
endif()

idf_component_register(SRCS ${srcs}
                       REQUIRES ${reqs}
                       INCLUDE_DIRS "." "../../shared" "${LINUX_MMAP_HEADER_DIR}")

if(DEFINED ENV{S31_RADIO_DEPS})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE S31_NATIVE_WIFI_PROBE=1)
endif()
