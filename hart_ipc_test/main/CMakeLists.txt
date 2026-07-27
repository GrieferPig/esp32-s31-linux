idf_component_register(SRCS main.c core1_trampoline.S
                       REQUIRES freertos esp_hw_support esp_rom hal esp_psram spi_flash esp_partition esp_mm soc
                       INCLUDE_DIRS "." "../../shared")
