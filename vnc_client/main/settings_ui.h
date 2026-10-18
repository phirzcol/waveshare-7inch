#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

/** Connection parameters collected from the UI */
typedef struct {
    char ssid[33];       // 1-32 chars + null
    char password[64];   // 0-63 chars + null
    char ip_addr[16];    // "xxx.xxx.xxx.xxx" + null
    uint16_t port;       // 1-65535
} settings_params_t;

/** AP scan result record for display */
typedef struct {
    char ssid[33];
    int8_t rssi;
} settings_ap_record_t;

/** Callback invoked when user taps Connect with valid parameters */
typedef void (*settings_connect_cb_t)(const settings_params_t *params);

/** Callback invoked when user taps Scan */
typedef void (*settings_scan_cb_t)(void);

/** Callback invoked when user taps Clear Saved */
typedef void (*settings_clear_cb_t)(void);

/** Configuration for creating the settings screen */
typedef struct {
    settings_connect_cb_t on_connect;
    settings_scan_cb_t on_scan;
    settings_clear_cb_t on_clear;
} settings_ui_config_t;

/**
 * @brief Create the settings UI screen with all widgets.
 * Must be called with LVGL lock held.
 * @return The settings screen lv_obj_t pointer.
 */
lv_obj_t *settings_ui_create(const settings_ui_config_t *config);

/**
 * @brief Pre-populate fields with saved values.
 * Call after settings_ui_create with values loaded from NVS.
 */
void settings_ui_set_params(const settings_params_t *params);

/**
 * @brief Display the AP scan results list.
 * @param ap_list Array of AP records.
 * @param count Number of APs in the list.
 */
void settings_ui_show_ap_list(const settings_ap_record_t *ap_list, uint8_t count);

/**
 * @brief Update the status label text (e.g., "Connecting to WiFi...").
 */
void settings_ui_set_status(const char *text);

/**
 * @brief Show an error message in the status area.
 */
void settings_ui_set_error(const char *text);

/**
 * @brief Enable or disable all input fields and buttons during connection.
 */
void settings_ui_set_inputs_enabled(bool enabled);

/**
 * @brief Show/hide the scanning indicator and disable/enable Scan button.
 */
void settings_ui_set_scanning(bool scanning);

/**
 * @brief Clear the error/status when user modifies a field.
 */
void settings_ui_clear_status(void);
