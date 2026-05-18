# ESP32-P4 Tablet — Dual-Boot Firmware

A dual-boot firmware for the **Waveshare ESP32-P4-WIFI6-Touch-LCD-7B** development board featuring a full tablet UI with music player, file manager, text editor, photo viewer, and a standalone VNC remote desktop client.

## Hardware

- **Board**: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
- **SoC**: ESP32-P4 (dual-core RISC-V 360MHz) + ESP32-C6 (WiFi 6 via SDIO)
- **Display**: 7" IPS 1024×600, MIPI-DSI, EK79007 controller
- **Touch**: GT911 capacitive (I2C)
- **Audio**: ES8311 DAC + ES7210 ADC, I2S, PA on GPIO53
- **Storage**: 32MB Flash, 32MB PSRAM, SD card (SDMMC 4-bit)
- **USB**: Type-A OTG 2.0 HS port (supports HID keyboard/mouse)

## Build Environment

| Component | Version |
|-----------|---------|
| ESP-IDF | v5.5.4 |
| LVGL | 9.2.2 (via managed component) |
| esp_lvgl_port | 2.7.2 |
| esp_hosted | 1.4.7 |
| BSP | waveshare/esp32_p4_wifi6_touch_lcd_7b 1.0.2 |

### Required Environment Variables (PowerShell)

```powershell
$env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.5.4"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\python_env\idf5.5_py3.11_env"
$env:IDF_TOOLS_PATH = "C:\Espressif"
$env:ESP_IDF_VERSION = "5.5"
$env:PATH = "C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;" + $env:PATH
```

> **Critical**: `$env:ESP_IDF_VERSION = "5.5"` must be set or esp_wifi_remote Kconfig will fail.

## Project Structure

```
├── test_wifi_audio/          ← Main tablet app (factory partition)
│   ├── main/
│   │   ├── main.c           ← All UI + app logic
│   │   ├── onscreen_keyboard.c/h
│   │   ├── text_editor.c/h
│   │   ├── photo_viewer.c/h
│   │   ├── settings_manager.c/h
│   │   ├── web_server.c/h
│   │   ├── web_api_handlers.c
│   │   └── web_page.html
│   ├── sdkconfig.defaults
│   ├── partitions.csv
│   └── CMakeLists.txt
├── vnc_client/               ← VNC remote desktop client (ota_0 partition)
│   ├── main/
│   │   ├── main.c           ← Settings page + VNC connection
│   │   ├── vnc_rfb.c/h      ← RFB protocol (Tight encoding, JPEG)
│   │   ├── vnc_display.c/h  ← Framebuffer + HW JPEG decode
│   │   └── vnc_input.c/h    ← Mouse/keyboard → VNC events
│   ├── sdkconfig.defaults
│   ├── partitions.csv
│   └── CMakeLists.txt
├── components/
│   ├── esp-audio-player/     ← MP3/WAV decoder with ring buffer
│   └── bsp_extra/            ← Board support (codec, audio init)
└── 4.0inch/serial_monitor/   ← Optional: CYD serial monitor sketch
```

## Partition Layout (32MB Flash)

| Name | Type | Offset | Size |
|------|------|--------|------|
| nvs | data | 0x9000 | 16KB |
| otadata | data | 0xd000 | 8KB |
| phy_init | data | 0xf000 | 4KB |
| factory | app | 0x10000 | 4MB |
| ota_0 | app | 0x410000 | 4MB |
| storage | data/fat | 0x810000 | 10MB |

## Build & Flash Instructions

### 1. Build Main App

```powershell
cd test_wifi_audio
idf.py build
```

### 2. Build VNC Client

```powershell
cd vnc_client
idf.py build
```

### 3. Flash Both (device in boot mode)

Flash order matters. The main app flash includes bootloader, partition table, and otadata.

```powershell
# Step 1: Flash main app to FACTORY partition (0x10000)
# This also writes bootloader, partition table, and otadata
cd test_wifi_audio
idf.py -p COM23 flash

# Step 2: Flash VNC client to OTA_0 partition (0x410000)
# IMPORTANT: Do NOT use "idf.py flash" from vnc_client/ — that would overwrite factory!
# Use esptool directly with the correct offset:
python -m esptool --chip esp32p4 -p COM23 -b 460800 --before=default_reset --after=hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB 0x410000 E:\path\to\vnc_client\build\vnc_client.bin
```

> **WARNING**: Running `idf.py flash` from the `vnc_client/` directory will flash to factory (0x10000) and overwrite the main app. Always use esptool with explicit offset `0x410000` for the VNC client.

### 4. Boot Mode

Hold **BOOT** button, press **RESET**, release **BOOT**. Flash, then press **RESET** to run.

## SD Card Setup

Create these folders on a FAT32 formatted SD card:

```
/sdcard/
├── h/              ← Background images (1024x600 JPEG)
├── photos/         ← Photos for photo viewer (JPEG)
├── text/           ← Text files for editor
└── music/          ← MP3/WAV files (or any folder with audio)
```

Place at least one 1024×600 JPEG in `/sdcard/h/` for the desktop background.

## Configuration

### WiFi
Edit `test_wifi_audio/main/main.c`:
```c
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASS     "YOUR_WIFI_PASSWORD"
```

Or configure via the built-in web settings server after connecting.

### Web Settings Server
Once WiFi is connected, the device serves a settings page at its IP address.
- **Default AP SSID**: `ESP32-P4-Tablet`
- **Default AP Password**: `steampunk`
- **Default Admin Password**: `esp32tablet`

### VNC Client
All VNC settings (server IP, port, JPEG quality, compression) are configured on-screen and saved to NVS. No code changes needed.

## Dual-Boot Usage

- **Main App → VNC**: Tap the "VNC" icon on the desktop
- **VNC → Main App**: Tap "Main App" button on VNC settings screen
- Both apps share NVS for WiFi credentials

## Features

### Main App
- Swipeable desktop with app icons
- Music player (MP3/WAV, ring buffer pipeline, auto-advance)
- File manager (browse SD card, tap MP3 to play, tap .txt to edit)
- Text editor with on-screen keyboard
- Photo viewer (hardware JPEG decode)
- WiFi management
- USB HID keyboard/mouse support (iPazzport combo)
- Web settings server

### VNC Client
- Connects to TightVNC/TurboVNC server
- Tight encoding with hardware JPEG decode
- Touch → VNC pointer events
- USB keyboard → VNC key events
- On-screen keyboard for VNC input
- Configurable JPEG quality and compression level
- Battery status indicator (brownout detector)

## Known Limitations

- VNC streaming + audio playback cannot run simultaneously (SDIO bus conflict)
- Battery monitoring is threshold-based only (Full/Low/Critical), not percentage
- ESP-Hosted SDIO queue sizes reduced to 10 for stability under heavy WiFi load
- Hardware JPEG decoder rejects images with dimensions not divisible by 8
