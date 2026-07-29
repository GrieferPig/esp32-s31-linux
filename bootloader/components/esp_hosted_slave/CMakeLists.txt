set(hosted_root "${CMAKE_CURRENT_LIST_DIR}/../../../esp-hosted-fg")
get_filename_component(hosted_root "${hosted_root}" ABSOLUTE)
set(slave_dir "${hosted_root}/slave/main")
set(common_dir "${hosted_root}/common")

set(srcs
    "${common_dir}/proto/esp_hosted_rpc.pb-c.c"
    "${slave_dir}/protocomm_pserial.c"
    "${slave_dir}/esp_hosted_coprocessor.c"
    "${slave_dir}/stats.c"
    "${slave_dir}/host_power_save.c"
    "${slave_dir}/slave_light_sleep.c"
    "${slave_dir}/nw_split_router.c"
    "${slave_dir}/slave_transport_gpio_pin_guard.c"
    "${slave_dir}/slave_gpio_expander.c"
    "${slave_dir}/slave_ext_coex.c"
    "${slave_dir}/slave_control.c"
    "${slave_dir}/slave_util.c"
    "${slave_dir}/slave_bt.c"
    "${slave_dir}/slave_wifi_std.c"
    "sram_slave_api.c")

if(CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT)
    list(APPEND srcs "${slave_dir}/slave_wifi_enterprise.c")
endif()

if(CONFIG_ESP_HOSTED_USE_MEMPOOL)
    list(APPEND srcs
        "${common_dir}/mempool/mempool_ll.c"
        "${common_dir}/mempool/mempool.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "${slave_dir}"
        "${common_dir}"
        "${common_dir}/log"
        "${common_dir}/proto"
        "${common_dir}/rpc"
        "${common_dir}/transport"
        "${common_dir}/mempool/include"
        "${common_dir}/utils"
        "../../main"
        "../../../shared"
    PRIV_REQUIRES
        esp_wifi bt esp_event esp_netif nvs_flash protocomm freertos heap
        esp_timer esp_driver_gpio esp_driver_uart app_update bootloader_support
    REQUIRES esp_common esp_app_format protobuf-c wpa_supplicant)

idf_component_get_property(protocomm_dir protocomm COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${protocomm_dir}/src/common")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_wifi_init")
