idf_component_register(
    SRCS "main.c" "timers_config.c" "web_server.c" "wifi_init.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_event esp_wifi esp_http_server esp_driver_gpio
)
