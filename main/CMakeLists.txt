idf_component_register(
    SRCS
        "app_main.c"
        "app_state.c"
        "event_bus.c"
        "audio_in.c"
        "wake_word.c"
        "audio_out.c"
        "ws_service.c"


        "opus_codec.c"

        "proto_service.c"
        "led_service.c"
        "tca9554.c"
        "wifi_service.c"
        "cfg_service.c"
        "log_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_codec_dev
        esp-sr
        opus_codec
        esp_websocket_client
        esp_wifi
        nvs_flash
        esp_timer
        esp_system
        esp_netif
        esp_http_client
        esp_https_ota
        driver
        freertos
        log
        esp_hw_support
        spi_flash
        esp_psram
        mbedtls
)

# Suppress warnings from IDF headers in third-party code
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)
