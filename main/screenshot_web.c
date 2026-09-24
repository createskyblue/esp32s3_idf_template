#include "screenshot_web.h"

#include "lcd_lvgl.h"
#include "web_platform.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_server.h"

#include <string.h>

static const char *TAG = "SHOT";

#define SHOT_MAX_W 320
#define SHOT_MAX_H 320
#define SHOT_BUF_BYTES (SHOT_MAX_W * SHOT_MAX_H * 3)   /* RGB888 */

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)(v >> 24);
}

/* ── GET /screenshot: 截取 LCD 当前画面, 返回 24bpp BMP(调试/文档用) ── */
static esp_err_t screenshot_handler(httpd_req_t *req)
{
    uint8_t *buf = heap_caps_malloc(SHOT_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    int w = 0, h = 0;
    if (lcd_lvgl_screenshot_rgb565(buf, SHOT_BUF_BYTES, &w, &h) != ESP_OK || w <= 0 || h <= 0) {
        heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
        return ESP_FAIL;
    }

    const uint32_t row_bytes = (uint32_t)w * 3;
    const uint32_t pad = (4u - (row_bytes % 4u)) % 4u;
    const uint32_t data_size = (row_bytes + pad) * (uint32_t)h;
    const uint32_t file_size = 54u + data_size;

    /* BMP 头(24bpp, 底部行优先) */
    uint8_t hdr[54] = { 0 };
    hdr[0] = 'B';
    hdr[1] = 'M';
    put_le32(hdr + 2, file_size);
    put_le32(hdr + 10, 54);            /* 像素数据偏移 */
    put_le32(hdr + 14, 40);            /* DIB 头大小 */
    put_le32(hdr + 18, (uint32_t)w);
    put_le32(hdr + 22, (uint32_t)h);
    put_le16(hdr + 26, 1);             /* 平面数 */
    put_le16(hdr + 28, 24);            /* 每像素位数 */
    put_le32(hdr + 34, data_size);
    put_le32(hdr + 38, 2835);          /* 72 DPI */
    put_le32(hdr + 42, 2835);

    httpd_resp_set_type(req, "image/bmp");
    if (httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr)) != ESP_OK) {
        heap_caps_free(buf);
        return ESP_FAIL;
    }

    uint8_t row[SHOT_MAX_W * 3 + 3];   /* 栈: 320*3+pad=963B */
    for (int y = h - 1; y >= 0; y--) {
        /* RGB888 内存字节序即 B,G,R = BMP 字节序, 直接拷贝 */
        const uint8_t *src = buf + (size_t)y * w * 3;
        memcpy(row, src, row_bytes);
        memset(row + row_bytes, 0, pad);
        if (httpd_resp_send_chunk(req, (const char *)row, row_bytes + pad) != ESP_OK) {
            heap_caps_free(buf);
            return ESP_FAIL;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    heap_caps_free(buf);
    ESP_LOGI(TAG, "screenshot %dx%d sent", w, h);
    return ESP_OK;
}

esp_err_t screenshot_web_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const httpd_uri_t uri = {
        .uri = "/screenshot",
        .method = HTTP_GET,
        .handler = screenshot_handler,
    };
    const esp_err_t err = httpd_register_uri_handler(server, &uri);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "/screenshot endpoint registered");
    } else {
        ESP_LOGE(TAG, "/screenshot registration failed: %s", esp_err_to_name(err));
    }
    return err;
}
