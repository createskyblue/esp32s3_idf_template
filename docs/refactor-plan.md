# 模板解耦重构计划

> 2026-08-02 定稿。范围 = 扫描报告里用户同意的三项优先级改动。
> 原则：**增量重构，保持现有边界与架构测试通过**（`tests/test_led_decoupling.py`）。

## 目标

模板已具备良好的"平台 + 业务"分层；本次只修三处没跟上该风格的漏网之鱼：

1. `sd_card` 组件混入演示/自检代码，且与 `sd_logger` 硬编码耦合
2. HTTP/JSON 辅助逻辑在三个组件里复制三份，`components/json` 是死组件
3. 模板身份（AP 密码、`app_build_id`）跨层硬编码、重复

## 变更 1：拆解 sd_card 演示代码 + 配置化入口

**现状问题**
- `sd_card.c:133` `sd_card_init()` 挂载成功后无条件执行 `list_directory()`（`:195`，递归打印整棵目录树）和 `read_write_test()`（`:198`，往卡上写 `/sdcard/test.txt`）。
- 挂载点 `/sdcard` 在 `sd_card.c:27` 和 `sd_logger.c:23` 各写一份；`sd_logger.c:24` 另有 `LOG_DIR "/sdcard/log"`。
- 引脚 / SPI host / 速率 / 挂载点全写死，API 是 `sd_card_init(void)`，无配置结构、无 deinit。
- `sd_card.c:25` 定义 `SD_CD_GPIO`（IO14）并配置上拉（`:143`），但 `:167` 又 `gpio_cd = -1` —— 死配置。

**改动方案**
- 新增 `sd_card_config_t`（挂载点、引脚、SPI host、速率、max_files 等），`sd_card_init` 改为接收配置（保持 `sd_card_init(void)` 兼容缺省/旧调用）：
  - 方案：保留 `sd_card_init()` 作为"默认配置"入口，新增 `sd_card_init_with_config(const sd_card_config_t *)`；缺省值 = 现在的硬编码。
- 把 `list_directory()`、`read_write_test()` 从 `sd_card_init` 移除：
  - `list_directory()` 直接删除（生产不需要）；
  - `read_write_test()` 移到 `tests/` 下的示例（`components/sd_card` 内保留为编译可选的 `sd_card_self_test()`，或挪到 README 示例代码）。**决定：默认固件不执行任何遍历/写测试文件**。
- 删除死配置 `SD_CD_GPIO` 相关引脚/上拉设置（CD 未使用）。
- `sd_logger` 增加 `sd_logger_config_t`（日志目录路径），缺省仍为 `/sdcard/log`；由调用方在 `main` 里显式传入挂载点，消除两处字面量耦合。
  - 最小改动：`sd_logger_init()` 增加 `sd_logger_init_with_config()` 变体；若改动过大，退而求其次——把 `/sdcard` 常量收敛为两组件共用的头（`components/sd_card/sd_mount.h`）并让 `sd_logger` 引用。

**验收**
- `sd_card_init()` 挂载后不打印目录树、不写 `test.txt`。
- 改挂载点时两组件只改一处。

## 变更 2：HTTP/JSON helper 收进 `json` 组件

**现状问题**
- 收 body 三份：`web_platform.c:106`、`ota_manager.c:605`、`file_manager.c:235`。
- 发 JSON 三份：`web_platform.c:89`、`ota_manager.c:625`、`file_manager.c:286`。
- `copy_str` 两份：`wifi_manager.c:46`、`ota_manager.c:68`。
- `components/json`（mbedtls md5/sha256 包装头）全项目无人引用。

**改动方案**
- 在 `components/json` 新增共享辅助头/源（命名建议 `json_http.h/.c` 或 `web_helpers`）：
  - `esp_err_t json_receive_body(httpd_req_t *req, char *buf, size_t size)`（限长 + 超时重试）；
  - `esp_err_t json_send_object(httpd_req_t *req, cJSON *root)`（打印 + 发送 + Cache-Control）；
  - `esp_err_t json_send_text(httpd_req_t *req, const char *json)`；
  - `void copy_str(char *dest, size_t dest_size, const char *src)`。
- `web_platform`、`ota_manager`、`file_manager` 改为调用共享实现，删除各自的本地拷贝。
  - 注意：`file_manager.c:235` 的实现与另外两个略有差异（错误消息不同），统一时以公共版本为准，必要时保留参数区分。
- `mbedtls` 包装头若确无用，删除（保持组件内容 = 辅助库）；**若删除会影响其他工程引用，则保留但注明**。
- 各组件 CMake 增加对 `json` 组件的依赖（`ota_manager`/`file_manager` 现在是 `PRIV_REQUIRES cjson` → 改为 `REQUIRES json`）。

**验收**
- 三处不再有 `receive_json_body`/`send_json_object` 之类的重复定义。
- `components/json` 被实际引用。

## 变更 3：模板身份收敛为单一配置来源

**现状问题**
- AP 密码写死两处：`main.c:37-38` 与 `index.html:196`（`const AP_PASS = 'template1234'`）。
- `web_platform.c:243` 硬编码 `app_build_id = "esp32s3-template-v1"`。

**改动方案**
- `network.json` 增加 `ap_password` 字段（配网 AP 的密码不是敏感机密，且前端本就要显示），前端 `index.html` 改为从 `/network.json` 读取 `ap_password`，删除 `const AP_PASS` 硬编码。
  - 由 `wifi_manager_get_ap_password()` 暴露（新增 getter）。
- `app_build_id` 改为编译期宏：
  - 在 `main` 的 CMake 或 `main.h` 定义 `APP_BUILD_ID`（`CONFIG_APP_BUILD_ID` / 或直接 `#define`），默认 `"esp32s3-template-v1"`；`web_platform.c` 引用该宏。复制模板时只改一处。

**验收**
- `main.c` 改 AP 密码后，前端页面显示自动跟随，无需改 HTML。
- 改 `app_build_id` 只动一处。

## 测试与提交

- 每次变更后跑 `tests/test_led_decoupling.py`（`python tests/test_led_decoupling.py`）确认现有架构门禁不破。
- 若测试覆盖了被删除的代码（如 sd 演示代码相关断言），同步更新测试；新增针对本次改动的断言。
- 提交按变更分组，commit message 用现有风格（`refactor:` / `feat:` 前缀）。

## 可选后续（本次不做，记录防遗忘）

- `web_platform.c`（524 行）拆分：WiFi 配置事务（`:269-378`）移出平台模块。
- `ota_manager`（832 行）状态机与 HTTP handler 解耦。
- SNTP 事件对外通知钩子，供 `sd_logger_notify_time_synced()` 接入。
- 前端双页面公共 CSS。
- `ota_manager.c:26`/`:603` 重复宏、`:656` 魔法数字 257、`led_fatal_error()` 死 API 清理。
- `tests/` 增加真正的行为测试（目前只有文本正则断言）。
