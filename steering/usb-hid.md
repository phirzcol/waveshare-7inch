---
inclusion: manual
---

# USB HID — iPazzport Keyboard/Mouse Combo

## Device Behavior
- Sends 8-byte mouse reports with report ID prefix
- Sends standard boot protocol keyboard reports
- STALLs on SET_IDLE and SET_PROTOCOL USB class requests
- Sends continuous idle reports (all zeros) at ~100Hz when connected
- Fast downward swipe sends GUI+Z keyboard report (device firmware gesture, not fixable)

## Mouse Report Format (8 bytes)
```
Byte 0: Report ID (always 0x01)
Byte 1: Buttons — bit 0 = left click, bit 1 = right click
Byte 2-3: X displacement (16-bit signed LE)
Byte 4-5: Y displacement (16-bit signed LE)
Byte 6: Scroll wheel (signed int8)
Byte 7: unused
```

## Keyboard Report (standard boot protocol)
```
Byte 0: Modifier keys (LCtrl, LShift, LAlt, LGUI, RCtrl, RShift, RAlt, RGUI)
Byte 1: Reserved
Bytes 2-7: Up to 6 simultaneous keycodes
```

## Patch Required
File: `patches/esp_lvgl_port_usbhid.c`
Target: `managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_usbhid.c`

**Must reapply after every `managed_components` re-fetch or clean build.**

The patch:
- Detects iPazzport format: `if (data_length >= 6 && data[0] == 0x01)`
- Parses 16-bit LE axes directly: `x += dx, y += dy` (no swap)
- Ignores SET_IDLE/SET_PROTOCOL errors (removes ESP_ERROR_CHECK)

## Init Sequence
```c
bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, false);
// In separate task with 8KB stack:
lvgl_port_hid_mouse_cfg_t mouse_cfg = { .disp = s_disp, .sensitivity = 1 };
lvgl_port_add_usb_hid_mouse_input(&mouse_cfg);
lvgl_port_hid_keyboard_cfg_t kb_cfg = { .disp = s_disp };
lv_indev_t *kb = lvgl_port_add_usb_hid_keyboard_input(&kb_cfg);
lv_group_t *group = lv_group_create();
lv_group_set_default(group);
lv_indev_set_group(kb, group);
```

## Rules
- MUST pass valid `lv_display_t*` to mouse config (NULL causes assert crash)
- HID init in separate task with 8KB stack (main task too small)
- Don't call `hid_host_device_close()` in disconnect callback — triggers watchdog
- LVGL calls from USB HID task MUST be wrapped in `bsp_display_lock()/unlock()`
- Sensitivity = 1 (iPazzport sends appropriately-scaled values already)
