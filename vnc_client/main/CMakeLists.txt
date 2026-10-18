idf_component_register(
    SRCS "main.c" "vnc_rfb.c" "vnc_display.c" "vnc_input.c"
    INCLUDE_DIRS "."
    REQUIRES
        bsp_extra
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_driver_jpeg
        esp_driver_ppa
        esp_mm
        usb
        lwip
        app_update
        esp_partition
)
