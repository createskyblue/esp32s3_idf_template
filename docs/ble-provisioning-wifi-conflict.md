# BLE 配网在"WiFi 配置错误/不可达"时失效 —— 根因排查记录

> 日期：2026-08-23 会话。保存此文档以防上下文压缩丢失结论。
> 状态：**根因已定位到 NimBLE 具体代码行**；应用层已用 workaround 缓解（STA 安静），
> 等待验证是否可向 espressif/esp-idf 提交 issue。

## 一、用户可复现的现象（铁证）

| WiFi 状态 | BLE 配网（BluFi + 微信小程序）结果 |
|-----------|----------------------------------|
| 未开 WiFi / 无配置 | ✅ 正常 |
| 配置正确的 WiFi（STA 连上后空闲） | ✅ 正常 |
| 配置错误的 WiFi（STA 反复连接尝试） | ❌ 失效：手机能连上、能订阅，但按"扫描 WiFi 列表"无任何反应 |

同一个手机、同一个固件，只改 WiFi 状态结果就不同 → **变量只有一个：STA 是否处于"连接尝试"状态**。
（曾经怀疑 esp_blufi 协议版本 1.3/1.4 与小程序不兼容，被此反证排除——版本问题与 WiFi 状态无关。）

## 二、症状链（已用串口日志确认）

1. 手机连入配网 BLE：**链路层连接成功**（MTU 协商、GATT 订阅、手机→板子写入全部正常，GET_WIFI_LIST 能被处理并触发扫描）。
2. 但 esp_blufi 的 GAP 回调收到的是 **`connection failed; status=26`**，而不是成功事件。
3. → esp_blufi 内部 `blufi_env.is_connected` 永远为 `false`（只有 status==0 的连接事件才置 true）。
4. → 发送函数 `btc_blufi_send_encap()`（`blufi_prf.c`）开头：
   ```c
   if (blufi_env.is_connected == false) {
       BTC_TRACE_ERROR("blufi connection has been disconnected");
       return;   // 静默丢弃
   }
   ```
   所有**板子→手机**的数据（WiFi 状态报告、WiFi 列表）被丢弃；**接收方向不查这个标志**，所以手机发的东西都被正常处理。
5. → 手机收不到任何响应 → App"按了没反应"。

心率演示正常的原因：那是 `ble_host_test`（板子当 central 连手环）的**另一条独立链路**，且是接收方向，与 esp_blufi 无关——恰好证明无线链路本身没问题。

## 三、根因：status=26 的精确来源（已追到代码行）

**文件**：`components/bt/host/nimble/nimble/nimble/host/src/ble_gap.c`
**函数**：`ble_gap_rx_rd_rem_ver_info_complete()`（LE Read Remote Version Information Complete 事件处理）
**第 3619 行**：
```c
} else {
    if (conn != NULL) {
        ...
        ble_gap_event_connect_call(ev->conn_handle, ev->status);   // ← status=26 在这里进入连接事件
        conn->slave_conn = 1;
    } else if (ev->status != 0) {
        ble_gap_event_connect_call(ev->conn_handle, ev->status);
    }
}
```

**机制**：
1. 外设（slave）连接建立成功后，NimBLE 再发 HCI 命令 **"LE Read Remote Version Information"** 读对端版本。
2. 这条 HCI 事务的完成事件携带 `ev->status`，被 NimBLE **原样当作"连接事件"的 status** 投递给广播回调（即 esp_blufi 的 GAP 回调）。
3. STA 反复连接尝试期间，**Wi-Fi/BT 共存仲裁器压制蓝牙无线**，这条 HCI 交换被干扰/超时 → 控制器报 **0x1A = 26（HCI "Unsupported Feature or Parameter Value"）** → 整个连接被误报为失败。

**要点**：连接本身是成功的（LE Connection Complete 成功、GATT 全通）。NimBLE 用"读对端版本"这条**后续 HCI 事务**的成败来定义连接成败，这是一个设计缺陷；在 Wi-Fi/BT 共存争抢下被触发。

`BLE_GAP_EVENT_CONNECT` 的所有 status 赋值点（核对过）：
- `ble_gap_master_connect_failure(status)`（central 侧失败，与 blufi 外设无关）
- `ble_gap_master_connect_cancelled`（EAPP=9 / ETIMEOUT=13）
- `ble_gap_rx_conn_comp_failed`（固定 62=CONN_ESTABLISHMENT）
- `ble_gap_rx_rd_rem_ver_info_complete`（**ev->status，26 的唯一入口**）
- `ble_gap_rx_disconn_complete` 附近（EAGAIN=1，断连场景）

## 四、应用层配合（最终设计，2026-08-23 定稿）

根因已由 NimBLE 补丁根治后，应用层只做一件事：**扫描时临时让出无线**。

- `wifi_manager`：普通退避重连（5s→10s→20s→…，最大 5 分钟），不打断；
  提供 `wifi_manager_suspend_sta()` / `wifi_manager_resume_sta()`。
  已移除"连续 NO_AP_FOUND 后永久暂停"的旧逻辑（路由器临时关机应保持自动重连）。
- `blufi_provisioning`：
  - `ESP_BLUFI_EVENT_GET_WIFI_LIST`（手机明确请求扫描）→ `suspend_sta()` → `esp_wifi_scan_start`
  - `WIFI_EVENT_SCAN_DONE` → `send_wifi_list()` → `resume_sta()`（扫描结束立即恢复）
  - 扫描启动失败 → 立即 `resume_sta()`
  - 不再在 BLE_CONNECT/DISCONNECT 时挂起（普通重连不受配网会话影响）
- `s_ble_connected` 在任意手机事件到达时置位（不依赖可能缺失的 BLE_CONNECT 事件）。

## 五、上游检查结论：bug 已在 mynewt-nimble master 修复（2026-08-23 联网核实）

对比 apache/mynewt-nimble master 的 `nimble/host/src/ble_gap.c`（194KB，已大重构）：

```c
// 上游 master（连接事件生成处，约 2149 行）
event.type = BLE_GAP_EVENT_CONNECT;
event.connect.conn_handle = evt->connection_handle;
event.connect.status = 0;                       // ← 固定为 0！
ble_gap_event_listener_call(&event);
ble_gap_call_conn_event_cb(&event, evt->connection_handle);
ble_gap_rd_rem_sup_feat_tx(evt->connection_handle);  // ← 读版本/特性改为连接事件之后独立事务
```

- 上游把连接事件 status **固定为 0**（已建立的连接必报成功），
  **不再用 "Read Remote Version" HCI 的 ev->status 覆盖连接事件**；
  读版本/特性改成连接事件投递之后的独立 HCI 事务，失败也不影响连接状态。
- 我们 v6.1-beta1 的 NimBLE 子模块提交为 `1fd2e39`（旧版），含第 3619 行的旧逻辑（bug）。
- ESP-IDF 侧迁移提交：`e08609e451` "fix(nimble): Migrate to NimBLE 1.9.0"（2026-07-25）。

**结论：不向 espressif/esp-idf 提交重复 issue**（上游已修复，属于旧版 NimBLE 的已知缺陷）。
升级到含 NimBLE 1.9.0 的 IDF 即可根除；当前可保留应用层 workaround（STA 安静）。

## 五·补、硬件验证（2026-08-23，1 行补丁实测）✅ 根因实锤

在本地 v6.1-beta1 的 NimBLE 打 1 行补丁（与上游语义一致）：

```c
// C:\esp\v6.1-beta1\esp-idf\components\bt\host\nimble\nimble\nimble\host\src\ble_gap.c
// ble_gap_rx_rd_rem_ver_info_complete() 的 slave 分支：
ble_gap_event_connect_call(ev->conn_handle, 0);   // 原为 ev->status，改为 0
```

**结果（PC 模拟手机 + 抓包）**：
- 补丁前：PC 连上后收 0 条通知（发送被 is_connected=false 丢弃）
- 补丁后：**PC 收到 WiFi 列表通知（158 字节，含 18+ 个真实 SSID）** —— 完整闭环：
  GET_WIFI_LIST → 扫描 → 发送列表 → PC 收到。
- 证明：连接事件恢复为 status=0 → esp_blufi is_connected=true → 发送不再被丢弃。

**根因实锤**：status=26 确实来自 ble_gap.c:3619（读版本 HCI 状态污染连接事件）；
上游修复（status 固定 0 + 版本读取独立事务）确实能根治。

**补丁注意事项**：
- 这是对 IDF 安装目录的本地修改，标有 `[local-patch nimble-connect-status]` 注释；
  升级/重装 IDF 会丢失，需重新打或改用含 NimBLE 1.9.0 的 IDF。
- 有了这个根治补丁，应用层的"STA 连续失败后安静"workaround **不再是必需**
  （连接事件在任何 WiFi 状态下都干净了）；用户若担心"路由器关机 10 分钟"场景，
  可以移除安静逻辑，只保留 suspend-on-connect + 扫描前挂起。

## 六、排查过程要点（防重复踩坑）

- `idf.py monitor` 默认会 reset 板子 → 测"STA 空闲"条件时必须用原始串口读取（pyserial），不要开 monitor。
- python 输出重定向到文件会被缓冲，强杀进程时丢失 → 用 `python -u`。
- 本项目 ESP32-S3 为 `SOC_MPI_SUPPORTED=y`，esp_blufi 写路径走内联 `btc_blufi_recv_handler`（不是队列路径）。
- 本机已装 IDF：v6.1-beta1（`C:\esp\v6.1-beta1\esp-idf`）；6.0.1 的 bt 组件未安装，无法对比旧版 esp_blufi。
- PC（Windows bleak）连接板子会使板子挂死（疑似 Windows 配对请求触发 NimBLE SMP 路径死锁），与手机场景无关，勿混淆。
