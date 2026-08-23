#include "wifi_config_http.h"
#include "app_config.h"
#include "app_storage.h"
#include "file_manager.h"
#include "ota_manager.h"
#include "web_platform.h"
#include "wifi_config_store.h"
#include "wifi_manager.h"

#include "json_http.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"

#define HTTP_JSON_BUFFER_BYTES 512u

static const char *TAG = "WIFI_WEB";

/* ── 私有文件策略（应用层所有，平台执行） ─────────────────────────────── */

static const char *protect_wifi_config(const char *fs_type, const char *path)
{
    if (fs_type != NULL && strcmp(fs_type, "internal") == 0 &&
        wifi_config_store_is_path(path)) {
        return "WiFi configuration is application-private";
    }
    return NULL;
}

/* ── GET /network.json ─────────────────────────────────────────────────── */
static esp_err_t network_json_handler(httpd_req_t *req)
{
    wifi_snapshot_t snap;
    wifi_manager_get_snapshot(&snap);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "sta_connected", snap.sta_connected);
    cJSON_AddStringToObject(root, "sta_ssid", snap.sta_ssid);
    cJSON_AddStringToObject(root, "sta_ip", snap.sta_connected ? snap.sta_ip : "0.0.0.0");
    cJSON_AddStringToObject(root, "ap_ssid", wifi_manager_get_ap_ssid());
    cJSON_AddStringToObject(root, "ap_password", wifi_manager_get_ap_password());
    cJSON_AddStringToObject(root, "ap_ip", snap.ap_ip);
    cJSON_AddStringToObject(root, "config_path", wifi_config_store_get_path());
    cJSON_AddBoolToObject(root, "config_exists", wifi_config_store_exists());

    const esp_app_desc_t *app_desc = esp_app_get_description();
    char build_ts[32];
    snprintf(build_ts, sizeof(build_ts), "%s %s", app_desc->date, app_desc->time);
    cJSON_AddStringToObject(root, "app_build_id", APP_BUILD_ID);
    cJSON_AddStringToObject(root, "firmware_sha256", esp_app_get_elf_sha256_str());
    cJSON_AddStringToObject(root, "build_timestamp", build_ts);
    cJSON_AddStringToObject(root, "idf_version", app_desc->idf_ver);
    return send_json_object(req, root);
}

/* ── GET /wifi_config.json ─────────────────────────────────────────────── */
static esp_err_t wifi_config_get_handler(httpd_req_t *req)
{
    wifi_snapshot_t snap;
    wifi_manager_get_snapshot(&snap);

    /* Load the persisted config so the page can prefill the settings;
     * fall back to the runtime snapshot (empty fields) when the filesystem
     * is busy or the file is missing/corrupt. */
    wifi_persisted_config_t saved = {0};
    bool have_saved = false;
    if (app_storage_try_acquire() == ESP_OK) {
        have_saved = wifi_config_store_load_full(&saved) == ESP_OK;
        app_storage_release();
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "ssid", have_saved ? saved.sta.sta_ssid : snap.sta_ssid);
    cJSON_AddBoolToObject(root, "has_password", snap.has_password);
    cJSON_AddStringToObject(root, "ip_mode",
                            have_saved && saved.sta.ip_static ? "static" : "dhcp");
    cJSON_AddStringToObject(root, "static_ip", have_saved ? saved.sta.ip_addr : "");
    cJSON_AddStringToObject(root, "netmask", have_saved ? saved.sta.ip_netmask : "");
    cJSON_AddStringToObject(root, "gateway", have_saved ? saved.sta.ip_gateway : "");
    cJSON_AddStringToObject(root, "dns", have_saved ? saved.sta.ip_dns : "");
    cJSON_AddStringToObject(root, "ap_ssid",
                            have_saved && saved.ap_ssid[0] != '\0'
                                ? saved.ap_ssid : wifi_manager_get_ap_ssid());
    cJSON_AddStringToObject(root, "ap_password",
                            have_saved && saved.ap_ssid[0] != '\0'
                                ? saved.ap_password : wifi_manager_get_ap_password());
    cJSON_AddStringToObject(root, "path", wifi_config_store_get_path());
    cJSON_AddBoolToObject(root, "config_exists", wifi_config_store_exists());
    return send_json_object(req, root);
}

/* ── POST /wifi_config.json ────────────────────────────────────────────── */
static esp_err_t wifi_config_post_handler(httpd_req_t *req)
{
    if (ota_manager_is_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req,
                                  "cannot update WiFi configuration during OTA");
    }

    char body[HTTP_JSON_BUFFER_BYTES];
    if (receive_json_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    /* Partial update: load the persisted config as the base so a request can
     * change only the WiFi credentials, IP policy, or AP identity without
     * touching the others, and without wiping a previously saved password. */
    wifi_persisted_config_t config = {0};
    if (app_storage_try_acquire() == ESP_OK) {
        (void)wifi_config_store_load_full(&config);
        app_storage_release();
    }

    cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(root, "password");
    cJSON *ip_mode_item = cJSON_GetObjectItemCaseSensitive(root, "ip_mode");
    cJSON *ap_ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ap_ssid");
    cJSON *ap_pass_item = cJSON_GetObjectItemCaseSensitive(root, "ap_password");
    const bool have_ssid = cJSON_IsString(ssid_item) &&
                           ssid_item->valuestring != NULL &&
                           ssid_item->valuestring[0] != '\0';
    const bool have_ip_mode = cJSON_IsString(ip_mode_item);
    const bool have_ap_ssid = cJSON_IsString(ap_ssid_item) &&
                              ap_ssid_item->valuestring != NULL &&
                              ap_ssid_item->valuestring[0] != '\0';
    const bool updated_wifi = have_ssid;
    const bool updated_ap = have_ap_ssid;
    if (!have_ssid && !have_ip_mode && !have_ap_ssid) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "provide ssid/password, ip_mode, and/or ap_ssid");
        return ESP_FAIL;
    }

    bool valid = true;
    if (have_ssid) {
        const size_t ssid_len = strlen(ssid_item->valuestring);
        if (ssid_len > WIFI_MANAGER_SSID_MAX_BYTES) {
            valid = false;
        } else {
            memcpy(config.sta.sta_ssid, ssid_item->valuestring, ssid_len + 1u);
            if (pass_item != NULL && cJSON_IsString(pass_item) &&
                pass_item->valuestring != NULL && pass_item->valuestring[0] != '\0') {
                const size_t pass_len = strlen(pass_item->valuestring);
                if (pass_len > WIFI_MANAGER_PASSWORD_MAX_BYTES) {
                    valid = false;
                } else {
                    memcpy(config.sta.sta_password, pass_item->valuestring,
                           pass_len + 1u);
                }
            }
            /* empty password keeps any previously saved one */
        }
    }

    if (have_ip_mode) {
        if (strcmp(ip_mode_item->valuestring, "static") == 0) {
            config.sta.ip_static = true;
            const char *const keys[4] = { "static_ip", "netmask", "gateway", "dns" };
            char *const fields[4] = { config.sta.ip_addr,
                                      config.sta.ip_netmask,
                                      config.sta.ip_gateway,
                                      config.sta.ip_dns };
            for (int i = 0; i < 4; ++i) {
                cJSON *item = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
                if (!cJSON_IsString(item) || item->valuestring == NULL ||
                    item->valuestring[0] == '\0' ||
                    strlen(item->valuestring) > WIFI_MANAGER_IP_MAX_BYTES ||
                    !wifi_manager_ipv4_is_valid(item->valuestring)) {
                    valid = false;
                    break;
                }
                memcpy(fields[i], item->valuestring, strlen(item->valuestring) + 1u);
            }
        } else if (strcmp(ip_mode_item->valuestring, "dhcp") == 0) {
            /* Keep the previous static values so switching back to static
             * restores them; they are ignored while ip_static is false. */
            config.sta.ip_static = false;
        } else {
            valid = false;
        }
    }

    if (have_ap_ssid) {
        const size_t ap_ssid_len = strlen(ap_ssid_item->valuestring);
        if (ap_ssid_len > WIFI_MANAGER_SSID_MAX_BYTES) {
            valid = false;
        } else {
            memcpy(config.ap_ssid, ap_ssid_item->valuestring, ap_ssid_len + 1u);
            if (ap_pass_item != NULL && cJSON_IsString(ap_pass_item) &&
                ap_pass_item->valuestring != NULL &&
                ap_pass_item->valuestring[0] != '\0') {
                const size_t ap_pass_len = strlen(ap_pass_item->valuestring);
                if (ap_pass_len > WIFI_MANAGER_PASSWORD_MAX_BYTES ||
                    ap_pass_len < 8u) {
                    valid = false;
                } else {
                    memcpy(config.ap_password, ap_pass_item->valuestring,
                           ap_pass_len + 1u);
                }
            }
            /* empty AP password keeps any previously saved one */
        }
    }

    cJSON_Delete(root);

    if (!valid || config.sta.sta_ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            config.sta.sta_ssid[0] == '\0'
                                ? "no saved WiFi config; provide ssid first"
                                : "invalid settings");
        return ESP_FAIL;
    }

    if (app_storage_try_acquire() != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req,
                                  "filesystem update is already in progress");
    }
    if (ota_manager_is_busy()) {
        app_storage_release();
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req,
                                  "cannot update WiFi configuration during OTA");
    }

    /* The transactional stage → apply → commit → rollback lives in the store. */
    esp_err_t err = wifi_config_store_apply_full(&config);
    app_storage_release();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "apply WiFi config failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "failed to save wifi config");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "ssid", config.sta.sta_ssid);
    cJSON_AddStringToObject(resp, "path", wifi_config_store_get_path());
    cJSON_AddStringToObject(resp, "message",
                            updated_wifi ? "saved; reconnecting STA"
                            : (updated_ap ? "AP identity saved"
                                          : "IP settings saved; reconnecting STA"));
    /* The address the STA will move to after the deferred apply, so the page
     * can redirect there; empty for DHCP (address unknown until renewal). */
    cJSON_AddStringToObject(resp, "new_ip",
                            config.sta.ip_static ? config.sta.ip_addr : "");
    return send_json_object(req, resp);
}

/* ── 注册 ──────────────────────────────────────────────────────────────── */

/* 私有文件保护策略：必须在 web_platform_init() 之前安装，确保 HTTP 服务器
 * 从接受第一个请求起就带着防护（文件管理器读写守卫 + 静态回退策略）。 */
esp_err_t wifi_config_http_install_guards(void)
{
    file_manager_set_read_guard(protect_wifi_config);
    file_manager_set_mutation_guard(protect_wifi_config);
    web_platform_set_private_path_cb(wifi_config_store_is_path);
    return ESP_OK;
}

esp_err_t wifi_config_http_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* clang-format off */
    httpd_uri_t net_uri   = { .uri = "/network.json",     .method = HTTP_GET,  .handler = network_json_handler };
    httpd_uri_t wcfg_g_uri= { .uri = "/wifi_config.json", .method = HTTP_GET,  .handler = wifi_config_get_handler };
    httpd_uri_t wcfg_p_uri= { .uri = "/wifi_config.json", .method = HTTP_POST, .handler = wifi_config_post_handler };
    /* clang-format on */

    esp_err_t reg_err;
    if ((reg_err = httpd_register_uri_handler(server, &net_uri))    != ESP_OK ||
        (reg_err = httpd_register_uri_handler(server, &wcfg_g_uri)) != ESP_OK ||
        (reg_err = httpd_register_uri_handler(server, &wcfg_p_uri)) != ESP_OK) {
        ESP_LOGE(TAG, "URI handler registration failed: %s", esp_err_to_name(reg_err));
        return reg_err;
    }
    ESP_LOGI(TAG, "WiFi config endpoints registered (/network.json, /wifi_config.json)");
    return ESP_OK;
}
