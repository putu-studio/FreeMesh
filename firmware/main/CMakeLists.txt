# The .c files of the "main" component. The optional gateway add-on is added below when present.
set(srcs "main.c" "app_radio.c" "app_wifi.c" "app_chat.c" "app_settings.c" "app_web.c" "app_io.c" "app_mode.c" "app_crypto.c" "app_friends.c" "app_groups.c" "app_voice.c" "app_tls.c" "app_sos.c" "app_gw.c" "app_people.c" "app_ble.c" "app_ota.c" "app_batt.c")
set(embed "webapp/index.html" "webapp/codec2.wasm.gz")     # files stored inside the program as they are

# The optional gateway add-on (see app_gw.h) is not in this repository. It is built when its source file is in this folder.
# "idf.py -DFREEMESH_BRIDGE=0 build" leaves it out even when the file is there; "=1" puts it back.
set(bridge OFF)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/app_bridge.c" AND NOT "${FREEMESH_BRIDGE}" STREQUAL "0")
    set(bridge ON)
    list(APPEND srcs "app_bridge.c")
    list(APPEND embed "webapp/bridge.js")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."                           # look for .h files in this folder
                       REQUIRES esp_driver_spi esp_driver_gpio esp_wifi esp_netif esp_event nvs_flash esp_http_server app_update esp_partition mbedtls esp_https_server esp_http_client codec2 bt dns_server bootloader_support esp_app_format esp_timer esp_adc   # ESP-IDF components this code uses.
                       EMBED_FILES ${embed})
if(bridge)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FREEMESH_BRIDGE=1)
endif()

# Test builds only: "idf.py -DFREEMESH_TEST_CRASH=1 build" makes a FreeMesh that stops on purpose at start, to test the update fallback.
# "idf.py -DFREEMESH_TEST_CRASH=0 build" returns to the normal build.
if(FREEMESH_TEST_CRASH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FREEMESH_TEST_CRASH=1)
endif()

# Test builds only: "idf.py -DFREEMESH_TEST_BATT=1 build" simulates a battery with the chip's own resistors, to test the battery level without extra parts.
if(FREEMESH_TEST_BATT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FREEMESH_TEST_BATT=1)
endif()
