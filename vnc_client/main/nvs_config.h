// =============================================================================
// NVS Configuration — VNC Connection Parameters
// =============================================================================
// Provides load/save/clear operations for VNC client connection parameters
// stored in NVS under the "vnc" namespace.
// =============================================================================

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/** NVS namespace for VNC connection parameters */
#define NVS_VNC_NAMESPACE "vnc"

/** Keys used in NVS */
#define NVS_KEY_SSID     "ssid"
#define NVS_KEY_PASSWORD "password"
#define NVS_KEY_IP_ADDR  "ip_addr"
#define NVS_KEY_PORT     "port"

/** Connection parameters structure with validity flags */
typedef struct {
    char ssid[33];        // 1-32 chars + null
    char password[64];    // 0-63 chars + null
    char ip_addr[16];     // "xxx.xxx.xxx.xxx" + null
    uint16_t port;        // 1-65535
    bool ssid_valid;      // true if SSID was read from NVS
    bool password_valid;  // true if password was read from NVS
    bool ip_valid;        // true if IP was read from NVS
    bool port_valid;      // true if port was read from NVS
} nvs_connection_params_t;

/**
 * @brief Load all connection parameters from NVS.
 * Fields that don't exist in NVS will have their _valid flag set to false.
 * @param params Output structure to populate.
 * @return ESP_OK on success, error code if NVS open fails.
 */
esp_err_t nvs_config_load(nvs_connection_params_t *params);

/**
 * @brief Save all connection parameters to NVS.
 * @param ssid WiFi SSID (1-32 chars).
 * @param password WiFi password (0-63 chars, may be empty).
 * @param ip_addr VNC server IP address string.
 * @param port VNC server port (1-65535).
 * @return ESP_OK on success, error code on failure.
 */
esp_err_t nvs_config_save(const char *ssid, const char *password,
                          const char *ip_addr, uint16_t port);

/**
 * @brief Erase all connection parameters from NVS.
 * @return ESP_OK on success, error code on failure.
 */
esp_err_t nvs_config_clear(void);
