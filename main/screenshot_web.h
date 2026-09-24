#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册 GET /screenshot 端点(调试/文档用)。
 *
 * 截取 LCD 当前画面并以 24bpp BMP 返回。必须与其它自定义端点一样
 * 在 web_platform_register_static_fallback() 之前注册。
 */
esp_err_t screenshot_web_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
