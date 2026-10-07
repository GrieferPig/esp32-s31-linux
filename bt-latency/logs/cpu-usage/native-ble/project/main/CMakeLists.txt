idf_component_register(SRCS "cpu_bench.c" "example_ble_server_throughput.c"
                    PRIV_REQUIRES bt nvs_flash esp_timer esp_system
                    INCLUDE_DIRS ".")
