# BLE App Connect — Reader-Side Implementation

## Overview
Add a BLE-based discovery and control layer so an iOS companion app can wirelessly manage files on the CrossPoint Reader without requiring an external WiFi network. BLE handles device discovery and AP lifecycle control; the existing WiFi AP + HTTP/WebSocket/WebDAV stack handles all file transfer at full speed.

**Problem solved:** Current file transfer requires the user to manually join a WiFi network or hotspot, find the IP, and use a browser. This feature enables a seamless one-tap connection from a native iOS app.

**Key benefits:**
- Works anywhere — no external router needed
- App discovers reader automatically via BLE advertising
- All file operations reuse existing `CrossPointWebServer` — no new transfer code
- BLE stack only active inside the new activity — zero RAM cost during reading

**Integration:** New `CONNECT_APP` option in `NetworkModeSelectionActivity`, launching `BleAppConnectActivity` which manages BLE advertising → AP startup → web server lifecycle.

**Full protocol specification:** `docs/ble-app-connect-spec.md`

## Context (from discovery)

**Files/components involved:**
- `src/activities/network/NetworkModeSelectionActivity.h/.cpp` — add 4th menu option
- `src/activities/network/CrossPointWebServerActivity.cpp` — reference for AP startup/shutdown pattern
- `src/components/themes/BaseTheme.h` — add `UIIcon::Bluetooth` to enum
- `src/components/icons/bluetooth.h` — new 32x32 bitmap icon (same format as `wifi.h`, `hotspot.h`)
- `src/components/themes/lyra/LyraTheme.cpp` — add Bluetooth case to `iconForName()` (lines 103-123)
- `lib/I18n/translations/english.yaml` — add i18n strings for BLE UI (other languages fall back to English)
- `platformio.ini` — add NimBLE-Arduino dependency

**Related patterns found:**
- Activity lifecycle: `onEnter()` alloc → `loop()` input → `onExit()` cleanup (Activity.h)
- AP startup sequence: `WiFi.mode(WIFI_AP)` → `WiFi.softAP()` → mDNS → web server (CrossPointWebServerActivity.cpp:184-238)
- AP shutdown: stop server → stop mDNS → stop DNS → disconnect AP → WiFi OFF (CrossPointWebServerActivity.cpp:61-100)
- Menu rendering: `GUI.drawList()` with `StrId` arrays and `UIIcon` arrays (NetworkModeSelectionActivity.cpp:69-78)
- Button input: `mappedInput.wasPressed(Button::Back)` / `Button::Confirm` pattern
- Battery: `powerManager.getBatteryPercentage()` (global singleton)
- MAC: `WiFi.macAddress(mac)` for 6-byte array
- Firmware version: `CROSSPOINT_VERSION` compile-time macro
- Hardware type: `gpio.deviceIsX3()` / `gpio.deviceIsX4()`

**Dependencies identified:**
- NimBLE-Arduino library (h2zero) — BLE stack for ESP32-C3
- Existing: `CrossPointWebServer`, WiFi AP code, `UITheme`, `I18n`, `HalPowerManager`

## Development Approach
- **Testing approach**: Build verification (`pio run`) + manual hardware testing (no unit test framework in project)
- Complete each task fully before moving to the next
- Make small, focused changes
- **CRITICAL: `pio run` must succeed after each task** — no moving forward with build errors
- **CRITICAL: update this plan file when scope changes during implementation**
- Maintain backward compatibility — existing file transfer modes unchanged

## Testing Strategy
- **Build**: `pio run` after each task (zero errors, zero warnings)
- **Static analysis**: `pio check` after final task
- **Format**: `clang-format` check before commit
- **Hardware testing**: Manual checklist in Post-Completion section

## Progress Tracking
- Mark completed items with `[x]` immediately when done
- Add newly discovered tasks with ➕ prefix
- Document issues/blockers with ⚠️ prefix
- Update plan if implementation deviates from original scope

## Solution Overview

**Architecture:** BLE for discovery/control + WiFi AP for data transfer.

**New components:**
1. `BleManager` (lib/hal/) — thin NimBLE wrapper: init, advertise, GATT service, callbacks via function pointers
2. `BleAppConnectActivity` (src/activities/network/) — activity managing BLE→AP→server lifecycle with 3 display states

**Connection flow:**
1. User selects "Connect via App" in network mode menu
2. `BleAppConnectActivity` starts → inits NimBLE → starts advertising
3. iOS app discovers reader via BLE → connects → reads device info
4. App writes `0x01` to AP Control characteristic
5. Reader starts WiFi AP + `CrossPointWebServer`
6. Reader notifies app with AP credentials (SSID, password, IP)
7. App joins AP programmatically → performs HTTP file operations
8. App writes `0x00` → reader stops server + AP
9. User presses Back → activity exits, BLE deinited, RAM freed

**Key design decisions:**
- NimBLE-Arduino (only BLE stack available for ESP32-C3, ~15KB RAM when active)
- Function pointer callbacks, not `std::function` (per CLAUDE.md)
- Single BLE connection limit
- Random 8-char AP password per session (transmitted over BLE only)
- BLE stack fully deinitialized on activity exit — zero residual RAM
- AP SSID: `CrossPoint-XXXX` (last 4 hex of MAC)

## Technical Details

**GATT Service UUID:** `43505200-4d47-5400-0000-000000000001`

**Characteristics:**

| Name | UUID suffix | Properties | Payload |
|------|------------|------------|---------|
| Device Info | `...0002` | Read | JSON: name, fw, hw, battery, mac (storage info deferred — see note below) |
| AP Control | `...0003` | Write + Notify | Write: `0x01`/`0x00`. Notify: JSON with AP credentials or state |
| Status | `...0004` | Read + Notify | JSON: transfer state, progress, errors |

**Note — Storage info deferred:** `HalStorage`/`SDCardManager` do not currently expose SD card capacity or free space. Adding `totalBytes()`/`freeBytes()` to the HAL requires modifying the SDK layer. Device Info will omit `storage` field for v1; the iOS app can get storage info from the HTTP `/api/status` endpoint once connected. Spec to be updated accordingly.

**Note — Status characteristic (v1 scope):** Real-time transfer progress notifications require a callback mechanism in `CrossPointWebServer` that doesn't exist yet. For v1, the Status characteristic will report `idle`/`error` states only. Full progress tracking is a follow-up after the iOS app can exercise the API. Spec to be updated.

**RAM budget:**
- NimBLE idle (advertising): ~10-15KB
- Active connection + GATT: ~15-20KB
- WiFi AP + web server (existing cost, already proven): ~40-50KB
- Total during BLE+AP: ~55-70KB additional over baseline

## What Goes Where
- **Implementation Steps**: All tasks are reader-side code changes in this codebase
- **Post-Completion**: Hardware testing, iOS app development (separate project)

## Implementation Steps

### Task 1: Add NimBLE-Arduino dependency and BLE build flags

**Files:**
- Modify: `platformio.ini`

- [x] Add `h2zero/NimBLE-Arduino @ ^2.1.1` to `lib_deps` in `[base]` section
- [x] Add BLE build flags to `[base]` build_flags: `-DCONFIG_BT_NIMBLE_ROLE_BROADCASTER=1`, `-DCONFIG_BT_NIMBLE_ROLE_OBSERVER=0`, `-DCONFIG_BT_NIMBLE_ROLE_PERIPHERAL=1`, `-DCONFIG_BT_NIMBLE_ROLE_CENTRAL=0`, `-DCONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`
- [x] Run `pio run` — must compile with NimBLE included (even if unused yet)
- [x] Check binary size output — verify app still fits in 6.25MB partition (Flash: 96.4%, 229KB headroom; NimBLE-Arduino@2.5.0 installed)

### Task 2: Add i18n strings for BLE App Connect UI

**Files:**
- Modify: `lib/I18n/translations/english.yaml`

- [x] Add translation keys: `STR_CONNECT_APP`, `STR_CONNECT_APP_DESC`, `STR_BLE_WAITING`, `STR_BLE_CONNECTED`, `STR_BLE_TRANSFERRING`, `STR_BLE_APP_CONNECT`
- [x] Run `python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/` to regenerate
- [x] Run `pio run` — must compile with new string IDs

### Task 3: Add Bluetooth icon to UIIcon enum and LyraTheme

**Files:**
- Create: `src/components/icons/bluetooth.h` (32x32 bitmap, same format as `wifi.h`, `hotspot.h`)
- Modify: `src/components/themes/BaseTheme.h`
- Modify: `src/components/themes/lyra/LyraTheme.cpp`

- [x] Create `src/components/icons/bluetooth.h` with a 32x32 `static const uint8_t BluetoothIcon[]` bitmap array (standard Bluetooth "B rune" symbol — use same encoding as existing icons like `WifiIcon`)
- [x] Add `Bluetooth` to `UIIcon` enum in `BaseTheme.h` (after `Hotspot`)
- [x] In `LyraTheme.cpp`: add `#include "components/icons/bluetooth.h"` and add `case UIIcon::Bluetooth: return BluetoothIcon;` to the `size == 32` branch of `iconForName()` (line ~120, alongside Wifi and Hotspot)
- [x] Run `pio run` — must compile

### Task 4: Add `CONNECT_APP` to NetworkMode and update menu

**Files:**
- Modify: `src/activities/network/NetworkModeSelectionActivity.h`
- Modify: `src/activities/network/NetworkModeSelectionActivity.cpp`
- Modify: `src/activities/network/CrossPointWebServerActivity.cpp`

- [x] Add `CONNECT_APP` to `NetworkMode` enum in `NetworkModeSelectionActivity.h`
- [x] Change `MENU_ITEM_COUNT` from 3 to 4 in `NetworkModeSelectionActivity.cpp`
- [x] Add `STR_CONNECT_APP` / `STR_CONNECT_APP_DESC` / `UIIcon::Bluetooth` to the menu arrays
- [x] Add `selectedIndex == 3` → `NetworkMode::CONNECT_APP` mapping in `loop()`
- [x] Handle `CONNECT_APP` in `CrossPointWebServerActivity::onNetworkModeSelected()` — for now, just log and return to mode selection (placeholder until Task 6 connects it)
- [x] Run `pio run` — must compile, menu should show 4th option

### Task 5: Create BleManager — NimBLE wrapper

**Files:**
- Create: `lib/hal/BleManager.h`
- Create: `lib/hal/BleManager.cpp`

- [x] Define `BleManager` class with: `begin()`, `end()`, `startAdvertising()`, `stopAdvertising()`, `isConnected()`, `notifyApControl(const char* json)`, `notifyStatus(const char* json)`, `setDeviceInfoJson(const char* json)`, `hasPendingCommand()`, `consumePendingCommand()`
- [x] Delete copy/move constructors and assignment operators (wraps singleton NimBLE stack)
- [x] Define callback typedefs: `using BleConnectionCallback = void (*)(bool connected, void* ctx);`
- [x] Implement `begin()`: init NimBLE, create server, create service with UUID `43505200-4d47-5400-0000-000000000001`, create 3 characteristics (Device Info read, AP Control write+notify, Status read+notify)
- [x] Implement NimBLE server callbacks for connect/disconnect using `NimBLEServerCallbacks` subclass — forward to function pointer callback
- [x] Implement AP Control characteristic write callback — **CRITICAL: do NOT execute WiFi/server operations in callback context** (NimBLE host task). Instead, store the command byte in a `volatile uint8_t pendingCommand` flag. The activity's `loop()` polls `hasPendingCommand()` and acts on the main thread.
- [x] Implement `startAdvertising()`: configure advertising data (device name, service UUID), set interval (100ms initially), start. Note: switching to slower interval after 30s requires explicit re-configuration triggered from `loop()` via millis() timer.
- [x] Implement `end()`: stop advertising, deinit NimBLE (`NimBLEDevice::deinit(true)`), free all resources
- [x] Implement `notifyApControl()` and `notifyStatus()`: set characteristic value and notify if client subscribed
- [x] Use `LOG_DBG`/`LOG_ERR` with tag `"BLE"` for all logging
- [x] Run `pio run` — must compile

### Task 6: Create BleAppConnectActivity

**Files:**
- Create: `src/activities/network/BleAppConnectActivity.h`
- Create: `src/activities/network/BleAppConnectActivity.cpp`

- [x] Define activity class extending `Activity` with states: `INITIALIZING`, `ADVERTISING`, `CONNECTED`, `AP_RUNNING`, `SHUTTING_DOWN`
- [x] Implement `onEnter()`: build device info JSON (name, `CROSSPOINT_VERSION`, hw type from `gpio.deviceIsX3()`, battery from `powerManager.getBatteryPercentage()`, MAC from `WiFi.macAddress()` — **no storage info in v1**), init `BleManager`, set device info, start advertising, render "Waiting for app..." screen. Log `ESP.getFreeHeap()` before and after BLE init.
- [x] Implement BLE connection callback: on connect → set `volatile bool` flag for main loop; on disconnect → set flag. Main `loop()` checks flags and updates state/rendering.
- [x] Implement command handling in `loop()`: poll `bleManager.hasPendingCommand()` — on command `0x01` → generate SSID (`CrossPoint-XXXX` from last 4 hex of MAC) and random 8-char alphanumeric password using `esp_random()` (hardware TRNG, not Arduino `random()`), start WiFi AP (`WiFi.softAP`), start `CrossPointWebServer`, notify AP Control with JSON `{state, ssid, pass, ip, http_port, ws_port}`, update state to `AP_RUNNING`; on `0x00` → stop web server, stop WiFi AP, update state to `ADVERTISING`
- [x] Implement `loop()` continued: handle `Button::Back` → trigger shutdown; when `AP_RUNNING` handle web server tight loop (same pattern as `CrossPointWebServerActivity::loop()` with watchdog reset every 32 iterations, yield every 64); check BLE disconnect timeout (60s)
- [x] Implement `render()`: use `renderer.getScreenWidth()`/`getScreenHeight()` for all positioning (orientation-aware). 3 display states — "Waiting for app..." (show device name + BLE icon), "App connected" (show connecting status), "Ready — transferring files" (show SSID + IP + QR code, following `CrossPointWebServerActivity::renderServerRunning()` pattern). Use `tr()` macro for all UI strings.
- [x] Implement `onExit()`: stop web server if running → stop WiFi AP if running → stop mDNS → `BleManager::end()` (full NimBLE deinit) → `WiFi.mode(WIFI_OFF)`. Log `ESP.getFreeHeap()` to verify all RAM freed.
- [x] Override `skipLoopDelay()` → true when AP running
- [x] Override `preventAutoSleep()` → true when BLE active
- [x] Run `pio run` — must compile

### Task 7: Wire BleAppConnectActivity into CrossPointWebServerActivity

**Files:**
- Modify: `src/activities/network/CrossPointWebServerActivity.cpp`
- Modify: `src/activities/network/CrossPointWebServerActivity.h` (if include needed)

- [x] Replace the placeholder `CONNECT_APP` handler from Task 4: launch `BleAppConnectActivity` as subactivity via `startActivityForResult()`
- [x] On result from `BleAppConnectActivity`: return to mode selection (same pattern as Calibre flow)
- [x] Run `pio run` — must compile

### Task 8: Build verification and code quality

**Files:**
- All modified/created files

- [x] Run `pio run -t clean && pio run` — zero errors, zero warnings
- [x] Run `pio check` — no critical static analysis findings
- [x] Run `find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i` — format all code
- [x] Verify no `.gitignore`-excluded files are staged (generated i18n headers, .pio/, etc.)
- [x] Review RAM budget: log `ESP.getFreeHeap()` in BleAppConnectActivity at key points (onEnter, after BLE init, after AP start, onExit) — values are in LOG_DBG output for hardware testing

### Task 9: Update documentation

- [x] Update `docs/ble-app-connect-spec.md` if any protocol details changed during implementation
- [x] Move this plan to `docs/plans/completed/`

## Post-Completion
*Items requiring manual intervention or external systems — no checkboxes, informational only*

**Hardware testing checklist:**
- Flash firmware to device, verify boot with no regressions
- Navigate to File Transfer → "Connect via App" — verify BLE advertising starts
- Use nRF Connect (iOS/Android) to scan — verify "CrossPoint" device appears
- Connect via nRF Connect — verify Device Info characteristic returns valid JSON
- Write `0x01` to AP Control — verify WiFi AP starts and notification received with credentials
- Join AP from phone — verify HTTP endpoints respond at `192.168.4.1`
- Upload/download a file via HTTP — verify transfer works
- Write `0x00` to AP Control — verify AP shuts down
- Press Back on reader — verify clean exit, BLE stops
- Check `ESP.getFreeHeap()` in serial output — verify >50KB free during BLE+AP operation
- Test all 4 orientations — verify display renders correctly
- Test existing file transfer modes (Join Network, Hotspot, Calibre) — verify no regressions

**iOS app development (separate project):**
- Use `docs/ble-app-connect-spec.md` as the protocol contract
- CoreBluetooth for BLE, NEHotspotConfiguration for WiFi AP joining, URLSession for HTTP
