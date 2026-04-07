# CrossPoint Reader — BLE App Connect Protocol Specification

**Version:** 1.0
**Date:** 2026-04-07
**Status:** Implemented (reader-side)

## Overview

This document specifies the protocol for connecting an iOS companion app to the CrossPoint Reader for wireless file management. The architecture uses **BLE for discovery and control**, and **WiFi (AP mode) for file transfer**.

### Why Hybrid BLE + WiFi?

- **BLE** is low-power and enables automatic device discovery without any network configuration
- **WiFi** provides the throughput needed for book files (EPUBs can be 1-50 MB)
- The reader creates its own WiFi hotspot — no external router needed
- BLE transmits the hotspot credentials to the app, which joins automatically

### Connection Flow

```
iOS App                          CrossPoint Reader
   |                                    |
   |  1. BLE Scan (find "CrossPoint")   |
   |<-----------------------------------|  BLE Advertising
   |                                    |
   |  2. BLE Connect                    |
   |----------------------------------->|
   |                                    |
   |  3. Read Device Info               |
   |----------------------------------->|
   |  <-- JSON: name, fw, battery, etc  |
   |<-----------------------------------|
   |                                    |
   |  4. Write AP Control: 0x01 (start) |
   |----------------------------------->|
   |         Reader starts WiFi AP      |
   |         Reader starts HTTP server  |
   |  <-- Notify: ssid, pass, ip        |
   |<-----------------------------------|
   |                                    |
   |  5. Join WiFi AP (programmatic)    |
   |-----> [WiFi AP: CrossPoint-XXXX]   |
   |                                    |
   |  6. HTTP file operations           |
   |<=================================>|  (all transfers over WiFi)
   |                                    |
   |  7. Write AP Control: 0x00 (stop)  |
   |----------------------------------->|
   |         Reader stops HTTP server   |
   |         Reader stops WiFi AP       |
   |  <-- Notify: ap_stopped            |
   |<-----------------------------------|
   |                                    |
   |  8. BLE Disconnect                 |
   |----------------------------------->|
```

---

## BLE Specification

### Advertising

| Parameter | Value |
|-----------|-------|
| Device name | `CrossPoint-XXXX` where XXXX = last 2 bytes of MAC (e.g., `CrossPoint-A1B2`) |
| Advertising interval | 20-100ms (fast discovery). Future: switch to 500ms after 30s for power saving |
| Advertising mode | Connectable, scannable |
| TX power | Default (0 dBm) |
| Max connections | 1 (single client only) |

### GATT Service

**Service UUID:** `43505200-4d47-5400-0000-000000000001`

Mnemonic: "CPR" (CrossPoint Reader) encoded in first bytes.

#### Characteristic 1: Device Info

| Property | Value |
|----------|-------|
| UUID | `43505200-4d47-5400-0000-000000000002` |
| Properties | Read |
| Format | UTF-8 JSON |

**Response payload:**
```json
{
  "name": "CrossPoint Reader",
  "fw": "1.2.0",
  "hw": "x4",
  "battery": 85,
  "mac": "AA:BB:CC:DD:EE:FF"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | User-visible device name |
| `fw` | string | Firmware version (semver) |
| `hw` | string | Hardware revision (`"x4"` or `"x3"`) |
| `battery` | int | Battery percentage 0-100, or -1 if unknown |
| `mac` | string | BLE MAC address |

**Note:** Storage info (`total_mb`, `free_mb`) is available via the HTTP `/api/status` endpoint after WiFi connection. It is omitted from the BLE characteristic in v1 because the underlying HAL does not expose SD card capacity. A future version may add a `storage` object here.

#### Characteristic 2: AP Control

| Property | Value |
|----------|-------|
| UUID | `43505200-4d47-5400-0000-000000000003` |
| Properties | Write, Notify |
| Format | Write: single byte command. Notify: UTF-8 JSON |

**Write commands (app to reader):**

| Byte | Command | Description |
|------|---------|-------------|
| `0x01` | START_AP | Start WiFi AP and HTTP/WebSocket server |
| `0x00` | STOP_AP | Stop HTTP server and WiFi AP |

**Notify responses (reader to app):**

After `START_AP`:
```json
{
  "state": "ap_running",
  "ssid": "CrossPoint-1A2B",
  "pass": "xK9m2pLq",
  "ip": "192.168.4.1",
  "http_port": 80,
  "ws_port": 81
}
```

After `STOP_AP`:
```json
{
  "state": "ap_stopped"
}
```

On error:
```json
{
  "state": "error",
  "msg": "SD card not mounted"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `state` | string | One of: `ap_running`, `ap_stopped`, `error` |
| `ssid` | string | WiFi AP SSID (present when `ap_running`) |
| `pass` | string | WiFi AP password, 8 alphanumeric chars (present when `ap_running`) |
| `ip` | string | Reader's IP on the AP network (always `192.168.4.1`) |
| `http_port` | int | HTTP server port (default 80) |
| `ws_port` | int | WebSocket server port (default 81) |
| `msg` | string | Error description (present when `error`) |

#### Characteristic 3: Status

| Property | Value |
|----------|-------|
| UUID | `43505200-4d47-5400-0000-000000000004` |
| Properties | Read, Notify |
| Format | UTF-8 JSON |

**Status notifications (reader to app):**

**v1 (initial implementation):**
```json
{"state": "idle"}
{"state": "connected"}
{"state": "ap_running"}
{"state": "error", "msg": "ap_failed"}
{"state": "error", "msg": "server_failed"}
{"state": "error", "msg": "SD full"}
```

**Future (when transfer progress callbacks are added to CrossPointWebServer):**
```json
{"state": "uploading", "file": "book.epub", "progress": 45}
{"state": "upload_done", "file": "book.epub"}
{"state": "downloading", "file": "book.epub", "progress": 72}
{"state": "download_done", "file": "book.epub"}
```

| Field | Type | Description |
|-------|------|-------------|
| `state` | string | Current transfer state |
| `file` | string | Filename being transferred (future — not in v1) |
| `progress` | int | Transfer progress 0-100 (future — not in v1) |
| `msg` | string | Error description (present when `error`) |

**Note:** v1 reports `idle`, `connected`, `ap_running`, and `error` states. `idle_timeout` (5-minute inactivity detection) is deferred to v2 — it requires a callback mechanism in `CrossPointWebServer` to detect transfer activity. Per-file transfer progress is also deferred to v2. The iOS app can infer transfer completion from HTTP response status codes in the meantime.

---

## WiFi File Management API

Once the iOS app joins the reader's WiFi AP, all file operations use HTTP.

**Base URL:** `http://192.168.4.1` (or use IP from AP Control notification)

### File Listing

```
GET /api/files?path=/
```

Returns JSON array of files and directories at the given path.

### File Download

```
GET /download?path=/Books/book.epub
```

Returns the file content with appropriate Content-Type and Content-Disposition headers.

### File Upload

```
POST /upload
Content-Type: multipart/form-data

Fields:
  - path: destination directory (e.g., "/Books")
  - file: the file data
```

**Alternative (faster):** WebSocket binary upload on port 81. Send filename as first text frame, then binary frames for file content.

### Create Directory

```
POST /mkdir
Content-Type: application/x-www-form-urlencoded

path=/Books/NewFolder
```

### Rename File/Directory

```
POST /rename
Content-Type: application/x-www-form-urlencoded

path=/Books/old-name.epub&newName=new-name.epub
```

### Move File/Directory

```
POST /move
Content-Type: application/x-www-form-urlencoded

path=/Books/book.epub&dest=/Archive/book.epub
```

### Delete File/Directory

```
POST /delete
Content-Type: application/x-www-form-urlencoded

path=/Books/unwanted.epub
```

### Device Status

```
GET /api/status
```

Returns device status (battery, storage, etc.) as JSON.

### WebDAV (Alternative)

The reader also exposes a full WebDAV server at the root URL. iOS apps can use standard WebDAV client libraries for file operations. Supported methods: OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL, MOVE, COPY, LOCK, UNLOCK.

Protected paths (starting with `.` or named `System Volume Information`, `XTCache`) return 403 Forbidden.

---

## Error Handling and Edge Cases

### Timeouts

| Scenario | Behavior |
|----------|----------|
| App disconnects BLE mid-transfer | WiFi AP + server stay up for 60s, then auto-shutdown |
| Phone never joins WiFi AP | AP stays up while BLE connected; shuts down on BLE disconnect + 60s timeout |
| No transfers for 5 minutes | (v2) Status notifies `idle_timeout`; AP stays running but reader warns user |

### Error Conditions

| Scenario | BLE Status Notification | Reader Display |
|----------|------------------------|----------------|
| SD card not mounted | `{"state":"error","msg":"SD removed"}` | Error screen, stops server |
| SD card full (upload) | `{"state":"error","msg":"SD full"}` | Shows warning |
| Low battery (<10%) | `{"state":"error","msg":"low battery"}` | Shows warning, stays running |
| Second BLE client connects | Rejected (single connection only) | No change |

### Radio Coexistence

The ESP32-C3 has a single radio shared between BLE and WiFi. Both protocols coexist via time-slicing managed by the ESP-IDF coexistence module. During WiFi AP initialization (~500ms), BLE may briefly disconnect. The iOS app should implement:

- **BLE reconnection:** 3-second retry window after AP start command
- **Connection timeout:** 10 seconds for WiFi AP join after receiving credentials

### Security

- BLE has no pairing requirement (short-range ~10m is sufficient for this use case)
- WiFi AP password is randomly generated per session (8 alphanumeric characters)
- AP credentials are only transmitted over BLE (not broadcast)
- HTTP endpoints have no authentication (same as existing web server)
- **Future consideration:** optional PIN displayed on reader screen, entered in app via BLE characteristic

---

## iOS Implementation Notes

### Frameworks

- **CoreBluetooth** — BLE scanning, connection, GATT read/write/notify
- **NEHotspotConfiguration** — programmatic WiFi AP joining (no user prompt on iOS 15+)
- **URLSession** — HTTP file operations against reader's web server
- Alternatively, a **WebDAV client library** for full file management

### Recommended Connection Flow (iOS)

1. `CBCentralManager.scanForPeripherals(withServices: [serviceUUID])`
2. Connect to discovered peripheral
3. Discover service and characteristics
4. Read Device Info characteristic — display to user
5. Write `0x01` to AP Control, subscribe to notifications
6. On `ap_running` notification, use `NEHotspotConfiguration` to join AP
7. Perform HTTP file operations
8. Write `0x00` to AP Control when done
9. Disconnect BLE

### BLE Payload Size

All JSON payloads are designed to fit within a single BLE notification (max ~512 bytes with negotiated MTU). If MTU negotiation yields a smaller size, the reader will fragment across multiple notifications using a simple framing: each fragment ends with `\n`, the final fragment of a JSON message ends with `}\n`.

---

## Reader-Side Implementation Summary

### New Components

| Component | Location | Purpose |
|-----------|----------|---------|
| `BleAppConnectActivity` | `src/activities/network/` | Activity managing BLE + AP lifecycle |
| `BleManager` | `lib/hal/` | NimBLE wrapper: init, advertise, GATT service, callbacks |

### Dependencies

| Library | Purpose | RAM Cost |
|---------|---------|----------|
| NimBLE-Arduino | BLE stack for ESP32-C3 | ~15KB when active, 0 when deinitialized |

### Reused Components

- `CrossPointWebServer` — HTTP + WebSocket file server (no changes needed)
- `WebDAVHandler` — WebDAV file operations (no changes needed)
- WiFi AP mode startup code — extracted from existing `CrossPointWebServerActivity`
