idf_component_register(
    SRCS "idf_deps.c"
    REQUIRES bt esp_wifi esp_phy esp_coex esp_event esp_netif nvs_flash
             wpa_supplicant mbedtls
    INCLUDE_DIRS ".")
