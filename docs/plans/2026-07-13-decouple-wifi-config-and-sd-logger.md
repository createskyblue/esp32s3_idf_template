# Decouple WiFi Configuration And SD Logger Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make WiFi consume caller-provided credentials, move JSON persistence and LittleFS ownership to the application layer, and leave SD logging disabled unless the user explicitly selects it.

**Architecture:** `app_main()` mounts LittleFS first, loads `/littlefs/wifi_config.json` through an application-owned store, and passes a value-type `wifi_manager_config_t` into `wifi_manager_init()`. `app_storage` owns the mount lifecycle and a recursive access lease shared by stores, HTTP file access, and application-provided OTA callbacks. `wifi_manager` owns only synchronized network runtime behavior; Web handlers stage and sync JSON, apply the runtime update, then atomically commit it, falling back to the prior configuration or provisioning AP on failure. The default startup path no longer initializes `sd_logger`.

**Tech Stack:** ESP-IDF 6.0.1, C, CMake, cJSON, Python `unittest`

---

### Task 1: Define architecture regression tests

**Files:**
- Modify: `tests/test_led_decoupling.py`

**Step 1: Add failing boundary tests**

Add tests asserting that `wifi_manager` contains no `sd_logger`, cJSON, `wifi_config.h`, `/littlefs`, or stdio persistence references; its public API accepts `wifi_manager_config_t`; `app_main()` mounts storage before loading/passing credentials; and the deprecated header-macro example is replaced by valid JSON.

**Step 2: Verify RED**

Run: `python -m unittest discover -s tests -v`

Expected: FAIL on the current `sd_logger`/cJSON/file coupling, old zero-argument init API, current initialization order, and existing `wifi_config.example.h`.

### Task 2: Make WiFi a caller-configured component

**Files:**
- Modify: `components/wifi_manager/wifi_manager.h`
- Modify: `components/wifi_manager/wifi_manager.c`
- Modify: `components/wifi_manager/CMakeLists.txt`

**Step 1: Introduce the value-type configuration API**

Define `wifi_manager_config_t` with bounded `sta_ssid` and `sta_password` arrays. Change initialization to `esp_err_t wifi_manager_init(const wifi_manager_config_t *config)` and runtime updates to `esp_err_t wifi_manager_set_credentials(const wifi_manager_config_t *config)`.

**Step 2: Remove persistence and logger responsibilities**

Delete the `wifi_config.h` fallback, cJSON parsing/writing, LittleFS path, `sd_logger` include/call, `config_loaded` snapshot field, and config-path getter. Remove `sd_logger` and `cjson` from the component dependency graph.

**Step 3: Verify focused tests**

Run: `python -m unittest discover -s tests -v`

Expected: WiFi boundary/API tests pass; application-order/JSON-example tests remain failing until Task 3.

### Task 3: Move storage and credential persistence to the application

**Files:**
- Create: `main/app_storage.h`
- Create: `main/app_storage.c`
- Create: `main/wifi_config_store.h`
- Create: `main/wifi_config_store.c`
- Modify: `main/main.c`
- Modify: `main/web_platform.c`
- Modify: `main/web_platform.h`
- Modify: `main/CMakeLists.txt`

**Step 1: Give the application ownership of LittleFS**

Move the LittleFS register/info logic from `web_platform.c` to `app_storage.c`. Call `app_storage_init()` as the first operation in `app_main()`.

**Step 2: Add the JSON credential store**

Implement `wifi_config_store_load()`, `wifi_config_store_save()`, `wifi_config_store_exists()`, and `wifi_config_store_get_path()` in the application layer. Load into `wifi_manager_config_t` after mounting and pass it to `wifi_manager_init()`.

**Step 3: Update Web configuration handlers**

Use the store for path/status/persistence. Stage and sync the requested JSON, snapshot/apply the runtime configuration, then atomically commit the staged file. Discard the stage and restore the snapshot on failure. Remove WiFi initialization and LittleFS mounting from `web_platform_init()`.

**Step 4: Make SD logging opt-in**

Remove `sd_logger_init()` and the `sd_logger` dependency from the default `main` startup path. Keep the standalone component unchanged for explicit user use.

**Step 5: Verify GREEN**

Run: `python -m unittest discover -s tests -v`

Expected: PASS.

### Task 4: Remove macro configuration and document JSON provisioning

**Files:**
- Delete: `main/wifi_config.example.h`
- Create: `main/wifi_config.example.json`
- Modify: `.gitignore`
- Modify: `README.md`

**Step 1: Replace the example and instructions**

Document copying `main/wifi_config.example.json` to ignored `data/wifi_config.json`, or provisioning through the Web page. Document that SD logging is an opt-in component and that default startup does not claim the global ESP log sink.

**Step 2: Run all tests and static checks**

Run: `python -m unittest discover -s tests -v`

Run: `git diff --check`

Expected: PASS with no whitespace errors.

**Step 3: Build firmware**

Run: load `C:\esp\v6.0.1\esp-idf\export.ps1`, then run `idf.py build` and propagate its exit code.

Expected: the ESP32-S3 firmware, bootloader, partition table, and LittleFS image build successfully; generated dependency data shows no `wifi_manager -> sd_logger/cjson` edge.

### Task 5: Harden persistence and private-file boundaries after review

**Files:**
- Modify: `components/wifi_manager/wifi_manager.h`
- Modify: `components/wifi_manager/wifi_manager.c`
- Modify: `components/file_manager/file_manager.h`
- Modify: `components/file_manager/file_manager.c`
- Modify: `main/wifi_config_store.h`
- Modify: `main/wifi_config_store.c`
- Modify: `main/web_platform.c`
- Modify: `tests/test_led_decoupling.py`

**Step 1: Preserve boundary-length credentials and support rollback**

Copy validated 32-byte SSIDs and 64-byte passwords into ESP-IDF's fixed arrays with `memcpy`, expose a caller-owned configuration snapshot, and restore the previous manager/driver state when an update cannot be applied.

**Step 2: Make JSON replacement power-loss safe**

Write to a protected temporary path, flush and `fsync` it, close it, and atomically `rename` it over the live configuration. Remove the temporary file on every pre-rename failure.

**Step 3: Isolate credentials from generic HTTP file access**

Add application policy hooks to file-manager listing/downloads and apply the same private-path policy to upload, delete, and mkdir operations. Reject the live and temporary credential paths from the static-file fallback, including dot-segment aliases.

**Step 4: Prevent storage/configuration races with OTA**

Reject Web credential updates while OTA is busy. Stage and sync first, apply second, and commit by atomic rename last; if commit fails, restore the previous runtime credentials so disk, manager, and driver do not silently diverge.

**Step 5: Re-run verification**

Run the full Python test suite, `git diff --check`, and an ESP-IDF 6.0.1 build. Request an independent review of the complete working-tree diff and resolve all critical or important findings.

### Task 6: Resolve second-review concurrency and error-path findings

**Files:**
- Modify: `components/ota_manager/CMakeLists.txt`
- Modify: `components/ota_manager/ota_manager.h`
- Modify: `components/ota_manager/ota_manager.c`
- Modify: `components/file_manager/file_manager.h`
- Modify: `components/file_manager/file_manager.c`
- Modify: `components/wifi_manager/wifi_manager.h`
- Modify: `components/wifi_manager/wifi_manager.c`
- Modify: `main/app_storage.h`
- Modify: `main/app_storage.c`
- Modify: `main/wifi_config_store.c`
- Modify: `main/web_platform.c`
- Modify: `main/main.c`
- Modify: `tests/test_led_decoupling.py`

**Step 1: Make application storage ownership real**

Add a recursive application storage lease. Credential load/save acquire it, static and file-manager HTTP handlers try-acquire it, and filesystem OTA holds it exclusively from unmount through remount. Inject the partition label and lifecycle callbacks into `ota_manager`; remove its direct LittleFS dependency and all hard-coded mount behavior.

**Step 2: Serialize WiFi runtime configuration**

Guard credential snapshots, apply operations, and rollback with a mutex. Start an empty STA plus provisioning SoftAP before applying caller credentials. Replace component-level `ESP_ERROR_CHECK` calls with returned errors, and let `app_main()` stop on fundamental initialization failure or continue when the SoftAP is already running.

**Step 3: Close JSON allocation and escaping edge cases**

Check both cJSON field-allocation results before printing, and size the load buffer to round-trip maximally escaped valid 32-byte SSIDs and 64-byte passwords. Report file existence accurately rather than claiming an invalid file was loaded.

**Step 4: Verify and re-review**

Run all regression tests, whitespace checks, a warning-free ESP-IDF build, dependency/ELF checks, and a final independent review of every previous Important finding.

### Task 7: Guarantee recovery from failed filesystem and WiFi transactions

**Step 1: Keep filesystem OTA recoverable**

Permit an exclusive update lease when LittleFS is already unmounted after a failed remount. Do not abort `app_main()` when the initial mount fails; start SoftAP and the OTA API, and serve a built-in recovery upload page from the application binary.

**Step 2: Use a staged WiFi persistence transaction**

Split persistence into stage/fsync, runtime apply, and atomic commit. Discard staged data on apply failure. If commit rollback also fails, disable STA reconnects and force provisioning mode instead of claiming the old configuration was restored.
