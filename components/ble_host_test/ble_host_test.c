#include "ble_host_test.h"
#include "ble_host.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"

#define BLE_HOST_TAG         "BLE_HOST_TEST"
#define HR_SVC_UUID16        0x180D   /* Heart Rate Service */
#define HR_MEASUREMENT_UUID  0x2A37   /* Heart Rate Measurement (notify) */
#define HR_CCCD_UUID         0x2902
#define SCAN_INTERVAL_MS     1000u    /* 演示任务：未连接时每秒尝试扫描 */
#define SCAN_STACK_BYTES     3072u
#define SCAN_TASK_PRIORITY   5

/* ── 心率广播演示 ────────────────────────────────────────────────────────
 * 扫描带心率服务(0x180D)或名字含 "HUAWEI Band" 的设备（如华为手环开启
 * 心率广播模式后，名字形如 "HUAWEI Band HR-XXXX"），连接 → 发现
 * Heart Rate Measurement 特征(0x2A37) → 订阅通知 → 解析并打印心率。
 * 未连接时每秒自动重试扫描；无需外部上位机，演示 BLE central 收数据。 */
static uint16_t s_conn_handle;
static uint16_t s_svc_end;         /* Heart Rate 服务结束 handle */
static uint16_t s_hr_val_handle;   /* 0x2A37 */
static uint16_t s_cccd_handle;
static bool     s_scanning;
static bool     s_connecting;
static bool     s_connected;
static bool     s_subscribed;
static int64_t  s_last_hr_us;      /* 上次收到心率的时间（空闲提示用） */

/* 广播里找 16 位服务 UUID（0x180D，AD type 0x02/0x03，小端 0D 18） */
static bool adv_has_hr_service(const uint8_t *adv, uint8_t adv_len)
{
    uint8_t i = 0;
    while (i + 1 < adv_len) {
        const uint8_t len = adv[i];
        if (len == 0) break;
        const uint8_t type = adv[i + 1];
        const uint8_t dlen = len - 1;
        const uint8_t *data = &adv[i + 2];
        if ((type == 0x02 || type == 0x03) && dlen >= 2) {
            for (uint8_t k = 0; k + 1 < dlen; k += 2) {
                if (data[k] == 0x0D && data[k + 1] == 0x18) return true;
            }
        }
        i += len + 1;
    }
    return false;
}

/* 从广播数据取设备名（AD type 0x08 完整名 / 0x09 短名） */
static void get_dev_name(const uint8_t *adv, uint8_t len, char *out, uint8_t out_size)
{
    out[0] = '\0';
    uint8_t i = 0;
    while (i + 1 < len) {
        const uint8_t l = adv[i];
        if (l == 0) break;
        const uint8_t type = adv[i + 1];
        const uint8_t dlen = l - 1;
        if ((type == 0x08 || type == 0x09) && dlen > 0) {
            uint8_t n = (dlen < out_size - 1) ? dlen : (out_size - 1);
            memcpy(out, &adv[i + 2], n);
            out[n] = '\0';
            return;
        }
        i += l + 1;
    }
}

/* 解析标准 BLE 心率测量（0x2A37）：flags[0] + 心率值（uint8/uint16），
 * 可选 RR 间隔（1/1024 s）——本演示只取心率值。 */
static void parse_hr_measurement(const uint8_t *d, uint16_t len)
{
    if (len < 2) {
        ESP_LOGW(BLE_HOST_TAG, "short HR packet (len=%u)", len);
        return;
    }
    const uint8_t flags = d[0];
    uint16_t hr;
    if (flags & 0x01u) {                       /* uint16 格式 */
        if (len < 3) {
            ESP_LOGW(BLE_HOST_TAG, "short uint16 HR packet (len=%u)", len);
            return;
        }
        hr = (uint16_t)(d[1] | ((uint16_t)d[2] << 8));
    } else {
        hr = d[1];
    }
    s_last_hr_us = esp_timer_get_time();
    ESP_LOGI(BLE_HOST_TAG, "心率=%u 次/分 (flags=0x%02x)", hr, flags);
}

static int write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)attr; (void)arg;
    if (error->status != 0) {
        ESP_LOGW(BLE_HOST_TAG, "write failed: %d", error->status);
    }
    return 0;
}

static int chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg);
static int dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg);

static int svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGE(BLE_HOST_TAG, "svc disc error: %d", error->status);
        return 0;
    }
    if (service != NULL) {
        s_svc_end = service->end_handle;
        ESP_LOGI(BLE_HOST_TAG, "Heart Rate svc found (0x%04x-0x%04x)",
                 service->start_handle, service->end_handle);
        ble_gattc_disc_all_chrs(conn_handle, service->start_handle,
                                service->end_handle, chr_cb, NULL);
        return 0;
    }
    if (s_hr_val_handle == 0) {
        ESP_LOGE(BLE_HOST_TAG, "Heart Rate Measurement char not found");
    }
    return 0;
}

static int chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGE(BLE_HOST_TAG, "chr disc error: %d", error->status);
        return 0;
    }
    if (chr != NULL) {
        ble_uuid16_t hr_uuid = BLE_UUID16_INIT(HR_MEASUREMENT_UUID);
        if (ble_uuid_cmp(&chr->uuid.u, &hr_uuid.u) == 0) {
            s_hr_val_handle = chr->val_handle;
        }
        return 0;
    }
    if (s_hr_val_handle != 0) {
        ESP_LOGI(BLE_HOST_TAG, "HR char found (0x%04x), discovering CCCD",
                 s_hr_val_handle);
        ble_gattc_disc_all_dscs(conn_handle, s_hr_val_handle, s_svc_end,
                                dsc_cb, NULL);
    } else {
        ESP_LOGE(BLE_HOST_TAG, "HR char not found");
    }
    return 0;
}

static int dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle; (void)arg;
    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGE(BLE_HOST_TAG, "dsc disc error: %d", error->status);
        return 0;
    }
    if (dsc != NULL) {
        ble_uuid16_t cccd = BLE_UUID16_INIT(HR_CCCD_UUID);
        if (ble_uuid_cmp(&dsc->uuid.u, &cccd.u) == 0) {
            s_cccd_handle = dsc->handle;
        }
        return 0;
    }
    if (s_cccd_handle != 0) {
        uint8_t val[2] = {0x01, 0x00};   /* 使能通知 */
        ble_gattc_write_flat(conn_handle, s_cccd_handle, val, sizeof(val),
                             write_cb, NULL);
        s_subscribed = true;
        s_last_hr_us = 0;
        ESP_LOGI(BLE_HOST_TAG, "subscribed to heart-rate notifications");
    } else {
        ESP_LOGE(BLE_HOST_TAG, "HR CCCD not found");
    }
    return 0;
}

/* GAP 事件：扫描 → 连接 → 订阅 → 收心率通知 */
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (!s_scanning) break;
        const struct ble_gap_disc_desc *d = &event->disc;
        char name[32];
        get_dev_name(d->data, d->length_data, name, sizeof(name));
        const bool has_hr = adv_has_hr_service(d->data, d->length_data);
        const bool is_hw_band = (strstr(name, "HUAWEI Band") != NULL);
        ESP_LOGI(BLE_HOST_TAG, "scan evt=%u '%s' hr_svc=%d",
                 d->event_type, name[0] ? name : "(no-name)", has_hr);
        if (has_hr || is_hw_band) {
            ESP_LOGI(BLE_HOST_TAG, "target found, connecting...");
            s_scanning = false;
            s_connecting = true;
            ble_gap_disc_cancel();
            ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &d->addr, 30000, NULL,
                            gap_event_handler, NULL);
        }
        break;
    }
    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status != 0) {
            ESP_LOGE(BLE_HOST_TAG, "connect failed: %d (will rescan)",
                     event->connect.status);
            break;
        }
        s_conn_handle = event->connect.conn_handle;
        s_connected = true;
        ESP_LOGI(BLE_HOST_TAG, "connected, discovering Heart Rate svc");
        ble_uuid16_t svc_uuid = BLE_UUID16_INIT(HR_SVC_UUID16);
        ble_gattc_disc_svc_by_uuid(s_conn_handle, &svc_uuid.u, svc_cb, NULL);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_connecting = false;
        s_subscribed = false;
        s_hr_val_handle = 0;
        s_cccd_handle = 0;
        ESP_LOGI(BLE_HOST_TAG, "disconnected; will rescan");
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.om != NULL) {
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            uint8_t buf[32];
            if (len > sizeof(buf)) len = sizeof(buf);
            os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
            parse_hr_measurement(buf, len);
        }
        break;
    default:
        break;
    }
    return 0;
}

static void start_scan(void)
{
    if (s_scanning || s_connected || s_connecting) return;
    struct ble_gap_disc_params params = {
        .filter_duplicates = 1,
        .passive = 0,
        .itvl = 0x50,
        .window = 0x30,
        .filter_policy = 0,
        .limited = 0,
    };
    const int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params,
                                gap_event_handler, NULL);
    if (rc == 0) {
        s_scanning = true;
        ESP_LOGI(BLE_HOST_TAG, "scanning for heart-rate device...");
    } else {
        ESP_LOGE(BLE_HOST_TAG, "scan start failed: %d", rc);
    }
}

/* 演示任务：未连接时每秒尝试扫描；已订阅但一段时间没数据时给提示 */
static void hr_demo_task(void *arg)
{
    (void)arg;
    bool idle_logged = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
        if (!s_connected) {
            start_scan();
            idle_logged = false;
            continue;
        }
        if (!s_subscribed) continue;
        if (s_last_hr_us == 0 || (esp_timer_get_time() - s_last_hr_us) > 3000000) {
            if (!idle_logged) {
                ESP_LOGI(BLE_HOST_TAG, "connected, waiting for heart-rate data...");
                idle_logged = true;
            }
        } else {
            idle_logged = false;
        }
    }
}

static void host_on_sync(void)
{
    start_scan();
}

esp_err_t ble_host_test_init(void)
{
    ESP_RETURN_ON_ERROR(ble_host_register_on_sync("host_test", host_on_sync),
                        BLE_HOST_TAG, "ble_host on-sync registration failed");
    xTaskCreate(hr_demo_task, "ble_hr_demo", SCAN_STACK_BYTES, NULL,
                SCAN_TASK_PRIORITY, NULL);
    ESP_LOGI(BLE_HOST_TAG, "init OK (heart-rate broadcast demo, scan interval %u ms)",
             SCAN_INTERVAL_MS);
    return ESP_OK;
}
