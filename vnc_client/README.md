# ESP32-P4 VNC Client

A VNC (Remote Framebuffer) client for the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B tablet. Connects to VNC servers over WiFi and displays the remote desktop on the 7" 1024×600 MIPI-DSI screen.

## Hardware

- **Board**: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
- **MCU**: ESP32-P4 (dual-core RISC-V 360MHz)
- **WiFi**: ESP32-C6-MINI-1 via SDIO (WiFi 6)
- **Display**: 7" IPS 1024×600, MIPI-DSI, EK79007 controller
- **Touch**: GT911 capacitive
- **RAM**: 32MB PSRAM, 32MB Flash
- **Input**: USB HID (iPazzport wireless keyboard/mouse combo)

## Features

- **Tight encoding** with hardware JPEG decode (ESP32-P4 JPEG accelerator)
- **ZRLE encoding** support (text-heavy UIs, TigerVNC/droidVNC-NG)
- **Raw encoding** (debug/baseline mode)
- **Settings UI** with WiFi SSID/password, VNC server IP/port, encoding selector
- **JPEG quality and compression sliders** (0-9 quality, 1-9 compression)
- **iPazzport USB HID** mouse + keyboard forwarding to VNC server
- **On-screen keyboard** for text input
- **PPA hardware fill** for solid-color rectangles
- **Single framebuffer** with DMA-aligned allocation

## Build

Requires ESP-IDF v5.5.4.

```powershell
$env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.5.4"
$env:ESP_IDF_VERSION = "5.5"
idf.py build
idf.py -p COM23 flash
```

**Important**: Set `ESP_IDF_VERSION=5.5` before building (fixes esp_wifi_remote Kconfig bug).

## Post-Build: Apply iPazzport Patch

After first build (which fetches managed_components), copy the patched HID file:

```
copy patches\esp_lvgl_port_usbhid.c managed_components\espressif__esp_lvgl_port\src\lvgl9\
```

Then rebuild. See `patches/README.md` for details.

## Compatible VNC Servers

- TightVNC (Windows) — Tight encoding with JPEG
- TigerVNC — ZRLE or Tight
- droidVNC-NG (Android) — ZRLE
- Any server supporting RGB565 pixel format

## Settings

All settings are stored in NVS and configured via the on-device settings screen:
- WiFi SSID & Password
- VNC Server IP & Port
- VNC Password (optional, DES auth)
- Encoding: Tight / ZRLE / Raw
- JPEG Quality (0-9)
- Compression Level (1-9)

## Architecture

```
[VNC Server] → WiFi 6 (ESP32-C6 SDIO)
    ↓
[ESP32-P4 VNC Client] → Decode (HW JPEG / ZRLE / Raw)
    ↓
[PSRAM Framebuffer 1024×600 RGB565]
    ↓ MIPI-DSI DMA
[7" IPS Display]
```

## Known Issues

- ZRLE encoding has a subtle blue tint on some static solid-color areas (Palette RLE decoder path). Tight and Raw modes are unaffected.
- Device does not auto-reset after flash — press RESET manually.

## License

MIT
