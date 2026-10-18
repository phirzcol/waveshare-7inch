---
inclusion: manual
---

# Hardware — Waveshare ESP32-P4-WIFI6-Touch-LCD-7B

## MCU
- ESP32-P4: dual-core RISC-V, 360MHz
- 32MB PSRAM (SPIRAM), 32MB Flash
- No built-in WiFi/BT — uses companion ESP32-C6

## Display
- 7" IPS, 1024×600, MIPI-DSI 2-lane
- Controller: EK79007
- Backlight: GPIO32, **inverted** (low = bright)
- BSP handles all init via `bsp_display_start_with_config()`

## Touch
- GT911 capacitive, I2C (SCL=8, SDA=7, reset=GPIO23)
- BSP auto-registers with LVGL

## Audio
- DAC: ES8311 (I2C addr 0x18)
- ADC: ES7210 (I2C addr 0x40)
- I2S bus, PA enable: GPIO53
- Codec init: `bsp_extra_codec_init()`

## SD Card
- SDMMC 4-bit (CLK=43, CMD=44, D0=39, D1=40, D2=41, D3=42)
- LDO channel 4
- Mount: `bsp_sdcard_mount()` → `/sdcard`

## WiFi / Bluetooth
- ESP32-C6-MINI-1 via SDIO (WiFi 6 / BLE 5)
- SDIO pins: CLK=18, CMD=19, D0=14, D1=15, D2=16, D3=17
- Reset: GPIO54
- ESP-Hosted driver (slave target: ESP32-C6)

## USB
- **Type-A** (host port): USB 2.0 HS — for HID devices (keyboard/mouse)
- **Type-C** (device port): USB 2.0 HS — for USB display mode, power input
- USB Host init: `bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, false)`

## Camera (unused currently)
- OV5647, MIPI CSI-2

## Key BSP Functions
```c
bsp_display_start_with_config(&cfg)  // Returns lv_display_t*
bsp_display_backlight_on()
bsp_sdcard_mount()                   // Mounts to /sdcard
bsp_usb_host_start(mode, false)
bsp_extra_codec_init()               // ES8311 + ES7210
bsp_display_lock(0) / bsp_display_unlock()  // LVGL mutex
```
