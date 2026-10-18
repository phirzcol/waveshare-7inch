# phirzWaveshare7inchp4 — ESP32-P4 Tablet Triple-Boot Firmware

Triple-boot firmware for the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B tablet. Three independent apps share a single 32MB flash chip and swap between each other via OTA partition switching.

## Hardware

- **Board**: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
- **MCU**: ESP32-P4 (dual-core RISC-V 360MHz)
- **WiFi**: ESP32-C6-MINI-1 via SDIO (WiFi 6)
- **Display**: 7" IPS 1024×600, MIPI-DSI, EK79007 controller
- **Touch**: GT911 capacitive
- **RAM**: 32MB PSRAM, 32MB Flash
- **Audio**: ES8311 DAC + ES7210 ADC, I2S
- **Storage**: SD card (SDMMC 4-bit)
- **Input**: USB HID (iPazzport wireless keyboard/mouse combo)
- **USB**: Type-A (host, for HID) + Type-C (device, for USB display mode)

## Apps

| Directory | Partition | Offset | Size | Description |
|-----------|-----------|--------|------|-------------|
| `main_app/` | factory | 0x10000 | 3MB | Tablet OS — music, files, photos, editor, WiFi, settings, web file manager |
| `vnc_client/` | ota_0 | 0x310000 | 3MB | VNC remote desktop — Tight/ZRLE/Raw encoding, HW JPEG decode |
| `usb_display/` | ota_1 | 0x610000 | 1MB | USB extended display — turns tablet into a USB monitor |

## Triple-Boot Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                     32MB Flash Layout                          │
├──────┬──────┬──────┬───────────┬───────────┬────────┬────────┤
│ NVS  │ OTA  │ PHY  │  factory  │   ota_0   │ ota_1  │  FAT   │
│ 16K  │ data │ 4K   │   3MB     │    3MB    │  1MB   │ ~11MB  │
│0x9000│ 8K   │0xF000│  0x10000  │ 0x310000  │0x610000│0x710000│
│      │0xD000│      │ Main App  │VNC Client │USB Disp│Storage │
└──────┴──────┴──────┴───────────┴───────────┴────────┴────────┘
```

### How Switching Works

**Main App → VNC Client** (persistent):
- User taps "VNC" icon on desktop
- App calls `esp_ota_set_boot_partition(ota_0)` + `esp_restart()`
- VNC client marks itself valid on boot → persists across reboots
- User taps "Main App" button on VNC settings to return

**Main App → USB Display** (one-shot, auto-rollback):
- User taps "Display" icon on desktop
- App calls `esp_ota_set_boot_partition(ota_1)` + `esp_restart()`
- USB display does NOT mark itself valid
- On next RESET → bootloader rolls back to factory (main app) automatically

**Rollback mechanism**: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is set in main_app and vnc_client. Both call `esp_ota_mark_app_valid_cancel_rollback()` on boot. USB display never calls it, so the bootloader treats it as a failed update and rolls back on reboot.

### Shared Data (NVS)

All apps read/write the same NVS flash region:

| Namespace | Key | Purpose |
|-----------|-----|---------|
| `wifi` | `ssid` | WiFi network name |
| `wifi` | `pass` | WiFi password |
| `vnc` | `host` | VNC server IP |
| `vnc` | `port` | VNC server port |
| `vnc` | `vnc_pass` | VNC authentication password |
| `vnc` | `encoding` | 0=Tight, 1=ZRLE, 2=Raw |
| `vnc` | `jpeg_q` | JPEG quality (0-9) |
| `vnc` | `comp_lvl` | Compression level (1-9) |

## Build

Requires ESP-IDF v5.5.4.

```powershell
# Set environment (required for all builds)
$env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.5.4"
$env:ESP_IDF_VERSION = "5.5"
$env:PATH = "C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;" + $env:PATH
```

**Important**: `ESP_IDF_VERSION=5.5` MUST be set before building (fixes esp_wifi_remote Kconfig resolution).

### Build Each App

```powershell
# Main App
cd main_app
idf.py build

# VNC Client
cd ../vnc_client
idf.py build

# USB Display
cd ../usb_display
idf.py set-target esp32p4
idf.py build
```

### Post-Build: Apply iPazzport Patch (main_app and vnc_client)

After the first build of main_app and vnc_client (which fetches `managed_components`), apply the USB HID patch:

```powershell
# In main_app/:
copy patches\esp_lvgl_port_usbhid.c managed_components\espressif__esp_lvgl_port\src\lvgl9\
idf.py build

# In vnc_client/:
copy patches\esp_lvgl_port_usbhid.c managed_components\espressif__esp_lvgl_port\src\lvgl9\
idf.py build
```

This patches the iPazzport 8-byte HID report format (16-bit axes, report ID 0x01) and ignores SET_IDLE/SET_PROTOCOL STALLs.

## Flash

### Initial Flash (all three apps)

```powershell
# 1. Flash main app to factory (includes bootloader + partition table + otadata)
cd main_app
idf.py -p COM23 flash

# 2. Flash VNC client to ota_0
python -m esptool --chip esp32p4 -p COM23 -b 460800 --before=default_reset --after=hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB 0x310000 ../vnc_client/build/vnc_client.bin

# 3. Flash USB display to ota_1
python -m esptool --chip esp32p4 -p COM23 -b 460800 --before=default_reset --after=hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB 0x610000 ../usb_display/build/usb_touch_screen.bin
```

### Update Individual Apps

```powershell
# Main app only (rewrites factory + bootloader + partition table)
cd main_app && idf.py -p COM23 flash

# VNC client only
python -m esptool --chip esp32p4 -p COM23 -b 460800 --before=default_reset --after=hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB 0x310000 vnc_client/build/vnc_client.bin

# USB display only
python -m esptool --chip esp32p4 -p COM23 -b 460800 --before=default_reset --after=hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB 0x610000 usb_display/build/usb_touch_screen.bin
```

### Critical Flash Rules

- **NEVER** use `idf.py flash` from vnc_client/ or usb_display/ — it overwrites factory
- **ALWAYS** use esptool with explicit offsets for ota_0 and ota_1
- Device does NOT auto-reset after flash — press RESET manually
- Put device in boot mode: hold BOOT, press RESET, release BOOT

## USB Display Mode

The USB display turns the tablet into an external monitor via USB:
- Connect Type-C port to PC for power
- Connect Type-A port to PC for video data
- Install Windows driver from `usb_display/windows_driver/`
- Press RESET to return to main app (auto-rollback)

## Web File Manager

When WiFi is connected in the main app, open the device IP in a browser to access:
- Device status (memory, uptime, audio state)
- WiFi credential management
- SD card file browser (browse, upload, download, delete)
- Brightness and volume controls
- Default login: admin / esp32tablet

## Known Issues

- ZRLE encoding (VNC) has a subtle blue tint on some static solid-color areas. Tight and Raw modes unaffected.
- iPazzport patch must be reapplied after `managed_components` is re-fetched.
- First WiFi connect attempt usually fails (ESP-Hosted SDIO settling). Auto-retries.

## License

MIT
