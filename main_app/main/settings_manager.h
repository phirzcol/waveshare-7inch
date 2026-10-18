#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// --- NVS Namespace and Setting Keys ---
#define SETTINGS_NVS_NAMESPACE  "settings"
#define SETTINGS_KEY_BRIGHTNESS "bright"
#define SETTINGS_KEY_VOLUME     "volume"
#define SETTINGS_KEY_AP_SSID    "ap_ssid"
#define SETTINGS_KEY_AP_PASS    "ap_pass"
#define SETTINGS_KEY_ADMIN_PASS "admin_pw"

typedef struct {
    uint8_t  brightness;       // 0-100
    uint8_t  volume;           // 0-100
    char     ap_ssid[33];      // AP mode SSID (1-32 chars + null)
    char     ap_pass[64];      // AP mode password (8-63 chars + null)
    char     admin_pass[33];   // Web admin password (8-32 chars + null)
} device_settings_t;

/**
 * Load all settings from NVS into the struct.
 * Missing keys get defaults (brightness=80, volume=50, ap_ssid="ESP32-P4-Tablet",
 * ap_pass="steampunk", admin_pass="esp32tablet").
 */
esp_err_t settings_load(device_settings_t *out);

/**
 * Set brightness (0-100). Writes NVS + applies to GPIO32 immediately.
 * Returns ESP_ERR_INVALID_ARG if value > 100.
 */
esp_err_t settings_set_brightness(uint8_t value);

/**
 * Set volume (0-100). Writes NVS + applies to ES8311 immediately.
 * Returns ESP_ERR_INVALID_ARG if value > 100.
 */
esp_err_t settings_set_volume(uint8_t value);

/**
 * Set AP credentials. Validates SSID (1-32 bytes) and password (8-63 bytes).
 * Writes NVS. If AP is currently active, restarts it with new credentials.
 */
esp_err_t settings_set_ap_credentials(const char *ssid, const char *password);

/**
 * Change admin password. Validates current password first.
 * Returns ESP_ERR_INVALID_STATE if current_pass doesn't match.
 * New password must be 8-32 characters.
 */
esp_err_t settings_change_admin_pass(const char *current_pass, const char *new_pass);

/**
 * Verify admin credentials for HTTP Basic Auth.
 * Returns true if username=="admin" and password matches stored.
 */
bool settings_verify_admin(const char *username, const char *password);

/**
 * Get current brightness from NVS.
 */
uint8_t settings_get_brightness(void);

/**
 * Get current volume from NVS.
 */
uint8_t settings_get_volume(void);
