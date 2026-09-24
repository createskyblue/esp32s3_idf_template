#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the 立创实战派 ESP32-S3 LCD (ST7789) + LVGL 9.5 display task.
 *
 * Initializes I2C + PCA9557 (LCD CS is driven by the IO expander),
 * SPI bus, ST7789 panel, LEDC backlight, then creates a FreeRTOS task
 * that runs the LVGL timer loop and shows a demo screen
 * (orientation buttons + WiFi/IP info, with CJK font from LittleFS).
 *
 * Safe to call once from app_main; the display task tolerates a missing
 * panel by logging and deleting itself (the rest of the system keeps running).
 */
esp_err_t lcd_lvgl_start(void);

/**
 * @brief Get the shared I2C master bus (created by lcd_lvgl).
 *
 * Audio codecs (ES8311/ES7210) and other on-board I2C peripherals share
 * this bus. Returns NULL if lcd_lvgl_start() has not run yet or failed.
 */
i2c_master_bus_handle_t lcd_lvgl_get_i2c_bus(void);

/**
 * @brief Enable/disable the speaker power amplifier (PCA9557 IO0[1]).
 *
 * The PA_EN pin is driven by the same PCA9557 IO expander that controls
 * LCD_CS, so this is a read-modify-write of the output port.
 */
esp_err_t lcd_lvgl_pa_enable(bool enable);

/**
 * @brief 截取当前屏幕内容(24bpp RGB888, 调试/文档用)。线程安全。
 *
 * 请求显示任务渲染当前激活屏(阻塞等待完成)。输出为 w×h 个像素,
 * 每像素 3 字节, 内存字节序 B,G,R(LVGL 原生, 即 BMP 字节序),
 * 行连续无填充。调用方分配 out, 容量必须 >= w*h*3。
 */
esp_err_t lcd_lvgl_screenshot_rgb565(uint8_t *out, size_t out_cap,
                                     int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif
