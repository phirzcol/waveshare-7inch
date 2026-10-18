#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * Start the HTTP server. Called when WiFi obtains an IP (Station or AP mode).
 * Allocates buffers from PSRAM. Idempotent (no-op if already running).
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if PSRAM allocation fails,
 *         or other esp_err_t on httpd_start failure.
 */
esp_err_t web_server_start(void);

/**
 * Stop the HTTP server. Called when WiFi disconnects or is switched off.
 * Frees all PSRAM buffers. Idempotent (no-op if not running).
 *
 * @return ESP_OK on success, or esp_err_t if httpd_stop fails.
 */
esp_err_t web_server_stop(void);

/**
 * Check if the web server is currently running.
 *
 * @return true if the server is active and listening, false otherwise.
 */
bool web_server_is_running(void);

/**
 * Get the shared PSRAM JSON scratch buffer (for use by URI handlers).
 * Only valid while the server is running.
 *
 * @return Pointer to the JSON buffer, or NULL if server is stopped.
 */
char *web_server_get_json_buf(void);

/**
 * Get the size of the shared JSON scratch buffer.
 *
 * @return Buffer size in bytes (2048).
 */
size_t web_server_get_json_buf_size(void);
