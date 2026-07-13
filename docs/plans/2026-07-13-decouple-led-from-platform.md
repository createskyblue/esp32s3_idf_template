# Decouple LED From Platform Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Keep every LED GPIO free in the default firmware while preserving `led_task` as an explicitly selected, standalone component.

**Architecture:** Remove LED policy and hardware ownership from WiFi, HTTP, and the default application startup path. Keep `led_task` independent, make every declared LED available through its public command API, and document the explicit dependency/init steps users take when they want it.

**Tech Stack:** ESP-IDF 6.0.1, C, CMake, Python `unittest`

---

### Task 1: Add LED ownership regression tests

**Files:**
- Create: `tests/test_led_decoupling.py`

**Step 1: Write the failing tests**

Add tests that scan platform C/CMake files and report any LED dependency, API use, or direct board-LED GPIO claim outside `components/led_task`. Treat application-owned files (`main.c`, `hello_web.*`, and `main/CMakeLists.txt`) as the explicit opt-in boundary. Add focused assertions that the optional component does not reserve `LED_GREEN` from `led_send_cmd()` and that the default configuration does not require an application idle hook.

**Step 2: Run tests to verify they fail**

Run: `python -m unittest discover -s tests -v`

Expected: FAIL because `wifi_manager`, `web_platform`, and `main` currently reference LED behavior, and `led_send_cmd()` rejects `LED_GREEN`.

### Task 2: Remove default LED ownership

**Files:**
- Modify: `components/wifi_manager/CMakeLists.txt`
- Modify: `components/wifi_manager/wifi_manager.c`
- Modify: `main/CMakeLists.txt`
- Modify: `main/web_platform.c`
- Modify: `main/main.c`
- Modify: `components/led_task/led_task.c`
- Modify: `components/led_task/led_task.h`
- Modify: `sdkconfig.defaults`

**Step 1: Decouple WiFi and HTTP**

Remove `led_task` from `wifi_manager` dependencies, remove LED commands from WiFi event handling, and remove the JSON-response activity flash from `web_platform`.

**Step 2: Make LED opt-in**

Remove `led_task` from the default application's dependencies and startup, remove the idle-hook heartbeat that directly drives GPIO 6, and disable the now-unused FreeRTOS application idle-hook requirement.

**Step 3: Return all LEDs to the optional component API**

Initialize all four declared LEDs uniformly and accept commands for `LED_GREEN` just like the other IDs.

**Step 4: Run tests to verify they pass**

Run: `python -m unittest discover -s tests -v`

Expected: PASS.

### Task 3: Document and build the result

**Files:**
- Modify: `README.md`

**Step 1: Update documentation**

Describe LED support as optional, state that the default firmware does not claim the LED GPIOs, and show the explicit CMake/include/init calls required to enable the component.

**Step 2: Run architecture tests**

Run: `python -m unittest discover -s tests -v`

Expected: PASS.

**Step 3: Build the firmware**

Run: load `C:\esp\v6.0.1\esp-idf\export.ps1`, then run `idf.py build` and propagate its exit code.

Expected: the ESP32-S3 application, bootloader, partition table, and LittleFS image build successfully with no LED dependency in `main` or `wifi_manager`.
