/**
 * @file web_api_handlers.c
 * @brief HTTP URI handler implementations for the settings REST API.
 *
 * Implements GET /, GET /api/settings, GET /api/status,
 * POST /api/settings, and POST /api/password.
 */

#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <string.h>
#include <stdlib.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "cJSON.h"
#include "mbedtls/base64.h"

#include "web_server.h"
#include "settings_manager.h"
#include "audio_player.h"

static const char *TAG = "WEB_API";

/* ---- Externs from main.c ---- */
extern bool s_wifi_connected;
extern char s_wifi_ip[20];
extern char s_tracks[64][256];
extern int  s_current_track;

/* ---- Embedded HTML page (linked from web_page.html via EMBED_FILES) ---- */
extern const uint8_t web_page_html_start[] asm("_binary_web_page_html_start");
extern const uint8_t web_page_html_end[]   asm("_binary_web_page_html_end");

/* ---- Helper: Set CORS header ---- */
static inline void set_cors_header(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

/* ---- Helper: Send JSON error response ---- */
static esp_err_t send_json_error(httpd_req_t *req, int status, const char *message)
{
    char *buf = web_server_get_json_buf();
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No buffer");
        return ESP_FAIL;
    }
    set_cors_header(req);
    snprintf(buf, web_server_get_json_buf_size(),
             "{\"status\":\"error\",\"message\":\"%s\"}", message);

    char status_str[4];
    snprintf(status_str, sizeof(status_str), "%d", status);
    httpd_resp_set_status(req, (status == 400) ? "400 Bad Request" :
                               (status == 401) ? "401 Unauthorized" :
                               (status == 403) ? "403 Forbidden" :
                               (status == 500) ? "500 Internal Server Error" : status_str);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---- Helper: Extract and verify HTTP Basic Auth ---- */
/* Returns true if auth is valid. On failure, sends 401 response. */
static bool check_basic_auth(httpd_req_t *req)
{
    char auth_header[256] = {0};
    int hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len <= 0 || hdr_len >= (int)sizeof(auth_header)) {
        set_cors_header(req);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32-P4 Settings\"");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Authentication required\"}", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    esp_err_t err = httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header));
    if (err != ESP_OK) {
        set_cors_header(req);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32-P4 Settings\"");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Authentication required\"}", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    /* Expect "Basic <base64>" */
    if (strncmp(auth_header, "Basic ", 6) != 0) {
        set_cors_header(req);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32-P4 Settings\"");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Authentication required\"}", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    const char *b64 = auth_header + 6;
    unsigned char decoded[128] = {0};
    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                    (const unsigned char *)b64, strlen(b64));
    if (ret != 0) {
        set_cors_header(req);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32-P4 Settings\"");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Authentication required\"}", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    decoded[decoded_len] = '\0';

    /* Split "username:password" */
    char *colon = strchr((char *)decoded, ':');
    if (!colon) {
        set_cors_header(req);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32-P4 Settings\"");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Authentication required\"}", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    *colon = '\0';
    const char *username = (const char *)decoded;
    const char *password = colon + 1;

    if (!settings_verify_admin(username, password)) {
        set_cors_header(req);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32-P4 Settings\"");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Authentication required\"}", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    return true;
}

/* ======================================================================== */
/* GET "/" — serve embedded HTML page                                        */
/* ======================================================================== */
esp_err_t handler_get_root(httpd_req_t *req)
{
    ESP_LOGD(TAG, "GET /");
    set_cors_header(req);
    httpd_resp_set_type(req, "text/html");
    const size_t page_len = web_page_html_end - web_page_html_start;
    httpd_resp_send(req, (const char *)web_page_html_start, page_len);
    return ESP_OK;
}

/* ======================================================================== */
/* GET "/api/settings" — return current settings as JSON                     */
/* ======================================================================== */
esp_err_t handler_get_settings(httpd_req_t *req)
{
    ESP_LOGD(TAG, "GET /api/settings");

    char *buf = web_server_get_json_buf();
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No buffer");
        return ESP_FAIL;
    }

    /* Load settings from NVS */
    device_settings_t settings;
    settings_load(&settings);

    /* Read WiFi SSID from NVS "wifi" namespace */
    char wifi_ssid[33] = "";
    {
        nvs_handle_t nvs;
        if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(wifi_ssid);
            nvs_get_str(nvs, "ssid", wifi_ssid, &len);
            nvs_close(nvs);
        }
    }

    /* Build JSON response */
    snprintf(buf, web_server_get_json_buf_size(),
             "{\"wifi_ssid\":\"%s\","
             "\"wifi_connected\":%s,"
             "\"brightness\":%d,"
             "\"volume\":%d,"
             "\"ap_ssid\":\"%s\","
             "\"ap_pass\":\"********\"}",
             wifi_ssid,
             s_wifi_connected ? "true" : "false",
             (int)settings.brightness,
             (int)settings.volume,
             settings.ap_ssid);

    set_cors_header(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ======================================================================== */
/* GET "/api/status" — return device status                                  */
/* ======================================================================== */
esp_err_t handler_get_status(httpd_req_t *req)
{
    ESP_LOGD(TAG, "GET /api/status");

    char *buf = web_server_get_json_buf();
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No buffer");
        return ESP_FAIL;
    }

    /* Uptime */
    int64_t uptime_us = esp_timer_get_time();
    int uptime_sec = (int)(uptime_us / 1000000);
    int uptime_hours = uptime_sec / 3600;
    int uptime_minutes = (uptime_sec % 3600) / 60;

    /* Memory */
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int free_internal_kb = (int)(free_internal / 1024);
    int free_psram_mb = (int)(free_psram / (1024 * 1024));

    /* WiFi mode */
    const char *wifi_mode = s_wifi_connected ? "station" : "ap";

    /* Audio state */
    const char *audio_state_str = "stopped";
    audio_player_state_t astate = audio_player_get_state();
    switch (astate) {
        case AUDIO_PLAYER_STATE_PLAYING:  audio_state_str = "playing"; break;
        case AUDIO_PLAYER_STATE_PAUSE:    audio_state_str = "paused"; break;
        case AUDIO_PLAYER_STATE_IDLE:     audio_state_str = "stopped"; break;
        case AUDIO_PLAYER_STATE_SHUTDOWN: audio_state_str = "stopped"; break;
        default: audio_state_str = "stopped"; break;
    }

    /* Audio track — extract filename from full path */
    const char *audio_track = "";
    if (s_current_track >= 0) {
        const char *path = s_tracks[s_current_track];
        const char *slash = strrchr(path, '/');
        audio_track = slash ? (slash + 1) : path;
    }

    /* Build JSON */
    snprintf(buf, web_server_get_json_buf_size(),
             "{\"ip\":\"%s\","
             "\"wifi_mode\":\"%s\","
             "\"free_internal_kb\":%d,"
             "\"free_psram_mb\":%d,"
             "\"uptime_hours\":%d,"
             "\"uptime_minutes\":%d,"
             "\"audio_state\":\"%s\","
             "\"audio_track\":\"%s\"}",
             s_wifi_ip,
             wifi_mode,
             free_internal_kb,
             free_psram_mb,
             uptime_hours,
             uptime_minutes,
             audio_state_str,
             audio_track);

    set_cors_header(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ======================================================================== */
/* POST "/api/settings" — update settings                                    */
/* ======================================================================== */
esp_err_t handler_post_settings(httpd_req_t *req)
{
    ESP_LOGD(TAG, "POST /api/settings");

    /* Check Basic Auth */
    if (!check_basic_auth(req)) {
        return ESP_OK; /* 401 already sent */
    }

    /* Read request body (max 512 bytes) */
    char body[512];
    int content_len = req->content_len;
    if (content_len <= 0 || content_len >= (int)sizeof(body)) {
        return send_json_error(req, 400, "Request body too large or empty");
    }

    int received = httpd_req_recv(req, body, content_len);
    if (received <= 0) {
        return send_json_error(req, 400, "Failed to read request body");
    }
    body[received] = '\0';

    /* Parse JSON */
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return send_json_error(req, 400, "Invalid JSON");
    }

    /* Process recognized keys */
    char updated_list[256] = "";
    int updated_count = 0;
    char warnings_list[256] = "";
    int warnings_count = 0;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        const char *key = item->string;
        if (!key) continue;

        if (strcmp(key, "brightness") == 0) {
            if (!cJSON_IsNumber(item)) {
                cJSON_Delete(root);
                return send_json_error(req, 400, "brightness must be a number");
            }
            int val = item->valueint;
            if (val < 0 || val > 100) {
                cJSON_Delete(root);
                return send_json_error(req, 400, "brightness must be 0-100");
            }
            esp_err_t err = settings_set_brightness((uint8_t)val);
            if (err != ESP_OK) {
                cJSON_Delete(root);
                return send_json_error(req, 500, "Failed to save brightness");
            }
            if (updated_count > 0) strlcat(updated_list, ",", sizeof(updated_list));
            strlcat(updated_list, "\"brightness\"", sizeof(updated_list));
            updated_count++;

        } else if (strcmp(key, "volume") == 0) {
            if (!cJSON_IsNumber(item)) {
                cJSON_Delete(root);
                return send_json_error(req, 400, "volume must be a number");
            }
            int val = item->valueint;
            if (val < 0 || val > 100) {
                cJSON_Delete(root);
                return send_json_error(req, 400, "volume must be 0-100");
            }
            esp_err_t err = settings_set_volume((uint8_t)val);
            if (err != ESP_OK) {
                cJSON_Delete(root);
                return send_json_error(req, 500, "Failed to save volume");
            }
            if (updated_count > 0) strlcat(updated_list, ",", sizeof(updated_list));
            strlcat(updated_list, "\"volume\"", sizeof(updated_list));
            updated_count++;

        } else if (strcmp(key, "wifi_ssid") == 0) {
            if (!cJSON_IsString(item) || !item->valuestring) {
                cJSON_Delete(root);
                return send_json_error(req, 400, "wifi_ssid must be a string");
            }
            size_t len = strlen(item->valuestring);
            if (len == 0 || len > 32) {
                cJSON_Delete(root);
                return send_json_error(req, 400, "wifi_ssid must be 1-32 characters");
            }
            /* Write to NVS "wifi" namespace */
            nvs_handle_t nvs;
            esp_err_t err = nvs_open("wifi", NVS_READWRITE, &nvs);
            if (err != ESP_OK) {
                cJSON_Delete(root);
                return send_json_error(req, 500, "Failed to open NVS");
            }
            err = nvs_set_str(nvs, "ssid", item->valuestring);
            if (err != ESP_OK) {
                nvs_close(nvs);
                cJSON_Delete(root);
                return send_json_error(req, 500, "Failed to save wifi_ssid");
            }
            nvs_commit(nvs);
            nvs_close(nvs);
            if (updated_count > 0) strlcat(updated_list, ",", sizeof(updated_list));
            strlcat(updated_list, "\"wifi_ssid\"", sizeof(updated_list));
            updated_count++;

        } else if (strcmp(key, "wifi_pass") == 0) {
            if (!cJSON_IsString(item) || !item->valuestring) {
                cJSON_Delete(root);
                return send_json_error(req, 400, "wifi_pass must be a string");
            }
            size_t len = strlen(item->valuestring);
            if (len < 8 || len > 63) {
                cJSON_Delete(root);
                return send_json_error(req, 400, "wifi_pass must be 8-63 characters");
            }
            /* Write to NVS "wifi" namespace */
            nvs_handle_t nvs;
            esp_err_t err = nvs_open("wifi", NVS_READWRITE, &nvs);
            if (err != ESP_OK) {
                cJSON_Delete(root);
                return send_json_error(req, 500, "Failed to open NVS");
            }
            err = nvs_set_str(nvs, "pass", item->valuestring);
            if (err != ESP_OK) {
                nvs_close(nvs);
                cJSON_Delete(root);
                return send_json_error(req, 500, "Failed to save wifi_pass");
            }
            nvs_commit(nvs);
            nvs_close(nvs);
            if (updated_count > 0) strlcat(updated_list, ",", sizeof(updated_list));
            strlcat(updated_list, "\"wifi_pass\"", sizeof(updated_list));
            updated_count++;

        } else if (strcmp(key, "ap_ssid") == 0) {
            if (!cJSON_IsString(item) || !item->valuestring) {
                cJSON_Delete(root);
                return send_json_error(req, 400, "ap_ssid must be a string");
            }
            size_t len = strlen(item->valuestring);
            if (len == 0 || len > 32) {
                cJSON_Delete(root);
                return send_json_error(req, 400, "ap_ssid must be 1-32 characters");
            }
            /* We need ap_pass too for set_ap_credentials, or just write directly to NVS */
            nvs_handle_t nvs;
            esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
            if (err != ESP_OK) {
                cJSON_Delete(root);
                return send_json_error(req, 500, "Failed to open NVS");
            }
            err = nvs_set_str(nvs, SETTINGS_KEY_AP_SSID, item->valuestring);
            if (err != ESP_OK) {
                nvs_close(nvs);
                cJSON_Delete(root);
                return send_json_error(req, 500, "Failed to save ap_ssid");
            }
            nvs_commit(nvs);
            nvs_close(nvs);
            if (updated_count > 0) strlcat(updated_list, ",", sizeof(updated_list));
            strlcat(updated_list, "\"ap_ssid\"", sizeof(updated_list));
            updated_count++;

        } else if (strcmp(key, "ap_pass") == 0) {
            if (!cJSON_IsString(item) || !item->valuestring) {
                cJSON_Delete(root);
                return send_json_error(req, 400, "ap_pass must be a string");
            }
            size_t len = strlen(item->valuestring);
            if (len < 8 || len > 63) {
                cJSON_Delete(root);
                return send_json_error(req, 400, "ap_pass must be 8-63 characters");
            }
            nvs_handle_t nvs;
            esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
            if (err != ESP_OK) {
                cJSON_Delete(root);
                return send_json_error(req, 500, "Failed to open NVS");
            }
            err = nvs_set_str(nvs, SETTINGS_KEY_AP_PASS, item->valuestring);
            if (err != ESP_OK) {
                nvs_close(nvs);
                cJSON_Delete(root);
                return send_json_error(req, 500, "Failed to save ap_pass");
            }
            nvs_commit(nvs);
            nvs_close(nvs);
            if (updated_count > 0) strlcat(updated_list, ",", sizeof(updated_list));
            strlcat(updated_list, "\"ap_pass\"", sizeof(updated_list));
            updated_count++;

        } else {
            /* Unknown key — add to warnings */
            if (warnings_count > 0) strlcat(warnings_list, ",", sizeof(warnings_list));
            strlcat(warnings_list, "\"ignored unknown key: ", sizeof(warnings_list));
            strlcat(warnings_list, key, sizeof(warnings_list));
            strlcat(warnings_list, "\"", sizeof(warnings_list));
            warnings_count++;
        }
    }

    cJSON_Delete(root);

    /* Build success response */
    char *buf = web_server_get_json_buf();
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No buffer");
        return ESP_FAIL;
    }

    snprintf(buf, web_server_get_json_buf_size(),
             "{\"status\":\"ok\",\"updated\":[%s],\"warnings\":[%s]}",
             updated_list, warnings_list);

    set_cors_header(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ======================================================================== */
/* POST "/api/password" — change admin password                              */
/* ======================================================================== */
esp_err_t handler_post_password(httpd_req_t *req)
{
    ESP_LOGD(TAG, "POST /api/password");

    /* Check Basic Auth */
    if (!check_basic_auth(req)) {
        return ESP_OK; /* 401 already sent */
    }

    /* Read request body */
    char body[256];
    int content_len = req->content_len;
    if (content_len <= 0 || content_len >= (int)sizeof(body)) {
        return send_json_error(req, 400, "Request body too large or empty");
    }

    int received = httpd_req_recv(req, body, content_len);
    if (received <= 0) {
        return send_json_error(req, 400, "Failed to read request body");
    }
    body[received] = '\0';

    /* Parse JSON */
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return send_json_error(req, 400, "Invalid JSON");
    }

    cJSON *current_pw = cJSON_GetObjectItem(root, "current_password");
    cJSON *new_pw = cJSON_GetObjectItem(root, "new_password");

    if (!cJSON_IsString(current_pw) || !current_pw->valuestring ||
        !cJSON_IsString(new_pw) || !new_pw->valuestring) {
        cJSON_Delete(root);
        return send_json_error(req, 400, "current_password and new_password are required strings");
    }

    esp_err_t err = settings_change_admin_pass(current_pw->valuestring, new_pw->valuestring);
    cJSON_Delete(root);

    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, 403, "Current password incorrect");
    } else if (err == ESP_ERR_INVALID_ARG) {
        return send_json_error(req, 400, "New password must be 8-32 characters");
    } else if (err != ESP_OK) {
        return send_json_error(req, 500, "Failed to save password");
    }

    /* Success */
    set_cors_header(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Password changed\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
