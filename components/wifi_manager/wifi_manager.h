#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MANAGER_SSID_MAX_BYTES     32u
#define WIFI_MANAGER_PASSWORD_MAX_BYTES 64u
#define WIFI_MANAGER_SNTP_SERVER_MAX_BYTES 63u
#define WIFI_MANAGER_IP_MAX_BYTES       15u /* "255.255.255.255" */

/** Caller-owned STA credentials + IP policy used at startup and on updates.
 *  When ip_static is false the STA uses DHCP and the *_ip fields are ignored;
 *  when true they must be valid IPv4 literals. */
typedef struct {
    char sta_ssid[WIFI_MANAGER_SSID_MAX_BYTES + 1u];
    char sta_password[WIFI_MANAGER_PASSWORD_MAX_BYTES + 1u];
    bool ip_static;
    char ip_addr[WIFI_MANAGER_IP_MAX_BYTES + 1u];
    char ip_netmask[WIFI_MANAGER_IP_MAX_BYTES + 1u];
    char ip_gateway[WIFI_MANAGER_IP_MAX_BYTES + 1u];
    char ip_dns[WIFI_MANAGER_IP_MAX_BYTES + 1u];
} wifi_manager_credentials_t;

/** Caller-owned WiFi startup policy copied by wifi_manager_init(). */
typedef struct {
    wifi_manager_credentials_t sta;
    char ap_ssid[WIFI_MANAGER_SSID_MAX_BYTES + 1u];
    char ap_password[WIFI_MANAGER_PASSWORD_MAX_BYTES + 1u];
    uint8_t ap_channel;
    uint8_t ap_max_connections;
    bool captive_portal_dns_enabled;
    char sntp_server[WIFI_MANAGER_SNTP_SERVER_MAX_BYTES + 1u];
} wifi_manager_config_t;

/** Immutable snapshot of current WiFi state (for HTTP handlers). */
typedef struct {
    bool sta_connected;
    char sta_ssid[33];
    char sta_ip[16];
    char ap_ip[16];
    bool has_password;
} wifi_snapshot_t;

/**
 * Start APSTA using credentials supplied by the caller.
 * SoftAP is started first; if the initial STA update is rejected, the function
 * returns that error while leaving SoftAP available for provisioning.
 */
esp_err_t wifi_manager_init(const wifi_manager_config_t *config);

/** True after APSTA mode has started, including STA-config fallback cases. */
bool wifi_manager_is_started(void);

/** Fill a point-in-time snapshot of WiFi state. */
void wifi_manager_get_snapshot(wifi_snapshot_t *out);

/** Copy the currently active STA credentials into caller-owned storage. */
void wifi_manager_get_credentials(wifi_manager_credentials_t *out);

/**
 * Copy new credentials and trigger an immediate STA reconnect.
 * An empty SSID disables STA connection attempts while keeping SoftAP active.
 * If the driver rejects the update, the previous credentials are restored.
 */
esp_err_t wifi_manager_set_credentials(
    const wifi_manager_credentials_t *credentials);

/** Disable STA reconnects and leave the provisioning SoftAP active. */
esp_err_t wifi_manager_enter_provisioning_mode(void);

/**
 * 暂时挂起 STA 自动重连：停止退避定时器并中止正在进行的连接尝试，让出
 * 无线信道给外部 WiFi 扫描（如 BluFi 配网请求 WiFi 列表）。WiFi 驱动在
 * STA 处于 connecting 状态时拒绝并发扫描（"STA is connecting, scan are
 * not allowed"）。已连接时仅置挂起标志，不断开已建立的连接。
 * 扫描完成后必须调用 wifi_manager_resume_sta() 恢复。
 */
esp_err_t wifi_manager_suspend_sta(void);

/** 恢复 STA 自动重连：清挂起标志并立即发起一次连接尝试（失败走既有退避）。 */
esp_err_t wifi_manager_resume_sta(void);

/** SoftAP SSID exposed for status responses. */
const char *wifi_manager_get_ap_ssid(void);

/** SoftAP password exposed for status responses. */
const char *wifi_manager_get_ap_password(void);

/** True when str is a well-formed dotted-quad IPv4 address. */
bool wifi_manager_ipv4_is_valid(const char *str);

/**
 * Reconfigure the SoftAP identity (SSID + password) at runtime; applied
 * immediately. An empty password switches the AP to open. Exposed getters
 * then reflect the new identity.
 */
esp_err_t wifi_manager_set_ap_config(const char *ap_ssid, const char *ap_password);

/** Application callback invoked when SNTP time is first synchronized. */
typedef void (*wifi_manager_time_synced_cb_t)(void *ctx);

/**
 * Register a callback invoked on SNTP time sync. May be called before or
 * after wifi_manager_init(); useful for letting an external logger switch
 * to timestamped filenames. Passing NULL clears the callback.
 */
esp_err_t wifi_manager_set_time_synced_callback(
    wifi_manager_time_synced_cb_t callback, void *ctx);

#ifdef __cplusplus
}
#endif
