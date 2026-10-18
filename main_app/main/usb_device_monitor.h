#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lvgl_port_usbhid.h"

esp_err_t usb_device_monitor_start(void);
bool usb_device_monitor_xinput_is_connected(void);
void usb_device_monitor_xinput_get_state(lvgl_port_gamepad_state_t *state);
void usb_device_monitor_xinput_get_vid_pid(uint16_t *vid, uint16_t *pid);
int usb_device_monitor_xinput_get_raw_report(uint8_t *buf, size_t buf_size);
