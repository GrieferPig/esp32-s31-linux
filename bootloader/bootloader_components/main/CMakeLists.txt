idf_build_get_property(idf_path IDF_PATH)

set(psram_device_dir "${idf_path}/components/esp_psram/device")

idf_component_register(SRCS "bootloader_start.c"
                            "bootloader_psram_compat.c"
                            "${psram_device_dir}/esp_psram_impl_ap_oct.c"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "${psram_device_dir}/include"
                       REQUIRES bootloader bootloader_support esp_partition esp_hal_wdt
                                esp_hw_support esp_hal_clock esp_hal_mspi esp_hal_pmu
                                hal)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_SPIRAM=1
    CONFIG_SPIRAM_MODE_OCT=1
    CONFIG_SPIRAM_SPEED_250M=1
    CONFIG_SPIRAM_SPEED=250
    CONFIG_PM_SLP_SPIRAM_HALFSLEEP_ENABLED=0
    CONFIG_PM_SLP_SPIRAM_HALFSLEEP_EXIT_WAIT_DELAY=0)

idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")

target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_hooks_include")
