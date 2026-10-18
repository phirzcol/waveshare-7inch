---
inclusion: manual
---

# WiFi — ESP-Hosted SDIO

## Architecture
- ESP32-P4 (host) ↔ ESP32-C6 (slave) via SDIO bus
- Driver: `espressif/esp_hosted` 1.4.x + `espressif/esp_wifi_remote` 0.14.x
- WiFi API is standard `esp_wifi_*` — ESP-Hosted is transparent

## Critical Config
```
CONFIG_ESP_HOSTED_SLAVE_TARGET_ESP32C6=y
CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=10
CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=10
```
- Queue sizes MUST be 10 (20 causes hard resets under VNC streaming load)
- `$env:ESP_IDF_VERSION = "5.5"` required — esp_wifi_remote uses it in Kconfig orsource

## Init Rules
- WiFi init happens ON-DEMAND (user taps Connect), NEVER at boot
- 2-second delay before first connect attempt (SDIO transport settling)
- Auto-retry: if first attempt fails, wait 2s and try again
- All WiFi operations have 10-15 second timeouts
- WiFi tasks run on their own FreeRTOS task, never on LVGL or audio cores

## SDIO Bus Conflicts
- ESP-Hosted SDIO polling competes with SD card SDMMC DMA
- When WiFi is active, audio ring buffer reader can stall
- Solution: ring buffer absorbs stalls (725ms headroom at 128KB)
- SDIO queue sizes reduced to 10 to limit bus contention

## Credentials
- Stored in NVS namespace "wifi", keys "ssid" and "pass"
- Both main app and VNC client read/write same namespace
- Web settings page can also update credentials

## Web Server
- Starts when WiFi connects: `web_server_start()`
- Stops on disconnect: `web_server_stop()`
- Serves settings page + file manager at device IP
- Default auth: admin / esp32tablet
- File transfer API: `/api/files`, `/api/files/download`, `/api/files/upload`
