/**
 * @file settings_manager.c
 * @brief Centralized settings management with NVS persistence.
 *
 * All device settings (brightness, volume, AP credentials, admin password)
 * are stored in NVS under the "settings" namespace. Both the web API and
 * the on-device LVGL UI read/write through this module to ensure consistency.
 */

#pragma GCC diagnostic ignored "-Wstringop-truncation"

#include "settings_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include <string.h>

static const char *TAG = "SETTINGS";

/* Default values */
#define DEFAULT_BRIGHTNESS  80
#define DEFAULT_VOLUME      50
#define DEFAULT_AP_SSID     "ESP32-P4-Tablet"
#define DEFAULT_AP_PASS     "steampunk"
#define DEFAULT_ADMIN_PASS  "esp32tablet"

esp_err_t settings_load(device_settings_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Set defaults first */
    out->brightness = DEFAULT_BRIGHTNESS;
    out->volume = DEFAULT_VOLUME;
    strncpy(out->ap_ssid, DEFAULT_AP_SSID, sizeof(out->ap_ssid) - 1);
    out->ap_ssid[sizeof(out->ap_ssid) - 1] = '\0';
    strncpy(out->ap_pass, DEFAULT_AP_PASS, sizeof(out->ap_pass) - 1);
    out->ap_pass[sizeof(out->ap_pass) - 1] = '\0';
    strncpy(out->admin_pass, DEFAULT_ADMIN_PASS, sizeof(out->admin_pass) - 1);
    out->admin_pass[sizeof(out->admin_pass) - 1] = '\0';

    /* Open NVS */
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_PART_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS namespace not found, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS: %s, using defaults", esp_err_to_name(err));
        return ESP_OK;
    }

    /* Read brightness */
    uint8_t u8_val;
    err = nvs_get_u8(handle, SETTINGS_KEY_BRIGHTNESS, &u8_val);
    if (err == ESP_OK) {
        out->brightness = u8_val;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Error reading brightness: %s", esp_err_to_name(err));
    }

    /* Read volume */
    err = nvs_get_u8(handle, SETTINGS_KEY_VOLUME, &u8_val);
    if (err == ESP_OK) {
        out->volume = u8_val;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Error reading volume: %s", esp_err_to_name(err));
    }

    /* Read AP SSID */
    size_t str_len = sizeof(out->ap_ssid);
    err = nvs_get_str(handle, SETTINGS_KEY_AP_SSID, out->ap_ssid, &str_len);
    if (err == ESP_OK) {
        /* Successfully read from NVS */
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Error reading AP SSID: %s", esp_err_to_name(err));
        /* Restore default */
        strncpy(out->ap_ssid, DEFAULT_AP_SSID, sizeof(out->ap_ssid) - 1);
        out->ap_ssid[sizeof(out->ap_ssid) - 1] = '\0';
    }

    /* Read AP password */
    str_len = sizeof(out->ap_pass);
    err = nvs_get_str(handle, SETTINGS_KEY_AP_PASS, out->ap_pass, &str_len);
    if (err == ESP_OK) {
        /* Successfully read from NVS */
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Error reading AP password: %s", esp_err_to_name(err));
        strncpy(out->ap_pass, DEFAULT_AP_PASS, sizeof(out->ap_pass) - 1);
        out->ap_pass[sizeof(out->ap_pass) - 1] = '\0';
    }

    /* Read admin password */
    str_len = sizeof(out->admin_pass);
    err = nvs_get_str(handle, SETTINGS_KEY_ADMIN_PASS, out->admin_pass, &str_len);
    if (err == ESP_OK) {
        /* Successfully read from NVS */
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Error reading admin password: %s", esp_err_to_name(err));
        strncpy(out->admin_pass, DEFAULT_ADMIN_PASS, sizeof(out->admin_pass) - 1);
        out->admin_pass[sizeof(out->admin_pass) - 1] = '\0';
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Settings loaded: brightness=%d, volume=%d, ap_ssid=%s",
             out->brightness, out->volume, out->ap_ssid);
    return ESP_OK;
}

uint8_t settings_get_brightness(void)
{
    nvs_handle_t handle;
    uint8_t value = DEFAULT_BRIGHTNESS;

    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get_brightness: NVS open failed, returning default");
        return value;
    }

    err = nvs_get_u8(handle, SETTINGS_KEY_BRIGHTNESS, &value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "get_brightness: read failed: %s", esp_err_to_name(err));
        value = DEFAULT_BRIGHTNESS;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        value = DEFAULT_BRIGHTNESS;
    }

    nvs_close(handle);
    return value;
}

uint8_t settings_get_volume(void)
{
    nvs_handle_t handle;
    uint8_t value = DEFAULT_VOLUME;

    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get_volume: NVS open failed, returning default");
        return value;
    }

    err = nvs_get_u8(handle, SETTINGS_KEY_VOLUME, &value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "get_volume: read failed: %s", esp_err_to_name(err));
        value = DEFAULT_VOLUME;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        value = DEFAULT_VOLUME;
    }

    nvs_close(handle);
    return value;
}

esp_err_t settings_set_brightness(uint8_t value)
{
    if (value > 100) {
        ESP_LOGE(TAG, "set_brightness: invalid value %d (must be 0-100)", value);
        return ESP_ERR_INVALID_ARG;
    }

    /* Write to NVS */
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_brightness: NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, SETTINGS_KEY_BRIGHTNESS, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_brightness: NVS write failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_brightness: NVS commit failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Apply to hardware via BSP */
    err = bsp_display_brightness_set((int)value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_brightness: hardware apply failed: %s", esp_err_to_name(err));
        /* NVS was written successfully, so don't return error */
    }

    ESP_LOGI(TAG, "Brightness set to %d%%", value);
    return ESP_OK;
}

esp_err_t settings_set_volume(uint8_t value)
{
    if (value > 100) {
        ESP_LOGE(TAG, "set_volume: invalid value %d (must be 0-100)", value);
        return ESP_ERR_INVALID_ARG;
    }

    /* Write to NVS */
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_volume: NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, SETTINGS_KEY_VOLUME, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_volume: NVS write failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_volume: NVS commit failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Apply to hardware via BSP codec */
    err = bsp_extra_codec_volume_set((int)value, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_volume: hardware apply failed: %s", esp_err_to_name(err));
        /* NVS was written successfully, so don't return error */
    }

    ESP_LOGI(TAG, "Volume set to %d%%", value);
    return ESP_OK;
}

esp_err_t settings_set_ap_credentials(const char *ssid, const char *password)
{
    /* Validate SSID: not NULL, length 1-32 */
    if (ssid == NULL) {
        ESP_LOGE(TAG, "set_ap_credentials: SSID is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    size_t ssid_len = strlen(ssid);
    if (ssid_len < 1 || ssid_len > 32) {
        ESP_LOGE(TAG, "set_ap_credentials: SSID length %d invalid (must be 1-32)", (int)ssid_len);
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate password: not NULL, length 8-63 */
    if (password == NULL) {
        ESP_LOGE(TAG, "set_ap_credentials: password is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    size_t pass_len = strlen(password);
    if (pass_len < 8 || pass_len > 63) {
        ESP_LOGE(TAG, "set_ap_credentials: password length %d invalid (must be 8-63)", (int)pass_len);
        return ESP_ERR_INVALID_ARG;
    }

    /* Write to NVS */
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_ap_credentials: NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, SETTINGS_KEY_AP_SSID, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_ap_credentials: NVS write SSID failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_set_str(handle, SETTINGS_KEY_AP_PASS, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_ap_credentials: NVS write password failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_ap_credentials: NVS commit failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "AP credentials updated: SSID=%s", ssid);
    return ESP_OK;
}

esp_err_t settings_change_admin_pass(const char *current_pass, const char *new_pass)
{
    if (current_pass == NULL || new_pass == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Load current admin password from NVS (or use default) */
    char stored_pass[33];
    strncpy(stored_pass, DEFAULT_ADMIN_PASS, sizeof(stored_pass) - 1);
    stored_pass[sizeof(stored_pass) - 1] = '\0';

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "change_admin_pass: NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t str_len = sizeof(stored_pass);
    err = nvs_get_str(handle, SETTINGS_KEY_ADMIN_PASS, stored_pass, &str_len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "change_admin_pass: read stored pass failed: %s", esp_err_to_name(err));
        /* Use default */
        strncpy(stored_pass, DEFAULT_ADMIN_PASS, sizeof(stored_pass) - 1);
        stored_pass[sizeof(stored_pass) - 1] = '\0';
    }

    /* Compare current_pass against stored */
    if (strcmp(current_pass, stored_pass) != 0) {
        ESP_LOGW(TAG, "change_admin_pass: current password mismatch");
        nvs_close(handle);
        return ESP_ERR_INVALID_STATE;
    }

    /* Validate new_pass: length 8-32 */
    size_t new_len = strlen(new_pass);
    if (new_len < 8 || new_len > 32) {
        ESP_LOGE(TAG, "change_admin_pass: new password length %d invalid (must be 8-32)", (int)new_len);
        nvs_close(handle);
        return ESP_ERR_INVALID_ARG;
    }

    /* Write new password to NVS */
    err = nvs_set_str(handle, SETTINGS_KEY_ADMIN_PASS, new_pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "change_admin_pass: NVS write failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "change_admin_pass: NVS commit failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Admin password changed successfully");
    return ESP_OK;
}

bool settings_verify_admin(const char *username, const char *password)
{
    /* Username must be "admin" */
    if (username == NULL || strcmp(username, "admin") != 0) {
        return false;
    }

    if (password == NULL) {
        return false;
    }

    /* Load admin password from NVS (or default) */
    char stored_pass[33];
    strncpy(stored_pass, DEFAULT_ADMIN_PASS, sizeof(stored_pass) - 1);
    stored_pass[sizeof(stored_pass) - 1] = '\0';

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t str_len = sizeof(stored_pass);
        err = nvs_get_str(handle, SETTINGS_KEY_ADMIN_PASS, stored_pass, &str_len);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "verify_admin: read stored pass failed: %s", esp_err_to_name(err));
            /* Use default */
            strncpy(stored_pass, DEFAULT_ADMIN_PASS, sizeof(stored_pass) - 1);
            stored_pass[sizeof(stored_pass) - 1] = '\0';
        }
        nvs_close(handle);
    } else {
        ESP_LOGW(TAG, "verify_admin: NVS open failed, using default password");
    }

    return (strcmp(password, stored_pass) == 0);
}
