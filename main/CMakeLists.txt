idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_event esp_http_server nvs_flash esp_driver_spi led_strip)

# WIFI_SSID / WIFI_PASS are read from .env by the top-level CMakeLists.txt.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           WIFI_SSID="${WIFI_SSID}"
                           WIFI_PASS="${WIFI_PASS}")
