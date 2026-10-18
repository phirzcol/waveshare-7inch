/**
 * @file web_file_handlers.c
 * @brief HTTP handlers for SD card file transfer (list, download, upload).
 *
 * Endpoints:
 *   GET  /api/files?path=/sdcard       — list directory contents as JSON
 *   GET  /api/files/download?path=...  — download a file
 *   POST /api/files/upload?path=...    — upload a file (multipart or raw body)
 *   POST /api/files/mkdir?path=...     — create a directory
 *   POST /api/files/delete?path=...    — delete a file
 */

#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

#include "web_server.h"

static const char *TAG = "WEB_FILE";

/* 4KB chunk buffer for file transfers — allocated from PSRAM on first use */
#define FILE_BUF_SIZE 4096

/* Helper: URL-decode a string in-place */
static void url_decode(char *str)
{
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Helper: extract query parameter value */
static bool get_query_param(httpd_req_t *req, const char *key, char *val, size_t val_size)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen >= 512) return false;

    char query[512];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    if (httpd_query_key_value(query, key, val, val_size) != ESP_OK) return false;
    url_decode(val);
    return true;
}

/* Helper: set CORS header */
static inline void set_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

/* ======================================================================== */
/* GET /api/files — list directory                                           */
/* ======================================================================== */
esp_err_t handler_get_files(httpd_req_t *req)
{
    char path[256] = "/sdcard";
    get_query_param(req, "path", path, sizeof(path));

    /* Security: must start with /sdcard */
    if (strncmp(path, "/sdcard", 7) != 0) {
        set_cors(req);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Access denied\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        set_cors(req);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Directory not found\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    /* Stream JSON array */
    httpd_resp_sendstr_chunk(req, "{\"path\":\"");
    httpd_resp_sendstr_chunk(req, path);
    httpd_resp_sendstr_chunk(req, "\",\"files\":[");

    struct dirent *ent;
    bool first = true;
    char entry_buf[384];

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        /* Get file info */
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);

        struct stat st;
        bool is_dir = false;
        long fsize = 0;
        if (stat(full_path, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            fsize = is_dir ? 0 : (long)st.st_size;
        }

        if (!first) httpd_resp_sendstr_chunk(req, ",");
        first = false;

        snprintf(entry_buf, sizeof(entry_buf),
                 "{\"name\":\"%s\",\"dir\":%s,\"size\":%ld}",
                 ent->d_name, is_dir ? "true" : "false", fsize);
        httpd_resp_sendstr_chunk(req, entry_buf);
    }
    closedir(dir);

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL); /* End chunked response */
    return ESP_OK;
}

/* ======================================================================== */
/* GET /api/files/download — download a file                                 */
/* ======================================================================== */
esp_err_t handler_get_file_download(httpd_req_t *req)
{
    char path[256] = "";
    if (!get_query_param(req, "path", path, sizeof(path)) || path[0] == '\0') {
        set_cors(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path parameter");
        return ESP_OK;
    }

    /* Security: must start with /sdcard */
    if (strncmp(path, "/sdcard", 7) != 0) {
        set_cors(req);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_send(req, "Access denied", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        set_cors(req);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    set_cors(req);
    httpd_resp_set_type(req, "application/octet-stream");

    /* Set Content-Disposition with filename */
    const char *fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;
    char disp[128];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    /* Stream file in chunks */
    char *buf = heap_caps_malloc(FILE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Download: %s (%ld bytes)", path, fsize);

    size_t remaining = (size_t)fsize;
    while (remaining > 0) {
        size_t to_read = remaining > FILE_BUF_SIZE ? FILE_BUF_SIZE : remaining;
        size_t read = fread(buf, 1, to_read, f);
        if (read == 0) break;

        if (httpd_resp_send_chunk(req, buf, read) != ESP_OK) {
            ESP_LOGE(TAG, "Download send failed");
            break;
        }
        remaining -= read;
    }

    fclose(f);
    heap_caps_free(buf);
    httpd_resp_send_chunk(req, NULL, 0); /* End chunked */
    return ESP_OK;
}

/* ======================================================================== */
/* POST /api/files/upload — upload a file (raw body)                         */
/* ======================================================================== */
esp_err_t handler_post_file_upload(httpd_req_t *req)
{
    char path[256] = "";
    if (!get_query_param(req, "path", path, sizeof(path)) || path[0] == '\0') {
        set_cors(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path parameter");
        return ESP_OK;
    }

    /* Security: must start with /sdcard */
    if (strncmp(path, "/sdcard", 7) != 0) {
        set_cors(req);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Access denied\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        set_cors(req);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
        return ESP_OK;
    }

    char *buf = heap_caps_malloc(FILE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_OK;
    }

    int content_len = req->content_len;
    int received_total = 0;

    ESP_LOGI(TAG, "Upload: %s (%d bytes)", path, content_len);

    while (received_total < content_len) {
        int to_recv = content_len - received_total;
        if (to_recv > FILE_BUF_SIZE) to_recv = FILE_BUF_SIZE;

        int received = httpd_req_recv(req, buf, to_recv);
        if (received <= 0) {
            ESP_LOGE(TAG, "Upload recv failed at %d/%d", received_total, content_len);
            break;
        }

        fwrite(buf, 1, received, f);
        received_total += received;
    }

    fclose(f);
    heap_caps_free(buf);

    set_cors(req);
    httpd_resp_set_type(req, "application/json");
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"path\":\"%s\",\"size\":%d}", path, received_total);
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ======================================================================== */
/* POST /api/files/mkdir — create directory                                   */
/* ======================================================================== */
esp_err_t handler_post_file_mkdir(httpd_req_t *req)
{
    char path[256] = "";
    if (!get_query_param(req, "path", path, sizeof(path)) || path[0] == '\0') {
        set_cors(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path parameter");
        return ESP_OK;
    }

    if (strncmp(path, "/sdcard", 7) != 0) {
        set_cors(req);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Access denied\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int ret = mkdir(path, 0775);
    set_cors(req);
    httpd_resp_set_type(req, "application/json");
    if (ret == 0) {
        httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"error\":\"Failed to create directory\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

/* ======================================================================== */
/* POST /api/files/delete — delete a file                                    */
/* ======================================================================== */
esp_err_t handler_post_file_delete(httpd_req_t *req)
{
    char path[256] = "";
    if (!get_query_param(req, "path", path, sizeof(path)) || path[0] == '\0') {
        set_cors(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path parameter");
        return ESP_OK;
    }

    if (strncmp(path, "/sdcard", 7) != 0) {
        set_cors(req);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Access denied\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int ret = unlink(path);
    set_cors(req);
    httpd_resp_set_type(req, "application/json");
    if (ret == 0) {
        httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"error\":\"Failed to delete file\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}
