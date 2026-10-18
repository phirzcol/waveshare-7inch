idf_component_register(
    SRCS "main.c" "web_server.c" "settings_manager.c" "web_api_handlers.c" "web_file_handlers.c" "onscreen_keyboard.c" "text_editor.c" "photo_viewer.c" "video_player.c" "usb_device_monitor.c" "game_input.c" "raw_sd_test.c"
    INCLUDE_DIRS "."
    EMBED_FILES "web_page.html" "raw_crab_seed.bin"
    REQUIRES
        bsp_extra
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        fatfs
        esp_driver_i2s
        esp_driver_jpeg
        esp_driver_ppa
        esp_mm
        usb
        esp_http_server
        json
        mbedtls
        app_update
        esp_partition
)
