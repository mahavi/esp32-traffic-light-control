idf_component_register(SRCS "main.c" "traffic_light.c" "button.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_gpio esp_driver_gptimer esp_timer)
