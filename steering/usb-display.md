---
inclusion: manual
---

# USB Display Mode

## What It Does
Turns the tablet into a USB external monitor. PC sends framebuffer data over USB, ESP32-P4 displays it on the 7" screen.

## USB Wiring
- **Type-C**: Power input from PC
- **Type-A**: Video data from PC (USB bulk transfer)
- Both must be connected to the same PC

## Windows Driver
Located in `usb_display/windows_driver/` — must be installed on the PC for the display to enumerate.

## Rollback Mechanism
The USB display uses OTA rollback to auto-return to main app:
1. Main app calls `esp_ota_set_boot_partition(ota_1)` + `esp_restart()`
2. USB display boots but NEVER calls `esp_ota_mark_app_valid_cancel_rollback()`
3. On next RESET → bootloader sees unconfirmed app → rolls back to factory
4. Main app boots and calls `esp_ota_mark_app_valid_cancel_rollback()` immediately

Requires `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` in main_app and vnc_client sdkconfig.defaults.

## Partition
- Lives in ota_1 at offset 0x610000 (1MB)
- Binary is ~455KB — plenty of room

## "Report not sent" Warnings
`W tinyusb_hid.h: Report not sent` — harmless. Touch HID reports fail when PC hasn't enumerated the HID endpoint yet. Display function works independently.

## Source
Based on Waveshare example `12_usb_extend_screen`. Uses TinyUSB device stack + custom USB display class.
