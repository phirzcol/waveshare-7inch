---
inclusion: auto
---

# Project Index

## What This Is
Triple-boot firmware for Waveshare ESP32-P4-WIFI6-Touch-LCD-7B tablet.
Three apps share 32MB flash via OTA partition switching.

## Apps
- `main_app/` — Tablet OS (music, files, photos, editor, WiFi, web file manager)
- `vnc_client/` — VNC remote desktop (Tight/ZRLE/Raw, HW JPEG)
- `usb_display/` — USB extended monitor (auto-rollback on reboot)

## Build Environment
- ESP-IDF v5.5.4 at `C:\Espressif\frameworks\esp-idf-v5.5.4`
- Target: esp32p4, COM23, 32MB flash, 32MB PSRAM
- **MUST set `$env:ESP_IDF_VERSION = "5.5"` before any build**
- The gdbinit OSError about ESP_ROM_ELF_DIR is harmless — build succeeds despite exit code 1

## Working Style
- Fix the code, not the user. No hardware workaround suggestions.
- Be direct. Skip filler.
- If something fails twice, rethink fundamentally.
- The user knows their hardware works.

## Steering Modules (use #filename in chat to load)
| File | When to load |
|------|-------------|
| `hardware.md` | Pin assignments, peripherals, board layout |
| `display-lvgl.md` | LVGL 9.2, screen transitions, OSK, lv_image |
| `wifi-hosted.md` | WiFi via ESP-Hosted SDIO, init sequence |
| `audio.md` | MP3/WAV playback, ring buffer, ES8311 |
| `usb-hid.md` | iPazzport keyboard/mouse, patch procedure |
| `vnc-client.md` | VNC protocol, encodings, framebuffer |
| `usb-display.md` | USB extend screen mode, rollback |
| `build-flash.md` | Partition layout, flash commands, env vars |
| `dead-ends.md` | Things that DON'T work — avoid retrying |

## Critical Rules
- Never modify `backup_sketch/` (frozen reference)
- Never use `lv_canvas` for framebuffers — always `lv_image` + `jpeg_alloc_decoder_mem()`
- All screen transitions MUST use `lv_async_call()`
- iPazzport patch must be reapplied after managed_components re-fetch
- WiFi init on-demand only, never at boot
