---
inclusion: manual
---

# Dead Ends — Things That DON'T Work

## Display
- **`lv_canvas`** for full-screen framebuffers — scrambled vertical stripes on ESP32-P4 MIPI-DSI
- **`heap_caps_malloc(MALLOC_CAP_SPIRAM)`** for display buffers — not DMA-aligned, causes corruption
- **Height 608** (16-aligned) in lv_image_dsc_t — LVGL miscalculates layout, use 600
- **`esp_async_memcpy_install_gdma_axi()`** — conflicts with JPEG decoder DMA
- **Direct `esp_lcd_panel_draw_bitmap()`** without LVGL — can't coexist with LVGL display
- **`lv_image_set_scale()`** on full-screen canvas — kills frame rate

## LVGL
- **`lv_obj_delete()` inside keyboard event callback** — use-after-free crash
- **`lv_obj_delete_async()`** for keyboard — LVGL task infinite loop + watchdog
- **Direct screen transitions from event callbacks** — LVGL infinite loop
- **`lv_group_add_obj`/`remove_obj` dynamically** — crashes, dangling pointers

## ZRLE Encoding (VNC)
- **3-byte CPIXEL BGR** — total multicolored garbage
- **3-byte CPIXEL RGB** — same garbage
- **Big-endian 2-byte reads** — all colors inverted
- **Disable PPA entirely (CPU fallback)** — WORSE, striped multicolored boxes
- **Cache flush/invalidate in vnc_display_swap** — no effect
- **R↔B swap on PPA fill_color_val** — no effect
- **PPA SRM for raw pixel blits** — crashed due to alignment issues

## Audio
- **Large I2S DMA buffers** to fix stutter — causes reverb/echo
- **Calling `is_mp3()` after seeking** — rewinds file to 0, destroys seek position
- **WiFi active during audio playback** — SDIO bus contention causes stutter (ring buffer mitigates but doesn't eliminate)

## WiFi / ESP-Hosted
- **WiFi init at boot** — SDIO polling affects audio/display before system settles
- **SDIO queue sizes > 10** — causes hard resets under VNC streaming load
- **`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`** without careful management — DMA conflicts

## USB HID
- **`ESP_ERROR_CHECK` on SET_IDLE/SET_PROTOCOL** — iPazzport STALLs, aborts program
- **`hid_host_device_close()` in disconnect callback** — blocks, triggers watchdog
- **HID init from main task** — stack too small, needs separate 8KB task
- **Passing NULL for display handle** — `lvgl_port_add_usb_hid_mouse_input` asserts
- **Sensitivity > 1** — iPazzport already sends scaled values, multiplying makes cursor barely move

## Memory
- **LVGL 64KB built-in pool** — too small for settings UI, use `CONFIG_LV_USE_CLIB_MALLOC=y`
- **`CONFIG_ESP_PSRAM_RESERVE_INTERNAL_DMA_SIZE=0`** — USB HID needs internal DMA buffers
