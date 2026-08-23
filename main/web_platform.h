#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#include <stdbool.h>

struct cJSON;

/**
 * Start the HTTP server with the platform handlers (/debug.json, /reboot,
 * /ota/..., file manager, /). app_storage_init() must already have succeeded.
 * WiFi-specific endpoints live in the application layer (wifi_config_http).
 *
 * The static file fallback (catch-all) is NOT registered — call
 * web_platform_register_static_fallback() after any custom handlers.
 */
esp_err_t web_platform_init(void);

/**
 * Return the HTTP server handle so callers can register custom URI handlers
 * between web_platform_init() and web_platform_register_static_fallback().
 */
httpd_handle_t web_platform_get_server(void);

/**
 * Register the LittleFS static file fallback handler on the catch-all path.
 * Must be called LAST — after all platform and custom handlers are registered.
 *
 * Requires a private-path policy installed via web_platform_set_private_path_cb()
 * (pass NULL for "no private files"); returns ESP_ERR_INVALID_STATE otherwise.
 */
esp_err_t web_platform_register_static_fallback(void);

/**
 * Application-supplied policy callback: return true when a VFS path is an
 * application-private file that the static file fallback must not serve
 * (the WiFi credential JSON, for example). The platform never hard-codes
 * specific private paths — the application layer installs its own policy.
 */
typedef bool (*web_platform_private_path_cb_t)(const char *path);

/**
 * Install the private-path policy used by the static file fallback.
 *
 * Invariants (documented contract):
 *  - Must be called before web_platform_register_static_fallback(); the
 *    fallback refuses to register (fail-fast, ESP_ERR_INVALID_STATE) while
 *    no policy is installed, so a template copy cannot silently lose
 *    private-file protection. Pass NULL to explicitly declare "no private
 *    files" (protection off).
 *  - Single policy slot: the last call wins; there is no chaining. An app
 *    with several private-file owners must compose them into one callback.
 *  - Configured once during startup (single writer), then read-only after
 *    the fallback handler is registered — no locking is required.
 */
void web_platform_set_private_path_cb(web_platform_private_path_cb_t cb);

/* ── JSON response helpers (usable by custom handlers) ───────────────── */

esp_err_t send_json_text(httpd_req_t *req, const char *json);
esp_err_t send_json_object(httpd_req_t *req, struct cJSON *root);
esp_err_t receive_json_body(httpd_req_t *req, char *buffer, size_t buffer_size);
