idf_component_register(SRCS "main.c"
                    PRIV_REQUIRES esp_wifi nvs_flash fatfs spi_flash sdmmc driver
                    INCLUDE_DIRS ".")
