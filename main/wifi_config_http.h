#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * 应用层 WiFi 配网页端点：/network.json（网络状态）与 /wifi_config.json
 * （配网 GET/POST，含字段校验、部分更新合并与应用层事务）。
 *
 * 这是"业务端点"的活示例——独立于平台 Web 服务，注册方式与 hello_web 相同：
 *   wifi_config_http_install_guards();            // ① 安全策略，必须先于平台 init
 *   web_platform_init();
 *   wifi_config_http_register(web_platform_get_server());  // ② 应用层端点
 *   hello_web_register(web_platform_get_server());         // ③ 你的业务
 *   web_platform_register_static_fallback();               // ④ 必须最后
 *
 * install_guards() 安装平台保护策略（WiFi 凭据是应用私有文件）：
 *   - file_manager 读写守卫：文件管理器不列出/下载/删除凭据文件；
 *   - web_platform 静态回退守卫：catch-all 不对外提供凭据文件。
 * 平台本身不感知具体文件名——策略由应用层模块提供。
 */

/**
 * Install the private-file protection policies (file-manager read/mutation
 * guards + the web_platform static-fallback policy). Must run BEFORE
 * web_platform_init() so the HTTP server never accepts a request without
 * protection in place.
 */
esp_err_t wifi_config_http_install_guards(void);

/** Register the /network.json and /wifi_config.json endpoints. */
esp_err_t wifi_config_http_register(httpd_handle_t server);
