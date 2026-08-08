idf_component_register(SRCS "uart_bench.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_uart esp_timer)
