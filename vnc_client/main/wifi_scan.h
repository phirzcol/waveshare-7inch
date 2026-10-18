#pragma once

#include "esp_err.h"
#include <stdint.h>

/** Maximum number of APs returned from a scan */
#define WIFI_SCAN_MAX_AP 20

/** Scan result record */
typedef struct {
    char ssid[33];
    int8_t rssi;
} wifi_scan_ap_t;

/** Scan result container */
typedef struct {
    wifi_scan_ap_t aps[WIFI_SCAN_MAX_AP];
    uint8_t count;
} wifi_scan_result_t;

/**
 * @brief Perform a WiFi AP scan with a 10-second timeout.
 * Results are sorted by RSSI descending (strongest signal first).
 * WiFi must be initialized and started before calling.
 * @param result Output scan results.
 * @return ESP_OK if scan completed (even with 0 results), error on failure.
 */
esp_err_t wifi_scan_start(wifi_scan_result_t *result);
