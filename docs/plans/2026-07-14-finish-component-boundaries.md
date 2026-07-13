# Finish Component Boundaries Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Finish the remaining component-boundary fixes without adding authentication or redesigning unrelated platform behavior.

**Architecture:** Keep `main` as the composition root. Hardware and policy remain available as components, but the default application must opt into SD explicitly, while file-system locations and WiFi startup policy are passed from the application. Preserve the existing OTA storage callback design and restore a source-compatible zero-argument firmware-only initializer.

**Tech Stack:** ESP-IDF 6.0.1, C, CMake, Python `unittest`

---

### Task 1: Make SD card initialization application opt-in

**Files:**
- Modify: `tests/test_led_decoupling.py`
- Modify: `main/main.c`
- Modify: `main/CMakeLists.txt`

**Step 1: Write the failing boundary test**

Assert that the default `main.c` and `main/CMakeLists.txt` contain no `sd_card` dependency or initialization, while `components/sd_card` remains present for user opt-in.

**Step 2: Verify RED**

Run: `python -m unittest tests.test_led_decoupling.ComponentBoundaryTests.test_default_platform_does_not_start_sd_card -v`

Expected: FAIL because `main` still links and calls `sd_card_init()`.

**Step 3: Implement the minimum change**

Remove only the default application's `sd_card` include, dependency, and initialization block. Do not delete or modify the standalone component.

**Step 4: Verify GREEN and commit**

Run the focused test and then the full Python suite.

Commit: `refactor: make SD card initialization opt-in`

### Task 2: Move file-manager storage locations to application configuration

**Files:**
- Modify: `tests/test_led_decoupling.py`
- Modify: `components/file_manager/file_manager.h`
- Modify: `components/file_manager/file_manager.c`
- Modify: `main/web_platform.c`

**Step 1: Write the failing boundary test**

Assert that `file_manager` exposes a storage configuration API and no longer contains the concrete `/littlefs`, `/sdcard`, or `storage` values.

**Step 2: Verify RED**

Run the focused test and confirm it fails on the current hard-coded constants.

**Step 3: Implement the minimum configuration API**

Add `file_manager_storage_config_t` with internal mount point, internal partition label, and optional SD mount point. Copy and validate the values once before handler registration. Keep the existing handler and guard APIs unchanged.

Configure the component from `web_platform.c` with application-owned values. An unmounted SD path remains a valid optional backend; the file manager reports it as unavailable without initializing hardware.

**Step 4: Verify GREEN and commit**

Run focused and full tests.

Commit: `refactor: inject file manager storage locations`

### Task 3: Pass WiFi startup policy from the application

**Files:**
- Modify: `tests/test_led_decoupling.py`
- Modify: `components/wifi_manager/wifi_manager.h`
- Modify: `components/wifi_manager/wifi_manager.c`
- Modify: `main/wifi_config_store.h`
- Modify: `main/wifi_config_store.c`
- Modify: `main/main.c`
- Modify: `main/web_platform.c`

**Step 1: Write the failing boundary test**

Assert that the WiFi component source no longer hard-codes the AP SSID/password, AP channel, captive-DNS choice, or SNTP server and that `main` supplies them in `wifi_manager_config_t`.

**Step 2: Verify RED**

Run the focused test and confirm the current constants are detected.

**Step 3: Separate startup policy from runtime credentials**

Define `wifi_manager_credentials_t` for STA SSID/password. Define `wifi_manager_config_t` for startup policy containing credentials, AP SSID/password/channel/max clients, captive-DNS enablement, and SNTP server. Keep runtime updates limited to `wifi_manager_credentials_t` so a Web credential change cannot overwrite AP policy.

Change the JSON store to load/save only `wifi_manager_credentials_t`. Build the startup configuration in `app_main()` and pass it to `wifi_manager_init()`.

**Step 4: Verify GREEN and commit**

Run focused and full tests.

Commit: `refactor: inject WiFi startup policy`

### Task 4: Restore the simple OTA initialization API

**Files:**
- Modify: `tests/test_led_decoupling.py`
- Modify: `components/ota_manager/ota_manager.h`
- Modify: `components/ota_manager/ota_manager.c`
- Modify: `main/web_platform.c`

**Step 1: Write the failing compatibility test**

Assert that `ota_manager_init(void)` remains available and the application uses an explicit `ota_manager_init_with_config()` only when filesystem OTA hooks are required.

**Step 2: Verify RED**

Run the focused test and confirm the current config-only signature fails it.

**Step 3: Add a compatibility wrapper**

Rename the current implementation to `ota_manager_init_with_config()` and add `ota_manager_init()` as a firmware-only wrapper that passes `NULL`. Do not change OTA behavior.

**Step 4: Verify GREEN and commit**

Run focused and full tests.

Commit: `fix: preserve simple OTA initialization`

### Task 5: Document and verify the final boundaries

**Files:**
- Modify: `README.md`

**Step 1: Update documentation**

Document explicit SD-card opt-in and show that AP/DNS/SNTP defaults live in `main`, not inside `wifi_manager`.

**Step 2: Run verification**

Run:
- `python -m unittest discover -s tests -v`
- `git diff --check`
- ESP-IDF 6.0.1 `idf.py build`
- ELF symbol checks for `led_task`, `sd_logger`, and `sd_card`

Expected: all tests and build pass; the default ELF contains none of those optional-component public symbols.

**Step 3: Commit**

Commit: `docs: explain application-owned platform options`
