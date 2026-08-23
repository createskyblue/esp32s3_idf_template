#include "web_platform.h"
#include "file_manager.h"
#include "ota_manager.h"
#include "app_storage.h"
#include "sd_card.h"

#include "json_http.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── constants ─────────────────────────────────────────────────────────── */
#define LITTLEFS_INDEX_PATH          APP_LITTLEFS_BASE_PATH "/index.html"
#define HTTP_FILE_BUFFER_BYTES       1024u

static const char *TAG = "WEB_PLATFORM";
static const char FILESYSTEM_RECOVERY_HTML[] =
    "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>文件系统恢复</title><body><h1>文件系统不可用</h1>"
    "<p>设备已进入 AP + OTA 恢复模式。请选择有效的 LittleFS 镜像重新上传。</p>"
    "<input id=\"image\" type=\"file\"><button onclick=\"recover()\">上传并恢复</button>"
    "<pre id=\"status\"></pre><script>async function recover(){const f=image.files[0];"
    "if(!f){status.textContent='请选择镜像';return;}status.textContent='上传中…';"
    "try{const r=await fetch('/ota/upload/filesystem',{method:'POST',body:f});"
    "status.textContent=await r.text();}catch(e){status.textContent=String(e);}}</script>"
    "</body></html>";

/* ── HTTP server handle ────────────────────────────────────────────────── */
static httpd_handle_t s_http_server;
static esp_timer_handle_t s_reboot_timer;
static web_platform_private_path_cb_t s_private_path_cb;
static bool s_private_path_cb_installed;

void web_platform_set_private_path_cb(web_platform_private_path_cb_t cb)
{
    s_private_path_cb = cb;
    s_private_path_cb_installed = true;
}

static bool resolve_static_path(const char *uri, char *path, size_t path_size)
{
    if (uri == NULL || path == NULL || path_size == 0u || uri[0] != '/') {
        return false;
    }

    const size_t uri_length = strcspn(uri, "?#");
    if (uri_length <= 1u) return false;

    size_t segment_start = 1u;
    for (size_t i = 1u; i <= uri_length; ++i) {
        if (i != uri_length && uri[i] != '/') continue;

        const size_t segment_length = i - segment_start;
        if (segment_length == 0u ||
            (segment_length == 1u && uri[segment_start] == '.') ||
            (segment_length == 2u && uri[segment_start] == '.' &&
             uri[segment_start + 1u] == '.') ||
            memchr(uri + segment_start, '\\', segment_length) != NULL) {
            return false;
        }
        segment_start = i + 1u;
    }

    const int length = snprintf(path, path_size, "%s%.*s",
                                APP_LITTLEFS_BASE_PATH, (int)uri_length, uri);
    return length >= 0 && (size_t)length < path_size;
}

/* Public helpers kept for custom handlers (see hello_web); delegate to the
 * shared json_http helpers so behavior stays consistent across components. */
esp_err_t send_json_text(httpd_req_t *req, const char *json)
{
    return json_send_text(req, json);
}

esp_err_t send_json_object(httpd_req_t *req, cJSON *root)
{
    return json_send_object(req, root);
}

esp_err_t receive_json_body(httpd_req_t *req, char *buffer, size_t buffer_size)
{
    return json_receive_body(req, buffer, buffer_size);
}

/* ══════════════════════════════════════════════════════════════════════════
 * HTTP handlers
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── GET / ─────────────────────────────────────────────────────────────── */
static esp_err_t root_handler(httpd_req_t *req)
{
    if (app_storage_try_acquire() != ESP_OK) {
        if (ota_manager_is_busy()) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            return httpd_resp_sendstr(req, "filesystem OTA is in progress");
        }
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
        return httpd_resp_sendstr(req, FILESYSTEM_RECOVERY_HTML);
    }

    FILE *file = fopen(LITTLEFS_INDEX_PATH, "r");
    if (file == NULL) {
        app_storage_release();
        ESP_LOGE(TAG, "failed to open %s", LITTLEFS_INDEX_PATH);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "index.html not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");

    char buffer[HTTP_FILE_BUFFER_BYTES];
    size_t read_bytes;
    esp_err_t err = ESP_OK;
    while ((read_bytes = fread(buffer, 1u, sizeof(buffer), file)) > 0u) {
        err = httpd_resp_send_chunk(req, buffer, read_bytes);
        if (err != ESP_OK) break;
    }
    fclose(file);
    app_storage_release();
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

/* ── GET catch-all  (static file fallback) ────────────────────────────── */
static esp_err_t littlefs_static_handler(httpd_req_t *req)
{
    char path[576];
    if (!resolve_static_path(req->uri, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid static path");
        return ESP_FAIL;
    }
    if (s_private_path_cb != NULL && s_private_path_cb(path)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_FAIL;
    }

    if (app_storage_try_acquire() != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "filesystem is temporarily unavailable");
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        app_storage_release();
        /* Redirect unknown paths to / for captive portal detection */
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    const char *type = "application/octet-stream";
    const char *ext = strrchr(path, '.');
    if (ext) {
        if (strcasecmp(ext, ".html") == 0)      type = "text/html; charset=utf-8";
        else if (strcasecmp(ext, ".js") == 0)   type = "application/javascript";
        else if (strcasecmp(ext, ".css") == 0)  type = "text/css";
        else if (strcasecmp(ext, ".json") == 0) type = "application/json";
        else if (strcasecmp(ext, ".svg") == 0)  type = "image/svg+xml";
        else if (strcasecmp(ext, ".png") == 0)  type = "image/png";
        else if (strcasecmp(ext, ".ico") == 0)  type = "image/x-icon";
    }
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");

    char buffer[HTTP_FILE_BUFFER_BYTES];
    size_t read_bytes;
    esp_err_t err = ESP_OK;
    while ((read_bytes = fread(buffer, 1u, sizeof(buffer), file)) > 0u) {
        err = httpd_resp_send_chunk(req, buffer, read_bytes);
        if (err != ESP_OK) break;
    }
    fclose(file);
    app_storage_release();
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

/* ── GET /debug.json ───────────────────────────────────────────────────── */
static esp_err_t debug_json_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }

    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "largest_free_block",
                            heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t internal_total = heap_caps_get_total_size(internal_caps);
    const size_t internal_free = heap_caps_get_free_size(internal_caps);
    const size_t internal_used = internal_total >= internal_free
                                     ? internal_total - internal_free
                                     : 0u;
    cJSON_AddNumberToObject(root, "internal_total_heap", internal_total);
    cJSON_AddNumberToObject(root, "internal_used_heap", internal_used);
    cJSON_AddNumberToObject(root, "internal_free_heap", internal_free);
    cJSON_AddNumberToObject(root, "internal_min_free_heap",
                            heap_caps_get_minimum_free_size(internal_caps));
    cJSON_AddNumberToObject(root, "internal_largest_free_block",
                            heap_caps_get_largest_free_block(internal_caps));
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const size_t psram_total = heap_caps_get_total_size(psram_caps);
    const size_t psram_free = heap_caps_get_free_size(psram_caps);
    const size_t psram_used = psram_total >= psram_free
                                  ? psram_total - psram_free
                                  : 0u;
    cJSON_AddNumberToObject(root, "psram_total_heap", psram_total);
    cJSON_AddNumberToObject(root, "psram_used_heap", psram_used);
    cJSON_AddNumberToObject(root, "psram_free_heap", psram_free);

#if configUSE_TRACE_FACILITY && configUSE_STATS_FORMATTING_FUNCTIONS
    char *task_buf = malloc(2048);
    if (task_buf != NULL) {
        int hdr = snprintf(task_buf, 2048,
                           "名称            状态  优先级  栈剩余  序号\r\n"
                           "------------------------------------------------\r\n");
        if (hdr > 0 && hdr < 2048) vTaskList(task_buf + hdr);
        cJSON_AddStringToObject(root, "task_list", task_buf);
        free(task_buf);
    }
#else
    cJSON_AddStringToObject(root, "task_list",
        "(需要启用 CONFIG_FREERTOS_USE_TRACE_FACILITY 和 CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS)");
#endif

    cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time() / 1000);
    return send_json_object(req, root);
}

/* ── POST /reboot ─────────────────────────────────────────────────────── */
static void reboot_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "scheduled reboot firing");
    esp_restart();
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (s_reboot_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = reboot_timer_cb,
            .name = "reboot_delay",
        };
        esp_err_t err = esp_timer_create(&args, &s_reboot_timer);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "cannot create reboot timer");
            return ESP_FAIL;
        }
    } else {
        (void)esp_timer_stop(s_reboot_timer);
    }

    esp_err_t err = esp_timer_start_once(s_reboot_timer, 3000ULL * 1000ULL);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "cannot schedule reboot");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "rebooting in 3 seconds");
    return send_json_object(req, resp);
}

/* ══════════════════════════════════════════════════════════════════════════
 * HTTP server setup
 * ══════════════════════════════════════════════════════════════════════════ */

static esp_err_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.stack_size = 16384;

    httpd_handle_t server = NULL;
    const esp_err_t httpd_err = httpd_start(&server, &config);
    if (httpd_err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(httpd_err));
        return httpd_err;
    }

    /* clang-format off */
    httpd_uri_t root_uri  = { .uri = "/",              .method = HTTP_GET,  .handler = root_handler };
    httpd_uri_t debug_uri = { .uri = "/debug.json",    .method = HTTP_GET,  .handler = debug_json_handler };
    httpd_uri_t reboot_uri= { .uri = "/reboot",        .method = HTTP_POST, .handler = reboot_handler };
    /* clang-format on */

    esp_err_t reg_err;
    if ((reg_err = httpd_register_uri_handler(server, &root_uri))   != ESP_OK ||
        (reg_err = httpd_register_uri_handler(server, &debug_uri))  != ESP_OK ||
        (reg_err = httpd_register_uri_handler(server, &reboot_uri)) != ESP_OK) {
        ESP_LOGE(TAG, "URI handler registration failed: %s", esp_err_to_name(reg_err));
        httpd_stop(server);
        return reg_err;
    }
    if ((reg_err = file_manager_register(server)) != ESP_OK) {
        ESP_LOGE(TAG, "file manager registration failed: %s", esp_err_to_name(reg_err));
        httpd_stop(server);
        return reg_err;
    }
    if ((reg_err = ota_manager_register(server)) != ESP_OK) {
        ESP_LOGE(TAG, "OTA handler registration failed: %s", esp_err_to_name(reg_err));
        httpd_stop(server);
        return reg_err;
    }

    /* Static file fallback (catch-all) is NOT registered here — callers must
     * invoke web_platform_register_static_fallback() LAST so exact URIs
     * match before the wildcard. */
    s_http_server = server;
    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

httpd_handle_t web_platform_get_server(void)
{
    return s_http_server;
}

esp_err_t web_platform_register_static_fallback(void)
{
    if (s_http_server == NULL) return ESP_ERR_INVALID_STATE;
    /* fail-fast：未安装私有路径策略时拒绝注册回退。否则静态回退会把应用
     * 私有文件（如 WiFi 凭据）一并对外提供，而模板复制场景容易静默丢失
     * 该保护——没有私有文件的应用显式调用 web_platform_set_private_path_cb(NULL)。 */
    if (!s_private_path_cb_installed) {
        ESP_LOGE(TAG, "no private-path policy installed; refusing to register the "
                      "static fallback (it would serve application-private files). "
                      "Call web_platform_set_private_path_cb() first.");
        return ESP_ERR_INVALID_STATE;
    }
    static const httpd_uri_t static_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = littlefs_static_handler,
    };
    esp_err_t err = httpd_register_uri_handler(s_http_server, &static_uri);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Static file fallback registered (/*)");
    return err;
}

esp_err_t web_platform_init(void)
{
    const file_manager_storage_config_t file_manager_config = {
        .internal_mount_point = APP_LITTLEFS_BASE_PATH,
        .internal_partition_label = APP_LITTLEFS_PARTITION_LABEL,
        .sd_mount_point = SD_CARD_DEFAULT_MOUNT_POINT,
    };
    const ota_manager_config_t ota_config = {
        .filesystem_partition_label = APP_LITTLEFS_PARTITION_LABEL,
        .filesystem_update_begin = app_storage_begin_update,
        .filesystem_update_end = app_storage_end_update,
    };
    ESP_ERROR_CHECK(file_manager_set_storage_config(&file_manager_config));
    ESP_ERROR_CHECK(ota_manager_init_with_config(&ota_config));
    file_manager_set_access_callbacks(app_storage_try_acquire,
                                      app_storage_release);
    /* 私有文件保护策略由应用层在平台 init 之前安装（见 wifi_config_http）。 */
    return start_webserver();
}
