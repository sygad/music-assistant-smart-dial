idf_component_register(
    SRCS "app_main.c" "net_check.c" "screenshot.c" "library.c" "ma.c" "ha.c" "amp.c" "ota.c" "playback.c" "voice.c" "crash.c" "memwatch.c"
    INCLUDE_DIRS "."
    REQUIRES ui board core json nvs_flash esp_wifi esp_netif esp_event esp_http_client mbedtls esp_driver_usb_serial_jtag esp_driver_i2s esp_app_format app_update esp_timer)

# The crash recorder (crash.c) runs ahead of IDF's own panic handler.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

# The cover decoder's working memory goes through memwatch.c, which counts it
# and decides where it lives (docs/MEMORY.md).
foreach(fn jpeg_calloc_inner jpeg_calloc_align_inner jpeg_free jpeg_free_align)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${fn}")
endforeach()

# Wi-Fi details and the Music Assistant token come from config/secrets.local
# (git-ignored) into a generated header in the build folder. The Home Assistant
# token speaks to Home Assistant: speech-to-text for voice search, and the amp
# power (31 Oct 2026).
set(SECRETS_FILE ${CMAKE_CURRENT_LIST_DIR}/../../config/secrets.local)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SECRETS_FILE})
file(STRINGS ${SECRETS_FILE} SECRET_LINES)
set(WIFI_SSID "")
set(WIFI_PASSWORD "")
set(MA_TOKEN "")
set(HA_TOKEN "")
set(HA_SERVER "")
set(MA_SERVER "")
set(AMP_SERVER "")
set(MA_PLAYER "")
set(DIAL_ROOM "")
foreach(line IN LISTS SECRET_LINES)
    if(line MATCHES "^WIFI_SSID=(.*)$")
        set(WIFI_SSID "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^WIFI_PASSWORD=(.*)$")
        set(WIFI_PASSWORD "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^MA_TOKEN=(.*)$")
        set(MA_TOKEN "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^HA_TOKEN=(.*)$")
        set(HA_TOKEN "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^HA_SERVER=(.*)$")
        set(HA_SERVER "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^MA_SERVER=(.*)$")
        set(MA_SERVER "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^AMP_SERVER=(.*)$")
        set(AMP_SERVER "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^MA_PLAYER=(.*)$")
        set(MA_PLAYER "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^DIAL_ROOM=(.*)$")
        set(DIAL_ROOM "${CMAKE_MATCH_1}")
    endif()
endforeach()
# The addresses and the player are this house's, so they live beside the
# tokens rather than in the C: anyone else building it fills in their own.
# AMP_SERVER may be blank: no amp, and amp.c takes it as always there.
foreach(v HA_SERVER MA_SERVER MA_PLAYER DIAL_ROOM)
    if("${${v}}" STREQUAL "")
        message(FATAL_ERROR "config/secrets.local has no ${v}= (see config/secrets.example)")
    endif()
endforeach()
# DIAL_ROOM becomes part of every entity id, so it must be one: lower case,
# digits and underscores.
if(NOT DIAL_ROOM MATCHES "^[a-z0-9_]+$")
    message(FATAL_ERROR "DIAL_ROOM=${DIAL_ROOM} in config/secrets.local must be lower case letters, digits and _ only")
endif()
string(REGEX REPLACE "/+$" "" HA_SERVER "${HA_SERVER}")
string(REGEX REPLACE "/+$" "" MA_SERVER "${MA_SERVER}")
string(REGEX REPLACE "/+$" "" AMP_SERVER "${AMP_SERVER}")
foreach(v WIFI_SSID WIFI_PASSWORD MA_TOKEN HA_TOKEN HA_SERVER MA_SERVER AMP_SERVER MA_PLAYER)
    string(REPLACE "\\" "\\\\" ${v} "${${v}}")
    string(REPLACE "\"" "\\\"" ${v} "${${v}}")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/secrets_gen.h
    "/* Generated from config/secrets.local. Never commit. */\n"
    "#pragma once\n"
    "#define SECRET_WIFI_SSID \"${WIFI_SSID}\"\n"
    "#define SECRET_WIFI_PASSWORD \"${WIFI_PASSWORD}\"\n"
    "#define SECRET_MA_TOKEN \"${MA_TOKEN}\"\n"
    "#define SECRET_HA_TOKEN \"${HA_TOKEN}\"\n"
    "#define SECRET_HA_SERVER \"${HA_SERVER}\"\n"
    "#define SECRET_MA_SERVER \"${MA_SERVER}\"\n"
    "#define SECRET_AMP_SERVER \"${AMP_SERVER}\"\n"
    "#define SECRET_MA_PLAYER \"${MA_PLAYER}\"\n"
    "#define SECRET_DIAL_ROOM \"${DIAL_ROOM}\"\n")
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
