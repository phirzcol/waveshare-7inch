/**
 * @file web_server.c
 * @brief Embedded HTTP server lifecycle management.
 *
 * Owns the httpd_handle_t lifecycle. Allocates all buffers from PSRAM.
 * Start/stop are idempotent and called by the WiFi event handler.
 */

#include "web_server.h"

#include <string.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

static const char *TAG = "WEB_SRV";

/* Server handle — NULL when stopped */
static httpd_handle_t s_server = NULL;
static bool s_stopping = false;

/* PSRAM scratch buffer for JSON response building (2KB) */
static char *s_json_buf = NULL;

#define JSON_BUF_SIZE 2048

/* ---- Extern URI handler functions (implemented in web_api_handlers.c) ---- */
extern esp_err_t handler_get_root(httpd_req_t *req);
extern esp_err_t handler_get_settings(httpd_req_t *req);
extern esp_err_t handler_post_settings(httpd_req_t *req);
extern esp_err_t handler_get_status(httpd_req_t *req);
extern esp_err_t handler_post_password(httpd_req_t *req);

/* ---- Extern URI handler functions (implemented in web_file_handlers.c) ---- */
extern esp_err_t handler_get_files(httpd_req_t *req);
extern esp_err_t handler_get_file_download(httpd_req_t *req);
extern esp_err_t handler_post_file_upload(httpd_req_t *req);
extern esp_err_t handler_post_file_mkdir(httpd_req_t *req);
extern esp_err_t handler_post_file_delete(httpd_req_t *req);

/* ---- URI definitions ---- */

static const httpd_uri_t uri_get_root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = handler_get_root,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_get_settings = {
    .uri       = "/api/settings",
    .method    = HTTP_GET,
    .handler   = handler_get_settings,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_post_settings = {
    .uri       = "/api/settings",
    .method    = HTTP_POST,
    .handler   = handler_post_settings,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_get_status = {
    .uri       = "/api/status",
    .method    = HTTP_GET,
    .handler   = handler_get_status,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_post_password = {
    .uri       = "/api/password",
    .method    = HTTP_POST,
    .handler   = handler_post_password,
    .user_ctx  = NULL
};

/* ---- File transfer URI definitions ---- */

static const httpd_uri_t uri_get_files = {
    .uri       = "/api/files",
    .method    = HTTP_GET,
    .handler   = handler_get_files,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_get_file_download = {
    .uri       = "/api/files/download",
    .method    = HTTP_GET,
    .handler   = handler_get_file_download,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_post_file_upload = {
    .uri       = "/api/files/upload",
    .method    = HTTP_POST,
    .handler   = handler_post_file_upload,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_post_file_mkdir = {
    .uri       = "/api/files/mkdir",
    .method    = HTTP_POST,
    .handler   = handler_post_file_mkdir,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_post_file_delete = {
    .uri       = "/api/files/delete",
    .method    = HTTP_POST,
    .handler   = handler_post_file_delete,
    .user_ctx  = NULL
};

/* ---- Public API ---- */

esp_err_t web_server_start(void)
{
    /* Idempotent: no-op if already running */
    if (s_server != NULL) {
        ESP_LOGD(TAG, "Server already running");
        return ESP_OK;
    }
    s_stopping = false;

    /* Allocate PSRAM JSON scratch buffer */
    s_json_buf = (char *)heap_caps_malloc(JSON_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (s_json_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate JSON buffer from PSRAM");
        return ESP_ERR_NO_MEM;
    }

    /* Configure httpd */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets  = 3;
    config.max_uri_handlers  = 12;
    config.task_priority     = 3;
    config.core_id           = 0;
    config.stack_size        = 8192;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    /* Start the server */
    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        /* Free partial allocations */
        heap_caps_free(s_json_buf);
        s_json_buf = NULL;
        s_server = NULL;
        return ret;
    }

    /* Register URI handlers */
    httpd_register_uri_handler(s_server, &uri_get_root);
    httpd_register_uri_handler(s_server, &uri_get_settings);
    httpd_register_uri_handler(s_server, &uri_post_settings);
    httpd_register_uri_handler(s_server, &uri_get_status);
    httpd_register_uri_handler(s_server, &uri_post_password);
    httpd_register_uri_handler(s_server, &uri_get_files);
    httpd_register_uri_handler(s_server, &uri_get_file_download);
    httpd_register_uri_handler(s_server, &uri_post_file_upload);
    httpd_register_uri_handler(s_server, &uri_post_file_mkdir);
    httpd_register_uri_handler(s_server, &uri_post_file_delete);

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    /* Idempotent: no-op if not running */
    if (s_server == NULL) {
        ESP_LOGD(TAG, "Server already stopped");
        return ESP_OK;
    }
    if (s_stopping) {
        ESP_LOGD(TAG, "Server stop already in progress");
        return ESP_OK;
    }

    /* Stop the httpd instance */
    s_stopping = true;
    esp_err_t ret = httpd_stop(s_server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_stop failed: %s", esp_err_to_name(ret));
        s_stopping = false;
        return ret;
    }

    s_server = NULL;
    s_stopping = false;

    /* Free PSRAM buffers */
    if (s_json_buf != NULL) {
        heap_caps_free(s_json_buf);
        s_json_buf = NULL;
    }

    ESP_LOGI(TAG, "Web server stopped");
    return ret;
}

bool web_server_is_running(void)
{
    return (s_server != NULL);
}

/* ---- Accessor for shared JSON buffer (used by handlers) ---- */

char *web_server_get_json_buf(void)
{
    return s_json_buf;
}

size_t web_server_get_json_buf_size(void)
{
    return JSON_BUF_SIZE;
}
