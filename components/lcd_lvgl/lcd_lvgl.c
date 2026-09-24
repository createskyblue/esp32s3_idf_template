#include "lcd_lvgl.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/font/binfont_loader/lv_binfont_loader.h"
#include "lv_fs_littlefs.h"
#include "wifi_manager.h"

/* ═══════════════ 立创实战派 ESP32-S3 引脚定义 ═══════════════ */

/* I2C (PCA9557 IO expander / QMI8658 IMU share this bus) */
#define BSP_I2C_SDA          GPIO_NUM_1
#define BSP_I2C_SCL          GPIO_NUM_2
#define BSP_I2C_FREQ_HZ      (100000)

/* PCA9557 IO expander (address 0x19) */
#define PCA9557_SENSOR_ADDR         0x19
#define PCA9557_INPUT_PORT          0x00
#define PCA9557_OUTPUT_PORT         0x01
#define PCA9557_POLARITY_PORT       0x02
#define PCA9557_CONFIGURATION_PORT  0x03
#define PCA9557_LCD_CS_BIT          (1u << 0)
#define PCA9557_PA_EN_BIT           (1u << 1)
#define PCA9557_DVP_PWDN_BIT        (1u << 2)

/* LCD ST7789, 320x240, SPI */
#define LCD_H_RES           320
#define LCD_V_RES           240
#define LCD_SPI_HOST        SPI3_HOST
#define LCD_PIXEL_CLOCK_HZ  (80 * 1000 * 1000)
#define LCD_PIN_MOSI        GPIO_NUM_40
#define LCD_PIN_SCLK        GPIO_NUM_41
#define LCD_PIN_CS          GPIO_NUM_NC   /* CS is driven by PCA9557 */
#define LCD_PIN_DC          GPIO_NUM_39
#define LCD_PIN_RST         GPIO_NUM_NC
#define LCD_PIN_BACKLIGHT   GPIO_NUM_42
#define LCD_BITS_PER_PIXEL  16

/* LEDC backlight: low-speed mode, ch0/timer1, 10-bit, 5 kHz, inverted */
#define LCD_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LCD_LEDC_CH         LEDC_CHANNEL_0
#define LCD_LEDC_TIMER      LEDC_TIMER_1
#define LCD_LEDC_DUTY_RES   LEDC_TIMER_10_BIT
#define LCD_LEDC_FREQ_HZ    5000
#define LCD_LEDC_MAX_DUTY   ((1u << 10) - 1u)

/* LVGL: two full-frame draw buffers in PSRAM. A full-frame buffer makes the
 * flush a single SPI transaction per frame, so the SPI transfer of frame N
 * overlaps the rendering of frame N+1 -> ~60 fps at 80 MHz SPI (measured
 * 62.9 fps / 15.9 ms per full-screen frame). */
#define LCD_BUFFER_ROWS     240
#define LCD_BUFFER_BYTES    (LCD_H_RES * LCD_BUFFER_ROWS * sizeof(uint16_t))

#define DISPLAY_TASK_STACK_BYTES 16384u
#define DISPLAY_TASK_PRIORITY    7u
/* How often the display task polls lv_timer_handler. Must be well below the
 * LVGL refresh period (16 ms) so a 60 Hz refresh actually gets serviced. */
#define DISPLAY_REFRESH_MS       5u

static const char *TAG = "LCD_LVGL";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_pca9557_dev = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;

/* Posted from the esp_lcd DMA-done callback so lcd_flush_wait_cb() can
 * block (instead of LVGL's default busy-loop) while a frame is draining. */
static SemaphoreHandle_t s_flush_done = NULL;

#if CONFIG_LCD_LVGL_BENCHMARK
/* Raw SPI throughput benchmark (heap buffers, bypasses LVGL). */
static void raw_spi_benchmark(void)
{
    ESP_LOGI(TAG, "DBG: CPU=%d MHz APB=%d MHz", esp_clk_cpu_freq()/1000000,
             esp_clk_apb_freq()/1000000);

    /* Full frame from PSRAM (this is what LVGL buffers use). */
    uint8_t *full = heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (!full) { ESP_LOGE(TAG, "raw spi: malloc failed"); return; }
    ESP_LOGI(TAG, "RAW SPI buf=%p align=%u", (void*)full, (unsigned)((uintptr_t)full & 0x3F));
    memset(full, 0x33, 320*240*2);
    const int nf = 30;
    uint64_t t0 = esp_timer_get_time();
    for (int i = 0; i < nf; i++) esp_lcd_panel_draw_bitmap(s_panel, 0, 0, 320, 240, full);
    uint64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "RAW SPI full(PSRAM): %.3f ms/block -> %.2f MB/s", (double)(t1-t0)/1e3/nf,
             (double)(320*240*2) / ((double)(t1-t0)/1e6/nf) / 1e6);

    /* 60-row block from DRAM: isolates pure SPI line rate (no PSRAM read). */
    uint8_t *dram = heap_caps_malloc(320 * 60 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (dram) {
        memset(dram, 0x55, 320*60*2);
        uint64_t d0 = esp_timer_get_time();
        for (int i = 0; i < nf; i++) esp_lcd_panel_draw_bitmap(s_panel, 0, 0, 320, 60, dram);
        uint64_t d1 = esp_timer_get_time();
        ESP_LOGI(TAG, "RAW SPI 60row(DRAM): %.3f ms/block -> %.2f MB/s", (double)(d1-d0)/1e3/nf,
                 (double)(320*60*2) / ((double)(d1-d0)/1e6/nf) / 1e6);
        heap_caps_free(dram);
    } else {
        ESP_LOGW(TAG, "raw spi: DRAM malloc failed (skip)");
    }

    heap_caps_free(full);
}
#endif /* CONFIG_LCD_LVGL_BENCHMARK */


#if CONFIG_LCD_LVGL_BENCHMARK
/* Full-screen refresh benchmark counters (updated from flush callback) */
static uint64_t s_flush_bytes = 0;
static uint64_t s_flush_us = 0;
static uint32_t s_flush_count = 0;
#endif /* CONFIG_LCD_LVGL_BENCHMARK */

/* ═══════════════ I2C (new driver API) ═══════════════ */

static esp_err_t bsp_i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus),
                        TAG, "I2C bus init failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCA9557_SENSOR_ADDR,
        .scl_speed_hz = BSP_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg,
                                                  &s_pca9557_dev),
                        TAG, "PCA9557 device add failed");
    ESP_LOGI(TAG, "I2C ready (SDA=1, SCL=2, 100 kHz)");
    return ESP_OK;
}

/* ═══════════════ PCA9557 IO expander ═══════════════ */

static esp_err_t pca9557_write_byte(uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = { reg, data };
    return i2c_master_transmit(s_pca9557_dev, buf, sizeof(buf), 100);
}

static esp_err_t pca9557_read_byte(uint8_t reg, uint8_t *data)
{
    return i2c_master_transmit_receive(s_pca9557_dev, &reg, 1, data, 1, 100);
}

static esp_err_t pca9557_init(void)
{
    /* Default outputs: DVP_PWDN=1, PA_EN=0, LCD_CS=1 */
    ESP_RETURN_ON_ERROR(pca9557_write_byte(PCA9557_OUTPUT_PORT, 0x05),
                        TAG, "PCA9557 output write failed");
    /* IO0-2 configured as outputs, the rest stay inputs */
    ESP_RETURN_ON_ERROR(pca9557_write_byte(PCA9557_CONFIGURATION_PORT, 0xf8),
                        TAG, "PCA9557 config write failed");
    return ESP_OK;
}

static esp_err_t lcd_cs_set(bool level)
{
    uint8_t data = 0;
    ESP_RETURN_ON_ERROR(pca9557_read_byte(PCA9557_OUTPUT_PORT, &data),
                        TAG, "PCA9557 read failed");
    if (level) {
        data |= PCA9557_LCD_CS_BIT;
    } else {
        data &= (uint8_t)~PCA9557_LCD_CS_BIT;
    }
    return pca9557_write_byte(PCA9557_OUTPUT_PORT, data);
}

/* ═══════════════ shared bus + PA (audio BSP hooks) ═══════════════ */

i2c_master_bus_handle_t lcd_lvgl_get_i2c_bus(void)
{
    return s_i2c_bus;
}

esp_err_t lcd_lvgl_pa_enable(bool enable)
{
    if (s_pca9557_dev == NULL) {
        ESP_LOGE(TAG, "PA: PCA9557 not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t data = 0;
    ESP_RETURN_ON_ERROR(pca9557_read_byte(PCA9557_OUTPUT_PORT, &data),
                        TAG, "PA: PCA9557 read failed");
    if (enable) {
        data |= PCA9557_PA_EN_BIT;
    } else {
        data &= (uint8_t)~PCA9557_PA_EN_BIT;
    }
    ESP_RETURN_ON_ERROR(pca9557_write_byte(PCA9557_OUTPUT_PORT, data),
                        TAG, "PA: PCA9557 write failed");
    ESP_LOGI(TAG, "PA %s", enable ? "ON" : "OFF");
    return ESP_OK;
}

/* ═══════════════ CJK 字体(文件系统加载, 无则回退内置) ═══════════════ */
static lv_font_t *s_ui_asr_font = NULL;

/* ═══════════════ 演示屏(替代官方 widget demo) ═══════════════
 * 回流自 ESP32S3_VoiceInput 的设置屏并精简: 演示 CJK 字体渲染、
 * 屏幕方向即时切换(面板 MADCTL + 触摸手动映射)与 WiFi/IP 显示(500ms 节流)。 */

static lv_display_t *s_disp = NULL;   /* display task 创建后保存 */
static int s_orientation = 0;         /* 0/1/2/3 = 0/90/180/270 度 */
static lv_obj_t *s_demo_scr = NULL;
static lv_obj_t *s_ori_btns[4] = { NULL };  /* 4 个方向按钮 */
static lv_obj_t *s_demo_wifi_lbl = NULL;    /* 当前 WiFi 名称 */
static lv_obj_t *s_demo_ip_lbl = NULL;      /* 当前 IP 地址 */
static bool s_demo_loaded = false;
static uint32_t s_demo_net_last_refresh_us = 0;  /* 网络信息刷新节流 */

/* 截图请求结构: display task 每帧轮询消费, 实现见 public API 前 */
typedef struct {
    SemaphoreHandle_t done;
    uint8_t *out;
    size_t out_cap;
    int w, h;
    esp_err_t result;
} screenshot_req_t;
static screenshot_req_t *s_shot_req = NULL;   /* 待处理截图请求 */
static void screenshot_execute(void);         /* 定义在 public API 前 */

/* LVGL 旋转保持 0(禁用其自动触摸变换, 触摸由我们手动映射), 仅交换分辨率实现
 * 布局自适应; 面板 MADCTL 配合(基态 swap=true, mirror(true,false)) */
static void demo_apply_rotation(void)
{
    if (s_disp == NULL) {
        return;
    }
    const int o = s_orientation;
    const int idx = (o >= 0 && o <= 3) ? o : 0;
    lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_0);
    if (idx == 1 || idx == 3) {
        lv_display_set_resolution(s_disp, LCD_V_RES, LCD_H_RES);   /* 竖屏 240x320 */
    } else {
        lv_display_set_resolution(s_disp, LCD_H_RES, LCD_V_RES);   /* 横屏 320x240 */
    }
    switch (idx) {
    case 0:
        esp_lcd_panel_swap_xy(s_panel, true);
        esp_lcd_panel_mirror(s_panel, true, false);
        break;
    case 1:   /* 90° */
        esp_lcd_panel_swap_xy(s_panel, false);
        esp_lcd_panel_mirror(s_panel, true, true);
        break;
    case 2:   /* 180° */
        esp_lcd_panel_swap_xy(s_panel, true);
        esp_lcd_panel_mirror(s_panel, false, true);
        break;
    default:  /* 270° */
        esp_lcd_panel_swap_xy(s_panel, false);
        esp_lcd_panel_mirror(s_panel, false, false);
        break;
    }
    ESP_LOGI(TAG, "orientation=%d applied", idx);
}

static void demo_ori_cb(lv_event_t *e)
{
    const int o = (int)(intptr_t)lv_event_get_user_data(e);
    s_orientation = o;
    demo_apply_rotation();
    /* 高亮当前方向 */
    for (int i = 0; i < 4; i++) {
        if (s_ori_btns[i] != NULL) {
            lv_obj_set_style_bg_color(s_ori_btns[i],
                i == o ? lv_color_hex(0x07C160) : lv_color_hex(0x3A4654), 0);
        }
    }
}

/* 刷新演示屏网络信息: WiFi 名称 + IP 地址(由显示任务调用)。
 * STA 未连接时显示 AP 地址, 用户仍可凭它访问配网页。 */
static void demo_refresh_network_info(void)
{
    if (s_demo_wifi_lbl == NULL || s_demo_ip_lbl == NULL) {
        return;
    }
    char buf[80];
    if (!wifi_manager_is_started()) {
        lv_label_set_text(s_demo_wifi_lbl, "WiFi: 初始化中");
        lv_label_set_text(s_demo_ip_lbl, "IP: --");
        return;
    }
    wifi_snapshot_t snap;
    wifi_manager_get_snapshot(&snap);
    if (snap.sta_connected) {
        snprintf(buf, sizeof(buf), "WiFi: %s", snap.sta_ssid);
        lv_label_set_text(s_demo_wifi_lbl, buf);
        snprintf(buf, sizeof(buf), "IP: %s", snap.sta_ip);
    } else {
        lv_label_set_text(s_demo_wifi_lbl, "WiFi: 未连接");
        snprintf(buf, sizeof(buf), "IP: %s (AP)", snap.ap_ip);
    }
    lv_label_set_text(s_demo_ip_lbl, buf);
}

/* 500ms 节流: 其余时间零开销 */
static void demo_network_refresh_if_active(void)
{
    if (s_demo_scr == NULL || lv_screen_active() != s_demo_scr) {
        return;
    }
    const uint64_t now = esp_timer_get_time();
    if (now - s_demo_net_last_refresh_us < 500000ULL) {
        return;
    }
    s_demo_net_last_refresh_us = (uint32_t)now;
    demo_refresh_network_info();
}

static void ui_apply(void)
{
    demo_network_refresh_if_active();
    if (s_demo_scr != NULL && !s_demo_loaded) {
        s_demo_loaded = true;
        lv_screen_load(s_demo_scr);
        ESP_LOGI(TAG, "demo screen loaded");
    }
}

static void demo_screen_create(void)
{
    s_demo_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_demo_scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(s_demo_scr, LV_OPA_COVER, 0);   /* 显式完全不透明 */
    lv_obj_set_size(s_demo_scr, lv_display_get_horizontal_resolution(s_disp),
                    lv_display_get_vertical_resolution(s_disp));   /* 显式全屏尺寸 */

    lv_obj_t *title = lv_label_create(s_demo_scr);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_font(title, s_ui_asr_font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x9AC8FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    /* 屏幕方向 2x2 */
    lv_obj_t *ori_lbl = lv_label_create(s_demo_scr);
    lv_label_set_text(ori_lbl, "屏幕方向");
    lv_obj_set_style_text_font(ori_lbl, s_ui_asr_font, 0);
    lv_obj_set_style_text_color(ori_lbl, lv_color_hex(0xE8E8E8), 0);
    lv_obj_align(ori_lbl, LV_ALIGN_TOP_LEFT, 24, 60);
    static const char *ori_names[4] = { "0°", "90°", "180°", "270°" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_button_create(s_demo_scr);
        lv_obj_set_size(b, 60, 32);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 24 + (i % 2) * 72, 84 + (i / 2) * 42);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x3A4654), 0);
        lv_obj_add_event_cb(b, demo_ori_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *bl = lv_label_create(b);
        lv_label_set_text(bl, ori_names[i]);
        lv_obj_set_style_text_font(bl, s_ui_asr_font, 0);
        lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(bl);
        s_ori_btns[i] = b;
    }

    /* 当前网络信息: WiFi 名称在上, IP 地址在下 */
    lv_obj_t *wifi_lbl = lv_label_create(s_demo_scr);
    lv_label_set_text(wifi_lbl, "WiFi: --");
    lv_obj_set_style_text_font(wifi_lbl, s_ui_asr_font, 0);
    lv_obj_set_style_text_color(wifi_lbl, lv_color_hex(0x9AC8FF), 0);
    lv_obj_align(wifi_lbl, LV_ALIGN_TOP_LEFT, 24, 200);
    s_demo_wifi_lbl = wifi_lbl;

    lv_obj_t *ip_lbl = lv_label_create(s_demo_scr);
    lv_label_set_text(ip_lbl, "IP: --");
    lv_obj_set_style_text_font(ip_lbl, s_ui_asr_font, 0);
    lv_obj_set_style_text_color(ip_lbl, lv_color_hex(0xE8E8E8), 0);
    lv_obj_align(ip_lbl, LV_ALIGN_TOP_LEFT, 24, 218);
    s_demo_ip_lbl = ip_lbl;

    demo_refresh_network_info();
    for (int i = 0; i < 4; i++) {
        if (s_ori_btns[i] != NULL) {
            lv_obj_set_style_bg_color(s_ori_btns[i],
                i == s_orientation ? lv_color_hex(0x07C160) : lv_color_hex(0x3A4654), 0);
        }
    }
}
/* ═══════════════ LEDC backlight ═══════════════ */

static esp_err_t bsp_display_backlight_init(void)
{
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LCD_LEDC_MODE,
        .duty_resolution = LCD_LEDC_DUTY_RES,
        .timer_num = LCD_LEDC_TIMER,
        .freq_hz = LCD_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg),
                        TAG, "LEDC timer config failed");

    const ledc_channel_config_t ch_cfg = {
        .gpio_num = LCD_PIN_BACKLIGHT,
        .speed_mode = LCD_LEDC_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = true,   /* backlight is active-low */
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg),
                        TAG, "LEDC channel config failed");
    return ESP_OK;
}

static esp_err_t bsp_display_backlight_set(int brightness_percent)
{
    if (brightness_percent < 0) brightness_percent = 0;
    if (brightness_percent > 100) brightness_percent = 100;
    const uint32_t duty =
        (LCD_LEDC_MAX_DUTY * (uint32_t)brightness_percent) / 100u;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LCD_LEDC_MODE, LCD_LEDC_CH, duty),
                        TAG, "LEDC set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LCD_LEDC_MODE, LCD_LEDC_CH),
                        TAG, "LEDC update duty failed");
    return ESP_OK;
}

/* ═══════════════ ST7789 panel ═══════════════ */

static esp_err_t lcd_panel_init(void)
{
    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg,
                                           SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_PIN_CS,       /* GPIO_NUM_NC: CS via PCA9557 */
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 2,                   /* board wiring uses SPI mode 2 */
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 4,   /* deeper queue: pipeline SPI with LVGL rendering */
        /* Let DMA read the LVGL draw buffers straight from PSRAM. The default
         * path memcpy()s every flush into internal RAM first, which measured
         * ~7.4 MB/s; direct PSRAM DMA reaches ~10 MB/s (80 MHz line limit)
         * and is the key to sustaining 60 fps. */
        .flags.psram_dma_direct = true,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_cfg, &io),
                        TAG, "New SPI panel IO failed");
    s_io = io;   /* keep handle: register the DMA-done callback after LVGL init */

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel),
                        TAG, "New ST7789 panel failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(lcd_cs_set(false), TAG, "LCD CS low failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "Panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true),
                        TAG, "Invert color failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true),
                        TAG, "Swap xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false),
                        TAG, "Mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true),
                        TAG, "Display on failed");
    ESP_RETURN_ON_ERROR(bsp_display_backlight_set(100),
                        TAG, "Backlight on failed");
    ESP_LOGI(TAG, "ST7789 panel ready (%ux%u @ %u Hz)",
             LCD_H_RES, LCD_V_RES, (unsigned)LCD_PIXEL_CLOCK_HZ);
    return ESP_OK;
}

/* ═══════════════ LVGL 9 ═══════════════ */

static uint32_t lv_tick_get_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* Called from the esp_lcd SPI task once GDMA has really finished sending the
 * frame. Only then is it safe for LVGL to reuse the draw buffer — the flush
 * callback itself only queues an async SPI transaction. */
static bool lcd_flush_ready_cb(esp_lcd_panel_io_handle_t io,
                               esp_lcd_panel_io_event_data_t *edata,
                               void *user_ctx)
{
    (void)io;
    (void)edata;
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    if (s_flush_done != NULL) {
        xSemaphoreGive(s_flush_done);
    }
    return true;
}

/* LVGL calls this instead of its default `while (disp->flushing);` busy
 * loop. We block on the DMA-done semaphore, so the display task sleeps
 * (0% CPU) instead of spinning while a frame is being pushed to the panel. */
static void lcd_flush_wait_cb(lv_display_t *disp)
{
    (void)disp;
    if (s_flush_done != NULL) {
        xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(100));
    }
}

static void lcd_flush_cb(lv_display_t *disp, const lv_area_t *area,
                         uint8_t *px_map)
{
    (void)disp;
    /* LVGL renders directly in RGB565_SWAPPED (big-endian byte order) to
     * match the ST7789, so the buffer is handed to esp_lcd as-is, zero-copy. */
#if CONFIG_LCD_LVGL_BENCHMARK
    const size_t count = (size_t)(area->x2 - area->x1 + 1) *
                         (size_t)(area->y2 - area->y1 + 1);
    const uint64_t flush_t0 = esp_timer_get_time();
#endif
    /* Queue the frame: DMA reads it from PSRAM while LVGL renders the next
     * one; lv_display_flush_ready() fires from lcd_flush_ready_cb(). */
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
#if CONFIG_LCD_LVGL_BENCHMARK
    s_flush_bytes += (uint64_t)count * sizeof(uint16_t);
    s_flush_us += esp_timer_get_time() - flush_t0;
    s_flush_count++;
#endif
}

/* ═══════════════ FT5x06 电容触摸（立创实战派触摸屏，与 LCD 同 I2C 总线）═══════════ */

static esp_lcd_touch_handle_t s_touch = NULL;
static esp_lcd_touch_point_data_t s_touch_cached[2];  /* 已旋转映射, 供 LVGL indev 消费 */
static uint8_t s_touch_cached_n = 0;

/* 触摸手动映射: 0° 原样; 旋转后按面板 MADCTL 方向换算(LVGL rotation 保持 0) */
static void map_touch_point(const esp_lcd_touch_point_data_t *in, esp_lcd_touch_point_data_t *out)
{
    const int32_t rx = in->x;
    const int32_t ry = in->y;
    switch (s_orientation) {
    case 1:   /* 90° */
        out->x = (uint16_t)ry;
        out->y = (uint16_t)(LCD_H_RES - 1 - rx);
        break;
    case 2:   /* 180° */
        out->x = (uint16_t)(LCD_H_RES - 1 - rx);
        out->y = (uint16_t)(LCD_V_RES - 1 - ry);
        break;
    case 3:   /* 270° */
        out->x = (uint16_t)(LCD_V_RES - 1 - ry);
        out->y = (uint16_t)rx;
        break;
    default:  /* 0° */
        out->x = (uint16_t)rx;
        out->y = (uint16_t)ry;
        break;
    }
}

/* 主循环触摸采样(约 200Hz): 读 I2C → 旋转映射 → 缓存给 LVGL + 触控板手势。
 * 原来由 LVGL indev 定时器驱动(LV_DEF_REFR_PERIOD=16ms ≈ 62Hz), 触控板滑动明显卡顿;
 * 改为显示主循环 5ms 轮询后, 手势增量更细更平滑。 */
static void touch_poll(void)
{
    if (s_touch == NULL) {
        s_touch_cached_n = 0;
        return;
    }
    esp_lcd_touch_read_data(s_touch);
    esp_lcd_touch_point_data_t pts[2];
    uint8_t n = 0;
    if (esp_lcd_touch_get_data(s_touch, pts, &n, 2) == ESP_OK && n > 0) {
        uint8_t m = 0;
        for (uint8_t i = 0; i < n && i < 2; i++) {
            map_touch_point(&pts[i], &s_touch_cached[i]);
            m++;
        }
        s_touch_cached_n = m;
    } else {
        s_touch_cached_n = 0;
    }
}

/* LVGL 轮询读取触摸点（LV_OS_NONE：read_cb 运行在 display 任务上下文，天然线程安全）
 * 消费主循环缓存的采样, 自身不做 I2C 读取 */
static void touch_read_cb(lv_indev_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    if (s_touch_cached_n > 0) {
        data->point.x = s_touch_cached[0].x;
        data->point.y = s_touch_cached[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

/* 初始化触摸屏并注册 LVGL 指针输入设备；坐标映射与屏幕旋转（swap_xy + mirror_x）一致 */
static void bsp_touch_init(void)
{
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_V_RES,      /* 触控 IC 原生 240x320，经 swap+mirror 映射到 320x240 */
        .y_max = LCD_H_RES,
        .rst_gpio_num = GPIO_NUM_NC,  /* 与 LCD 共用复位 */
        .int_gpio_num = GPIO_NUM_NC,  /* 轮询模式，不用中断脚 */
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 1,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };

    esp_lcd_panel_io_handle_t tp_io = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    if (esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io) != ESP_OK) {
        ESP_LOGE(TAG, "touch panel IO init failed");
        return;
    }
    if (esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_touch) != ESP_OK) {
        ESP_LOGE(TAG, "FT5x06 touch init failed");
        return;
    }

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    ESP_LOGI(TAG, "FT5x06 touch ready, LVGL pointer indev registered");
}

#if CONFIG_LCD_LVGL_BENCHMARK
/* Full-screen refresh benchmark: force whole-screen redraws and measure
 * frames-per-second (render + SPI transfer) plus the raw SPI flush
 * throughput to the panel. */
static void run_fullscreen_benchmark(void)
{
    const int frames = 30;
    lv_obj_t *scr = lv_scr_act();

    /* Warm-up: one full frame so renderer/caches are ready. */
    lv_obj_invalidate(scr);
    lv_timer_handler();

    const uint32_t flushes0 = s_flush_count;
    const uint64_t flush_us0 = s_flush_us;
    const uint64_t flush_bytes0 = s_flush_bytes;

    lv_timer_t *refr = lv_display_get_refr_timer(lv_display_get_default());

    uint64_t render_us = 0;
    const uint64_t t0 = esp_timer_get_time();
    for (int i = 0; i < frames; i++) {
        /* Cycle the full-screen background so every pixel is rewritten. */
        const uint32_t color = 0x000000u | ((uint32_t)(i & 7) << 5);
        lv_obj_set_style_bg_color(scr, lv_color_hex(color), 0);
        lv_obj_invalidate(scr);
        /* LVGL 9: refresh timer pauses itself after each run; force it due
         * so every loop iteration really redraws the whole screen. */
        lv_timer_ready(refr);
        const uint64_t r0 = esp_timer_get_time();
        lv_timer_handler();
        render_us += esp_timer_get_time() - r0;
    }
    const uint64_t t1 = esp_timer_get_time();

    const uint64_t elapsed_us = (uint64_t)(t1 - t0);
    const double frame_ms = (double)elapsed_us / 1000.0 / (double)frames;
    const double fps = 1000.0 / frame_ms;

    const uint32_t flushes = s_flush_count - flushes0;
    const uint64_t flush_us = s_flush_us - flush_us0;
    const uint64_t flush_bytes = s_flush_bytes - flush_bytes0;
    const double mbps = (double)flush_bytes / ((double)flush_us / 1e6) / 1e6;

    ESP_LOGI(TAG, "FULLSCREEN render: %.2f ms/frame (timer_handler), flush: %.2f ms/frame",
             (double)render_us / 1000.0 / (double)frames,
             (double)flush_us / 1000.0 / (double)frames);
    ESP_LOGI(TAG, "FULLSCREEN refresh: %.2f fps (%.3f ms/frame, %d frames, %dx%d)",
             fps, frame_ms, frames, LCD_H_RES, LCD_V_RES);
    ESP_LOGI(TAG, "FULLSCREEN flush: %u partial flushes (%llu B), %.2f MB/s to panel",
             flushes, (unsigned long long)flush_bytes, mbps);
}
#endif /* CONFIG_LCD_LVGL_BENCHMARK */

static void display_task(void *arg)
{
    (void)arg;

    esp_err_t err = lcd_panel_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD init failed (%s); display task stopped",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    /* Two full-frame draw buffers; location is a Kconfig switch
     * (LCD_LVGL_BUF_IN_PSRAM) so DRAM vs PSRAM refresh can be compared. */
#if CONFIG_LCD_LVGL_BUF_IN_PSRAM
    uint32_t buf_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    uint32_t buf_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    uint8_t *buf1 = heap_caps_malloc(LCD_BUFFER_BYTES, buf_caps);
    uint8_t *buf2 = heap_caps_malloc(LCD_BUFFER_BYTES, buf_caps);
    if (buf1 == NULL || buf2 == NULL) {
        ESP_LOGE(TAG, "Not enough memory for LVGL draw buffers");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "LVGL buffers in %s: buf1=%p buf2=%p (%u B each)",
             (buf_caps & MALLOC_CAP_SPIRAM) ? "PSRAM" : "DRAM",
             buf1, buf2, (unsigned)LCD_BUFFER_BYTES);

    lv_init();
    lv_tick_set_cb(lv_tick_get_ms);

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    s_disp = disp;   /* 设置屏旋转用 */
    /* Render directly in ST7789-native byte order (big-endian RGB565):
     * removes the per-pixel byte-swap loop in the flush callback. */
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(disp, lcd_flush_cb);
    s_flush_done = xSemaphoreCreateBinary();
    if (s_flush_done == NULL) {
        ESP_LOGE(TAG, "Failed to create flush semaphore");
    }
    lv_display_set_flush_wait_cb(disp, lcd_flush_wait_cb);

    /* Flush is async: only release the LVGL draw buffer when the transfer
     * has actually finished (on_color_trans_done), never before. */
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lcd_flush_ready_cb,
    };
    esp_err_t cb_err = esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, disp);
    if (cb_err != ESP_OK) {
        ESP_LOGE(TAG, "Register panel-io callback failed: %s",
                 esp_err_to_name(cb_err));
    }

    /* PARTIAL 渲染模式(不动底层 buffer/驱动); 旋转暂存设置不实际旋转 */
    lv_display_set_buffers(disp, buf1, buf2, LCD_BUFFER_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    bsp_touch_init();

    /* 文件系统字体：注册 LVGL FS 驱动并从 /littlefs 加载 CJK 字体
     * （驱动根目录就是 /littlefs，所以路径只写文件名） */
    lv_fs_littlefs_register();
    lv_font_t *cjk = lv_binfont_create("S:/lv_font_simhei_16_cjk.bin");
    if (cjk != NULL) {
        s_ui_asr_font = cjk;
        ESP_LOGI(TAG, "CJK font loaded from littlefs");
    } else {
        s_ui_asr_font = (lv_font_t *)&lv_font_montserrat_14;
        ESP_LOGW(TAG, "lv_font_simhei_16_cjk.bin not found on /littlefs, fallback to montserrat");
    }

    /* 演示屏(旋转按钮 + WiFi/IP, 替代官方 widget demo) */
    demo_screen_create();

    ESP_LOGI(TAG, "display task started");
#if CONFIG_LCD_LVGL_BENCHMARK
    raw_spi_benchmark();
    run_fullscreen_benchmark();
#endif
    while (1) {
        touch_poll();   /* 触摸采样(200Hz) */
        ui_apply();
        if (s_shot_req != NULL) {
            screenshot_execute();   /* 截图请求: 渲染当前屏(调试/文档) */
        }
        uint32_t time_till_next = lv_timer_handler();
        /* Sleep until the next LVGL timer is due instead of polling every
         * DISPLAY_REFRESH_MS. lv_timer_handler() returns ms until the next
         * due timer; cap it so we stay responsive. */
        if (time_till_next > DISPLAY_REFRESH_MS) {
            time_till_next = DISPLAY_REFRESH_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(time_till_next));
    }
}

/* ═══════════════ 截图(调试/文档): 请求显示任务渲染当前屏 ═══════════════
 * HTTP 任务填充请求并阻塞等完成信号; 显示任务每帧轮询消费,
 * 用 LVGL 快照把当前激活屏渲染成 RGB565 到调用方缓冲(PSRAM)。 */
/* 显示任务上下文: 渲染当前激活屏到请求缓冲(RGB565) */
static void screenshot_execute(void)
{
    screenshot_req_t *r = s_shot_req;
    s_shot_req = NULL;
    if (r == NULL || r->done == NULL || r->out == NULL) {
        if (r != NULL && r->done != NULL) {
            r->result = ESP_ERR_INVALID_ARG;
            xSemaphoreGive(r->done);
        }
        return;
    }
    r->result = ESP_ERR_TIMEOUT;
    lv_obj_t *scr = lv_screen_active();
    if (scr != NULL) {
        const int w = lv_display_get_horizontal_resolution(s_disp);
        const int h = lv_display_get_vertical_resolution(s_disp);
        if ((size_t)w * h * 3 <= r->out_cap) {
            lv_draw_buf_t db;
            /* RGB888(内存字节序 B,G,R) = 设计原色, 无 RGB565 量化误差 */
            if (lv_draw_buf_init(&db, w, h, LV_COLOR_FORMAT_RGB888, w * 3, r->out,
                                 (size_t)w * h * 3) == LV_RESULT_OK) {
                if (lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB888, &db) == LV_RESULT_OK) {
                    r->w = w;
                    r->h = h;
                    r->result = ESP_OK;
                } else {
                    r->result = ESP_ERR_NOT_SUPPORTED;
                }
            } else {
                r->result = ESP_ERR_NO_MEM;
            }
        } else {
            r->result = ESP_ERR_INVALID_SIZE;
        }
    }
    xSemaphoreGive(r->done);
}

esp_err_t lcd_lvgl_screenshot_rgb565(uint8_t *out, size_t out_cap,
                                     int *out_w, int *out_h)
{
    if (out == NULL || out_w == NULL || out_h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    screenshot_req_t req = {
        .done = xSemaphoreCreateBinary(),
        .out = out,
        .out_cap = out_cap,
        .w = 0,
        .h = 0,
        .result = ESP_ERR_TIMEOUT,
    };
    if (req.done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_shot_req = &req;   /* 显示任务每帧轮询消费(≤5ms 一帧, 快照渲染后立即发信号) */
    xSemaphoreTake(req.done, portMAX_DELAY);
    const esp_err_t ret = req.result;
    vSemaphoreDelete(req.done);
    if (ret == ESP_OK) {
        *out_w = req.w;
        *out_h = req.h;
    }
    return ret;
}

/* ═══════════════ public API ═══════════════ */

esp_err_t lcd_lvgl_start(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C init failed");
    ESP_RETURN_ON_ERROR(pca9557_init(), TAG, "PCA9557 init failed");
    ESP_RETURN_ON_ERROR(bsp_display_backlight_init(), TAG, "Backlight init failed");

    if (xTaskCreate(display_task, "display", DISPLAY_TASK_STACK_BYTES, NULL,
                    DISPLAY_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "display task created");
    return ESP_OK;
}
