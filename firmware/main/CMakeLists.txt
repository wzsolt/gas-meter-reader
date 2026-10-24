idf_component_register(SRCS "app_main.c" "core.c" "runtime.c" "network.c" "ota_server.c" "image_check.c" "wifi_join.c" "maintenance_control.c" "sync_drain.c" "meter_reset.c" "daily_schedule.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "maintenance.html"
    REQUIRES esp_driver_gpio esp_adc esp_timer esp_wifi esp_event esp_netif
             nvs_flash app_update esp_http_server esp_partition cjson mqtt mdns mbedtls)
