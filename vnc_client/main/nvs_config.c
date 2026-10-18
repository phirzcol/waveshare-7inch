// =============================================================================
// NVS Configuration — VNC Connection Parameters (Implementation)
// =============================================================================
// Implements load/save/clear for WiFi + VNC settings stored in NVS.
// Follows the same patterns as wifi_app.c (nvs_open, nvs_set_str/get_str,
// nvs_commit, nvs_close).
// =============================================================================

#include "nvs_config.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "NVS_CONFIG";

// =============================================================================
// Load
// =============================================================================

esp_err_t nvs_config_load(nvs_connection_params_t *params)
{
    if (!params) return ESP_ERR_INVALID_ARG;

    // Zero out the struct — all _valid flags default to false
    memset(params, 0, sizeof(nvs_connection_params_t));

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_VNC_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        // Namespace doesn't exist yet — not an error, just no saved data
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No saved VNC config in NVS");
            return ESP_OK;
        }
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // SSID
    size_t len = sizeof(params->ssid);
    ret = nvs_get_str(handle, NVS_KEY_SSID, params->ssid, &len);
    if (ret == ESP_OK) {
        params->ssid_valid = true;
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to read SSID: %s", esp_err_to_name(ret));
    }

    // Password
    len = sizeof(params->password);
    ret = nvs_get_str(handle, NVS_KEY_PASSWORD, params->password, &len);
    if (ret == ESP_OK) {
        params->password_valid = true;
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to read password: %s", esp_err_to_name(ret));
    }

    // IP Address
    len = sizeof(params->ip_addr);
    ret = nvs_get_str(handle, NVS_KEY_IP_ADDR, params->ip_addr, &len);
    if (ret == ESP_OK) {
        params->ip_valid = true;
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to read IP address: %s", esp_err_to_name(ret));
    }

    // Port (uint16)
    ret = nvs_get_u16(handle, NVS_KEY_PORT, &params->port);
    if (ret == ESP_OK) {
        params->port_valid = true;
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to read port: %s", esp_err_to_name(ret));
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Config loaded — ssid:%s ip:%s port:%s",
             params->ssid_valid ? "yes" : "no",
             params->ip_valid ? "yes" : "no",
             params->port_valid ? "yes" : "no");
    return ESP_OK;
}

// =============================================================================
// Save
// =============================================================================

esp_err_t nvs_config_save(const char *ssid, const char *password,
                          const char *ip_addr, uint16_t port)
{
    if (!ssid || !ip_addr) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_VNC_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // SSID
    ret = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS set SSID failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    // Password (may be empty string)
    ret = nvs_set_str(handle, NVS_KEY_PASSWORD, password ? password : "");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS set password failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    // IP Address
    ret = nvs_set_str(handle, NVS_KEY_IP_ADDR, ip_addr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS set IP failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    // Port
    ret = nvs_set_u16(handle, NVS_KEY_PORT, port);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS set port failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    // Commit all writes
    ret = nvs_commit(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Config saved — SSID:'%s' IP:'%s' Port:%u", ssid, ip_addr, port);
    }

    nvs_close(handle);
    return ret;
}

// =============================================================================
// Clear
// =============================================================================

esp_err_t nvs_config_clear(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_VNC_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Erase all keys in the namespace
    nvs_erase_key(handle, NVS_KEY_SSID);
    nvs_erase_key(handle, NVS_KEY_PASSWORD);
    nvs_erase_key(handle, NVS_KEY_IP_ADDR);
    nvs_erase_key(handle, NVS_KEY_PORT);

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "All VNC config cleared from NVS");
    } else {
        ESP_LOGE(TAG, "NVS commit failed during clear: %s", esp_err_to_name(ret));
    }

    return ret;
}
