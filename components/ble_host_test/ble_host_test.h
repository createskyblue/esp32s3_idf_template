#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * BLE 主机(central)心率广播演示(NimBLE)：扫描带心率服务(0x180D)或名字含
 * "HUAWEI Band" 的设备（华为手环开启心率广播后名字形如
 * "HUAWEI Band HR-XXXX"），连接 → 发现 Heart Rate Measurement 特征
 * (0x2A37) → 订阅通知 → 解析标准 BLE 心率测量并打印。未连接时每秒
 * 自动重试扫描；无需外部上位机，演示 BLE central 接收外设广播数据。
 *
 * NimBLE 下每个 GAP 操作自带事件回调，不与 BluFi 配网/echo 冲突。
 * 需在 ble_host_init() 之前调用（注册 on_sync 钩子，host sync 后自动开始
 * 扫描，由 ble_host 统一拉起 host）。
 */
esp_err_t ble_host_test_init(void);

#ifdef __cplusplus
}
#endif
