# BatteryMonitor WebSocket JSON-RPC API

> Version: **1.0** ｜ App version: BatteryMonitor ≥ 1.0 ｜ Transport: **WebSocket** ｜ Protocol: **JSON-RPC 2.0**

BatteryMonitor is a Windows desktop battery monitoring application that aggregates battery information from Bluetooth / Xbox / HID (2.4G dongle) devices.
This document defines an in-process **WebSocket Server** that exposes a **JSON-RPC 2.0** interface for retrieving device battery snapshots, subscribing to real-time updates, triggering refreshes, and reading/writing global and per-device preferences.

- All RPC method names use **dot-namespaced** identifiers (`devices.*`, `settings.app.*`, `settings.device.*`, `system.*`).
- Enum fields are serialized as **lowercase strings** for readability; sentinel values (e.g. `-1`) are serialized as **`null`**.
- Designed for **local tool integration** (e.g. StreamDock, Home Assistant, Rainmeter, custom dashboards). Listens on `127.0.0.1` by default with optional token authentication.

---

## Table of Contents

1. [Transport & Connection](#1-transport--connection)
2. [Authentication (optional)](#2-authentication-optional)
3. [JSON-RPC 2.0 Conventions](#3-json-rpc-20-conventions)
4. [Data Model](#4-data-model)
5. [Method Overview](#5-method-overview)
6. [System Methods `system.*`](#6-system-methods-system)
7. [Device Methods `devices.*`](#7-device-methods-devices)
8. [Refresh Control `refresh.*`](#8-refresh-control-refresh)
9. [Subscriptions & Push](#9-subscriptions--push)
10. [Global Settings `settings.app.*`](#10-global-settings-settingsapp)
11. [Per-Device Settings `settings.device.*`](#11-per-device-settings-settingsdevice)
12. [Notifications (Server → Client)](#12-notifications-server--client)
13. [Error Codes](#13-error-codes)

---

## 1. Transport & Connection

| Item | Value |
| --- | --- |
| Protocol | WebSocket (RFC 6455), text frames carrying UTF-8 JSON |
| Default bind address | `127.0.0.1` (localhost only). Can be changed to `0.0.0.0` in Settings to expose on all interfaces |
| Default port | **`19211`** (configurable, range 1024–65535) |
| Connection URL | `ws://127.0.0.1:19211/` (root path) |
| Heartbeat | Clients should send a [Ping frame] or call `system.ping` (application-level) every ≤ 30s |
| Concurrency | Server runs on the application main thread (Qt event loop); requests are processed serially in arrival order |

> The server does **not** push device data automatically after connection. Use [`devices.subscribe`](#9-subscriptions--push) for real-time data.

### Enabling the Server

The server is **disabled by default**. Enable it via either:

1. **Settings page**: toggle the "WebSocket service" switch under Settings (persisted to QSettings, auto-restores on next launch). Port / host / token can be configured on the same page.
2. **Command-line arguments** (force-start for this session, not persisted):

   ```
   BatteryMonitor.exe --websocket_server                  # uses the configured port, default 19211
   BatteryMonitor.exe --websocket_server --port 8080      # overrides port for this session
   BatteryMonitor.exe --minimized --websocket_server      # starts hidden to tray with Server
   ```

   - `--websocket_server`: force-starts the WebSocket Server for this launch, **ignoring** the Settings toggle (but still reads host/token from Settings).
   - `--port <N>`: overrides the listen port (range 1024–65535); takes effect only when combined with `--websocket_server` or when the Settings toggle is on.

---

## 2. Authentication (optional)

**Disabled by default** (no auth needed on loopback). If `rpc.token` is configured in Settings (non-empty), then:

- The client's **first message** must be `system.authorize`, or the connection URL must include the query parameter `?token=<your-token>` (e.g. `ws://127.0.0.1:19211/?token=abc`).
- If authentication is not completed within 5 seconds or the token doesn't match, the server closes the connection (WebSocket Close code `1008 Policy Violation`).
- Any business request sent before authentication completes receives `error.code = -32001 Unauthorized`.

Keep auth disabled for local tools. When exposing across machines (`0.0.0.0`), always enable token authentication.

---

## 3. JSON-RPC 2.0 Conventions

Strictly follows the [JSON-RPC 2.0](https://www.jsonrpc.org/specification) specification.

### Request

```json
{ "jsonrpc": "2.0", "method": "devices.list", "params": {}, "id": 1 }
```

- `jsonrpc` must be `"2.0"`.
- `method` is required, dot-namespaced.
- `params` is optional; must be an **object** (named params). Positional array params are not supported.
- `id` may be an integer / string / `null`. **Omitting `id` or setting it to `null` marks the message as a notification** (server does not reply; used only for Server→Client push — clients should not send notifications).

### Success Response

```json
{ "jsonrpc": "2.0", "result": { /* ... */ }, "id": 1 }
```

### Error Response

```json
{ "jsonrpc": "2.0", "error": { "code": -32601, "message": "Method not found", "data": {} }, "id": 1 }
```

- `result` and `error` are **mutually exclusive**.
- `data` is optional, carrying additional diagnostic info.

---

## 4. Data Model

### 4.1 `Device` Object

Maps to the C++ `BatteryDevice` struct (`src/core/BatteryDevice.h`). This is the API's core data structure.

| Field | Type | Description |
| --- | --- | --- |
| `id` | string | **Stable unique device identifier.** Bluetooth: WinRT device Id (long string); Xbox: `XInput_<0..3>` or `xbox:raw:<id>`; HID: vendor-derived id. Clients use this to address a device. |
| `name` | string | Original human-readable device name (from system / vendor). |
| `alias` | string | User-defined alias (from `DeviceSettings`; empty string = not set). |
| `displayName` | string | Effective display name = `alias` non-empty ? `alias` : `name`. |
| `type` | string | Device category: `"bluetooth"` \| `"xbox"` \| `"hid"` |
| `subType` | string | Sub-category: `"generic"` \| `"airpods"` |
| `percentage` | number\|null | Precise battery level `0-100`; `null` means no precise value (e.g. Xbox discrete levels) — fall back to `level`. AirPods: lowest of left/right/case. |
| `level` | string | **Always populated** discrete level, for coloring / sorting / alerts: `"unknown"` \| `"empty"` \| `"low"` \| `"medium"` \| `"full"` |
| `leftPercent` | number\|null | AirPods left bud battery; `null` = unknown (single-ear use / case open). Only meaningful when `subType=="airpods"`. |
| `rightPercent` | number\|null | AirPods right bud battery. |
| `casePercent` | number\|null | AirPods charging case battery. |
| `charging` | boolean | Whether charging (any bud charging → `true`; currently only set by the AirPods path). |
| `paired` | boolean | Whether paired with this machine. Always `true` for normal devices; `false` for unpaired AirPods/Beats broadcasts. |
| `wired` | boolean | Whether on wired power (e.g. Xbox on USB has no battery). |
| `connected` | boolean | Whether the device is currently online. |
| `stale` | boolean | Whether the current value is from the **sticky cache** (provider didn't return the device this round; last reading is carried over). UI grays this out. |
| `lastSeen` | integer | Unix millisecond timestamp (epoch ms) of the last successful read. |

**Example (Bluetooth headphones):**

```json
{
  "id": "Bluetooth#BluetoothaddrXX:XX:XX:XX:XX:XX-...",
  "name": "WH-1000XM5",
  "alias": "",
  "displayName": "WH-1000XM5",
  "type": "bluetooth",
  "subType": "generic",
  "percentage": 73,
  "level": "medium",
  "leftPercent": null,
  "rightPercent": null,
  "casePercent": null,
  "charging": false,
  "paired": true,
  "wired": false,
  "connected": true,
  "stale": false,
  "lastSeen": 1720000000000
}
```

**Example (AirPods, `subType=="airpods"`):**

```json
{
  "id": "airpods:XX:XX:XX:XX:XX:XX",
  "name": "AirPods Pro",
  "alias": "",
  "displayName": "AirPods Pro",
  "type": "bluetooth",
  "subType": "airpods",
  "percentage": 45,
  "level": "low",
  "leftPercent": 60,
  "rightPercent": 45,
  "casePercent": 80,
  "charging": false,
  "paired": true,
  "wired": false,
  "connected": true,
  "stale": false,
  "lastSeen": 1720000000000
}
```

> AirPods `percentage` is the **lowest valid value** among left/right/case (`-1` / unknown paths are skipped), for sorting and low-battery alerts.

### 4.2 `level` Enum

| String | Value | Meaning |
| --- | --- | --- |
| `"unknown"` | 0 | Unknown |
| `"empty"` | 1 | Empty |
| `"low"` | 2 | Low |
| `"medium"` | 3 | Medium |
| `"full"` | 4 | Full |

### 4.3 `type` / `subType` Enums

| `type` | Meaning | Typical devices |
| --- | --- | --- |
| `"bluetooth"` | Bluetooth (BLE GATT / Classic BT A2DP / AirPods Continuity) | BT headphones, wristbands, AirPods |
| `"xbox"` | Xbox gamepad (XInput / WinRT RawGameController) | Xbox wireless controllers |
| `"hid"` | HID protocol 2.4G dongle / USB | AULA, AJAZZ, VGN, Razer keyboards/mice |

| `subType` | Meaning |
| --- | --- |
| `"generic"` | Normal device (uses only `percentage` + `level`) |
| `"airpods"` | Apple AirPods / Beats (has left/right/case triple battery) |

---

## 5. Method Overview

| Method | Direction | Description |
| --- | --- | --- |
| [`system.authorize`](#6-system-methods-system) | Req | Authenticate session (when auth is enabled) |
| [`system.ping`](#6-system-methods-system) | Req | Application-level heartbeat, returns `pong` |
| [`system.getInfo`](#6-system-methods-system) | Req | Server metadata (version, uptime, capabilities) |
| [`devices.list`](#7-device-methods-devices) | Req | Get snapshot of all devices |
| [`devices.get`](#7-device-methods-devices) | Req | Get a single device by `id` |
| [`refresh.now`](#8-refresh-control-refresh) | Req | Trigger an async refresh (non-blocking) |
| [`devices.subscribe`](#9-subscriptions--push) | Req | Subscribe to device change push |
| [`devices.unsubscribe`](#9-subscriptions--push) | Req | Unsubscribe |
| [`settings.app.get`](#10-global-settings-settingsapp) | Req | Read global preferences |
| [`settings.app.set`](#10-global-settings-settingsapp) | Req | Modify global preferences |
| [`settings.device.get`](#11-per-device-settings-settingsdevice) | Req | Read per-device preferences |
| [`settings.device.set`](#11-per-device-settings-settingsdevice) | Req | Modify per-device preferences |
| [`devices.updated`](#12-notifications-server--client) | **Notify** | Device list change push |
| [`refresh.completed`](#12-notifications-server--client) | **Notify** | Refresh cycle completed push (subscribers only) |

---

## 6. System Methods `system.*`

### `system.authorize`

Required only when the server has authentication enabled (`rpc.token` non-empty). Authentication must complete within 5 seconds of connecting, otherwise the server closes the connection.

**Request params:**

```json
{ "token": "your-token" }
```

**Response (success):**

```json
{ "authorized": true }
```

**Response (failure):** returns `-32001 Unauthorized`, then immediately closes the connection (Close code `1008`).

> Alternatively, append `?token=<your-token>` to the connection URL (e.g. `ws://127.0.0.1:19211/?token=abc`) — in this case, the first message doesn't need to call this method.

### `system.ping`

Application-level heartbeat. Server responds immediately; useful for measuring round-trip latency / keepalive.

**Request params:** `{ "payload"?: string }` (optional, echoed back)

**Response:**

```json
{ "pong": true, "payload": "abc", "timestamp": 1720000000000 }
```

### `system.getInfo`

Returns server and application metadata.

**Response:**

```json
{
  "name": "BatteryMonitor",
  "version": "1.0",
  "apiVersion": "1.0",
  "platform": "windows",
  "startedAt": 1720000000000,
  "uptime": 3600000,
  "refreshIntervalMs": 10000,
  "capabilities": ["bluetooth", "classicBluetooth", "airpods", "xbox", "hid"]
}
```

- `version`: application version (`APP_VERSION`).
- `apiVersion`: this API's version.
- `startedAt` / `uptime`: process start time and elapsed time (ms).
- `capabilities`: list of registered providers (corresponding to `addProvider` calls in `main.cpp`).

---

## 7. Device Methods `devices.*`

### `devices.list`

Returns a snapshot of all devices (the latest aggregated result from `BatteryManager`, including sticky-cached entries).

**Request params:** `{}` or omitted

**Response:**

```json
{
  "devices": [ /* Device[], see §4.1 */ ],
  "updatedAt": 1720000000000,
  "count": 3
}
```

- `updatedAt`: timestamp of when this snapshot was last published (corresponds to when `BatteryManager::devicesUpdated` fired).

**Example:**

```jsonc
// → Server
{ "jsonrpc": "2.0", "method": "devices.list", "id": 1 }
// ← Client
{ "jsonrpc": "2.0", "result": { "devices": [ /* ... */ ], "updatedAt": 1720000000000, "count": 2 }, "id": 1 }
```

### `devices.get`

Get a single device by `id`.

**Request params:**

```json
{ "id": "XInput_0" }
```

**Response:**

```json
{ "device": { /* Device, see §4.1 */ } }
```

**Error:** returns `-32004 DeviceNotFound` if the device doesn't exist.

---

## 8. Refresh Control `refresh.*`

### `refresh.now`

Triggers an **async** refresh (maps to `BatteryManager::refreshNow()`). Returns immediately; the actual refresh result is delivered via subsequent `devices.updated` push or the next `devices.list`.

**Request params:**

```json
{ "wait"?: false }
```

- `wait` (optional, default `false`): when `true`, blocks until the refresh completes (or times out after 10s), with the latest snapshot included in the response.

**Response (`wait=false`):**

```json
{ "queued": true }
```

**Response (`wait=true`):**

```json
{ "queued": true, "devices": [ /* Device[] */ ], "updatedAt": 1720000000000 }
```

> Rapid repeated calls are safe: `BatteryManager` internally deduplicates per-provider via `inFlight`/`pendingRefresh` flags.

---

## 9. Subscriptions & Push

The server does not push proactively by default. After subscribing, when the device list changes (`BatteryManager::devicesUpdated` fires), the server sends a `devices.updated` notification to the connection.

### `devices.subscribe`

**Request params:**

```json
{
  "events"?: ["devices.updated", "refresh.completed"],
  "immediate"?: true
}
```

- `events`: optional, the event whitelist to subscribe to; omitted means subscribe to all.
- `immediate`: optional, default `false`. When `true`, the server immediately sends the current snapshot right after subscription succeeds (equivalent to one `devices.list` push).

**Response:**

```json
{ "subscribed": ["devices.updated", "refresh.completed"], "subscriptionId": "s-1a2b" }
```

- `subscriptionId`: the subscription handle for this connection.

**Event types:**

| Event | Trigger condition |
| --- | --- |
| `devices.updated` | Any device list / device state field changed relative to the previous version (change detection dedup is done in `RpcServer` to avoid flooding when nothing changed) |
| `refresh.completed` | A refresh cycle completed (including periodic timer-triggered refreshes) |

### `devices.unsubscribe`

Cancels **all** subscriptions for the current connection.

**Request params:** `{}` (no params needed)

**Response:**

```json
{ "unsubscribed": ["s-1a2b"] }
```

> The current implementation manages subscriptions "per-connection" — each connection holds one subscription handle, and `unsubscribe` clears all subscriptions for that connection. Subscriptions are auto-cleaned on disconnect.

---

## 10. Global Settings `settings.app.*`

Maps to `util/AppSettings` (QSettings-backed, thread-safe).

### `settings.app.get`

**Response:** (all fields are current persisted values)

```json
{
  "refreshIntervalMs": 10000,
  "staleRetentionSec": 180,
  "language": "",
  "theme": "system",
  "hideUnpairedAirPods": true,
  "autoStart": false,
  "rpc": { "enabled": true, "host": "127.0.0.1", "port": 19211, "token": "" }
}
```

| Field | Type | Description |
| --- | --- | --- |
| `refreshIntervalMs` | integer | Polling interval (ms), default `10000` |
| `staleRetentionSec` | integer | Sticky cache retention window (seconds), default `180`; `0` = never cache |
| `language` | string | Language code; `""` = follow system (`"zh_CN"`, `"en"`, etc.) |
| `theme` | string | `"system"` \| `"light"` \| `"dark"` |
| `hideUnpairedAirPods` | boolean | Whether to hide AirPods/Beats broadcasts not paired with this machine |
| `autoStart` | boolean | Whether to start with Windows (reads/writes registry `Run` key) |
| `rpc` | object | WebSocket server's own configuration; empty `token` means no auth |

### `settings.app.set`

**Request params** (provide only the fields to modify; the rest stay unchanged):

```json
{ "refreshIntervalMs": 5000 }
```

Writable fields: `refreshIntervalMs`, `staleRetentionSec`, `language`, `theme`, `hideUnpairedAirPods`, `autoStart`, and the `rpc` sub-object.

**Response**: same structure as `settings.app.get`, returning the **updated** complete settings for client reconciliation.

**Validation:**

| Field | Constraint | On violation |
| --- | --- | --- |
| `refreshIntervalMs` | `1000`–`3600000` | `-32602 InvalidParams` (with `data.field` / `data.constraint`) |
| `staleRetentionSec` | `0`–`86400` | `-32602` (with `data`) |
| `language` | Any string accepted (no validation) | No error (unknown language codes just mean no translation file loads) |
| `theme` | ∈ {`system`, `light`, `dark`} | `-32602` (with `data`) |
| `rpc.port` | `1024`–`65535` | `-32602` (with `data`) |

> `language` / `hideUnpairedAirPods` / `autoStart` are not value-constrained; any valid type is accepted.
>
> Modifying `rpc.enabled` / `host` / `port` / `token` requires a **server restart** to fully take effect; the server adds `"needRestart": true` at the top level of the response to indicate this.

---

## 11. Per-Device Settings `settings.device.*`

Maps to `util/DeviceSettings` (persisted by SHA-256 hash of device id).

### `settings.device.get`

**Request params:**

```json
{ "id": "XInput_0" }
```

**Response:**

```json
{
  "alias": "",
  "trayVisible": true,
  "alertEnabled": true,
  "lowBatteryThreshold": 20,
  "alertPolicy": "once",
  "keepCachedForever": false
}
```

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `alias` | string | `""` | User-defined alias; empty = not set |
| `trayVisible` | boolean | `true` | Whether to show in system tray |
| `alertEnabled` | boolean | `true` | Whether low-battery alert is enabled |
| `lowBatteryThreshold` | integer | `20` | Upper battery percentage threshold for triggering alerts (1–100) |
| `alertPolicy` | string | `"once"` | Alert repeat policy |
| `keepCachedForever` | boolean | `false` | Whether to keep the device as stale indefinitely after going offline (bypasses global `staleRetentionSec`) |

### `alertPolicy` Enum

| String | Meaning |
| --- | --- |
| `"once"` | Alert once when first dropping below threshold; won't trigger again until battery recovers (default) |
| `"always"` | Alert on every refresh |
| `"5min"` | At most once every 5 minutes while below threshold |
| `"15min"` | At most once every 15 minutes |
| `"30min"` | At most once every 30 minutes |
| `"60min"` | At most once every 60 minutes |

### `settings.device.set`

**Request params:**

```json
{ "id": "XInput_0", "alias": "My Gamepad", "lowBatteryThreshold": 15 }
```

Only `id` and the fields to modify are required.

**Response**: same structure as `settings.device.get`, returning the updated complete per-device settings.

**Validation**: `lowBatteryThreshold` ∈ `1..100`, `alertPolicy` must be a valid enum value, otherwise `-32602`.

---

## 12. Notifications (Server → Client)

Notifications are JSON-RPC messages **without the `id` field**, sent unidirectionally to subscribed connections.

### `devices.updated`

Pushed when the device list changes.

```json
{
  "jsonrpc": "2.0",
  "method": "devices.updated",
  "params": {
    "devices": [ /* Device[] */ ],
    "updatedAt": 1720000000000,
    "subscriptionId": "s-1a2b"
  }
}
```

> The server performs change detection based on a "device signature" (the set of display-affecting fields) and **does not push when nothing changed**, to avoid periodic flooding. Use polling `devices.list` if you need a full snapshot every time.

### `refresh.completed`

```json
{
  "jsonrpc": "2.0",
  "method": "refresh.completed",
  "params": { "at": 1720000000000, "subscriptionId": "s-1a2b" }
}
```

---

## 13. Error Codes

Follows JSON-RPC 2.0 predefined error codes, plus application-level errors in the `-32xxx` range.

| code | message | Meaning |
| --- | --- | --- |
| `-32700` | Parse error | JSON parse failed |
| `-32600` | Invalid Request | Not a valid JSON-RPC 2.0 request |
| `-32601` | Method not found | Method does not exist or is not implemented |
| `-32602` | Invalid params | Parameter missing / wrong type / out of range |
| `-32603` | Internal error | Server internal exception |
| `-32001` | Unauthorized | Not authenticated or invalid token |
| `-32002` | Forbidden | *(reserved)* Authenticated but no permission |
| `-32003` | NotSubscribed | *(reserved)* `subscriptionId` doesn't exist or doesn't belong to this connection |
| `-32004` | DeviceNotFound | The device with the specified `id` doesn't exist |
| `-32005` | ReadOnlyField | *(reserved)* Attempted to write a read-only field |
| `-32090` | Server busy | *(reserved)* Server overloaded, retry later |
| `-32099` | Unknown error | *(reserved)* Unclassified server error |

> Codes marked *(reserved)* are defined in code but not returned by any path in the current implementation; reserved for future expansion.

The error object may carry `data`, e.g. on parameter validation failure:

```json
{
  "jsonrpc": "2.0",
  "error": {
    "code": -32602,
    "message": "Invalid params",
    "data": { "field": "refreshIntervalMs", "constraint": "1000..3600000" }
  },
  "id": 7
}
```

> `data.field` is the out-of-range field name; `data.constraint` is the valid range description; it does not include the invalid value the client sent. The `DeviceNotFound` error's `data` is `{ "id": "<requested device id>" }`.

---

*This document defines the API contract. Implementation details are authoritative in `src/rpc/` source code; any breaking change bumps `apiVersion` and is recorded in the "Version" line at the top.*
