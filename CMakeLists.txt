cmake_minimum_required(VERSION 3.16)

# ---- Wi-Fi credentials come from .env, which is not committed ---------------
# Copy .env.example to .env and edit it. Anything missing falls back to the
# defaults below, so a fresh clone still builds.
set(WIFI_SSID "Visakha Lamp")
set(WIFI_PASS "12345678")
set(ENV_FILE "${CMAKE_CURRENT_LIST_DIR}/.env")

if(EXISTS "${ENV_FILE}")
    # re-run cmake (and rebuild with the new credentials) whenever .env changes
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ENV_FILE}")
    file(STRINGS "${ENV_FILE}" env_lines)
    foreach(env_line IN LISTS env_lines)
        if(NOT env_line MATCHES "^[ \t]*#"
           AND env_line MATCHES "^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*(.*)$")
            set(env_key "${CMAKE_MATCH_1}")
            set(env_val "${CMAKE_MATCH_2}")
            string(STRIP "${env_val}" env_val)
            string(REGEX REPLACE "^\"(.*)\"$" "\\1" env_val "${env_val}")
            string(REGEX REPLACE "^'(.*)'$" "\\1" env_val "${env_val}")
            if(env_key STREQUAL "AP_SSID")
                set(WIFI_SSID "${env_val}")
            elseif(env_key STREQUAL "AP_PASS")
                set(WIFI_PASS "${env_val}")
            endif()
        endif()
    endforeach()
    message(STATUS "Wi-Fi credentials read from ${ENV_FILE} (SSID '${WIFI_SSID}')")
else()
    message(WARNING "No .env file: using the default Wi-Fi credentials. "
                    "Copy .env.example to .env and edit it to set your own.")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(visakha-lamp)
