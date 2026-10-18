// =============================================================================
// WiFi AP Scan Module
// =============================================================================
// Performs a blocking WiFi scan with 10-second timeout.
// Returns up to 20 APs sorted by RSSI descending (strongest first).
// WiFi must be initialized and started (STA mode) before calling.
// Uses esp_wifi APIs — ESP-Hosted SDIO to C6 handles transport transparently.
// =============================================================================

#include "wifi_scan.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WIFI_SCAN";

/**
 * @brief Comparator for qsort — sorts by RSSI descending (strongest first).
 */
static int rssi_compare_desc(const void *a, const void *b)
{
    const wifi_scan_ap_t *ap_a = (const wifi_scan_ap_t *)a;
    const wifi_scan_ap_t *ap_b = (const wifi_scan_ap_t *)b;

    // Sort descending: higher RSSI (less negative) comes first
    if (ap_b->rssi > ap_a->rssi) return 1;
    if (ap_b->rssi < ap_a->rssi) return -1;
    return 0;
}

esp_err_t wifi_scan_start(wifi_scan_result_t *result)
{
    if (!result) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(wifi_scan_result_t));

    // Configure scan: active scan, no hidden networks, no specific channel
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 100,
                .max = 300,
            },
        },
    };

    // Start blocking scan with implicit timeout
    // The blocking call returns when scan completes or fails
    ESP_LOGI(TAG, "Starting WiFi scan (blocking, 10s timeout)...");
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Retrieve scan results
    uint16_t ap_count = 0;
    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Scan found %d APs", ap_count);

    // Cap to our maximum
    if (ap_count > WIFI_SCAN_MAX_AP) {
        ap_count = WIFI_SCAN_MAX_AP;
    }

    // Fetch AP records from the WiFi driver
    wifi_ap_record_t ap_records[WIFI_SCAN_MAX_AP];
    uint16_t fetch_count = ap_count;
    ret = esp_wifi_scan_get_ap_records(&fetch_count, ap_records);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Copy SSID and RSSI into our result structure
    uint8_t valid_count = 0;
    for (uint16_t i = 0; i < fetch_count && valid_count < WIFI_SCAN_MAX_AP; i++) {
        // Skip hidden networks (empty SSID)
        if (ap_records[i].ssid[0] == '\0') {
            continue;
        }

        strncpy(result->aps[valid_count].ssid, (const char *)ap_records[i].ssid, 32);
        result->aps[valid_count].ssid[32] = '\0';
        result->aps[valid_count].rssi = ap_records[i].rssi;
        valid_count++;
    }

    result->count = valid_count;

    // Sort by RSSI descending (strongest signal first)
    if (result->count > 1) {
        qsort(result->aps, result->count, sizeof(wifi_scan_ap_t), rssi_compare_desc);
    }

    ESP_LOGI(TAG, "Scan complete: %d APs (after filtering hidden)", result->count);
    return ESP_OK;
}
