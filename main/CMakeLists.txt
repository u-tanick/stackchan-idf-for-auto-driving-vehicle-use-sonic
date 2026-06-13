set(_main_priv_reqs
    board
    scs_servo
    avatar
    conversation
    config_service
    wifi_config_service
    jtts
    telegram
    espressif__esp_audio_codec
    json
    M5Unified
    M5GFX
    esp_timer
    esp_psram
    driver
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    app_update
    lwip
)

# quirc (QR decoder) is only needed when the camera driver is compiled in.
# Same gating as components/board/CMakeLists.txt — keeps atoms3r builds free
# of the managed component download.
if(CONFIG_STACKCHAN_CAMERA_ENABLED)
    list(APPEND _main_priv_reqs espressif__quirc)
endif()

idf_component_register(
    SRCS
        "app_main.cpp"
        "atom_status.cpp"
        "audio_codec.cpp"
        "battery.cpp"
        "audio_stream_sink.cpp"
        "conversation_task.cpp"
        "device_ui.cpp"
        "diag.cpp"
        "face_config.cpp"
        "i2c_dump.cpp"
        "led_task.cpp"
        "lt_timer.cpp"
        "qr_task.cpp"
        "render_task.cpp"
        "servo_limits.cpp"
        "servo_task.cpp"
        "speech.cpp"
        "wifi_audio.cpp"
        "wifi_sta.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${_main_priv_reqs}
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
