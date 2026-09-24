idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
                       REQUIRES esp_wifi esp_netif esp_event esp_http_server nvs_flash esp_driver_spi led_strip)

# The web page is plain HTML in web/page.html; the build turns it into page.h, so
# editing the html and running `idf.py build` is all that is needed.
# Guarded because IDF also runs this file in script mode while collecting
# requirements, and add_custom_command does not exist there.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    idf_build_get_property(python PYTHON)
    set(page_src "${CMAKE_SOURCE_DIR}/web/page.html")
    set(page_hdr "${CMAKE_CURRENT_BINARY_DIR}/page.h")

    add_custom_command(OUTPUT "${page_hdr}"
                       COMMAND "${python}" "${CMAKE_SOURCE_DIR}/tools/build_page.py"
                               "${page_src}" "${page_hdr}"
                       DEPENDS "${page_src}" "${CMAKE_SOURCE_DIR}/tools/build_page.py"
                       COMMENT "Turning web/page.html into page.h"
                       VERBATIM)

    add_custom_target(page_header DEPENDS "${page_hdr}")
    add_dependencies(${COMPONENT_LIB} page_header)
endif()

# WIFI_SSID / WIFI_PASS are read from .env by the top-level CMakeLists.txt.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           WIFI_SSID="${WIFI_SSID}"
                           WIFI_PASS="${WIFI_PASS}")
