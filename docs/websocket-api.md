# BatteryMonitor WebSocket JSON-RPC API

> 版本：**1.0** ｜ 对应应用版本：BatteryMonitor ≥ 1.0 ｜ 传输：**WebSocket** ｜ 协议：**JSON-RPC 2.0**

BatteryMonitor 是一个 Windows 桌面电池监控程序，聚合蓝牙 / Xbox / HID(2.4G 接收器) 等多种设备的电量信息。
本文档定义一套运行在本机进程内的 **WebSocket Server**，外界可通过 **JSON-RPC 2.0** 获取设备电量快照、订阅实时更新、触发刷新、读写全局与按设备的偏好设置。

- 所有 RPC 方法名采用**点分命名空间**（`devices.*`、`settings.app.*`、`settings.device.*`、`system.*`）。
- 枚举字段统一用**小写字符串**，比裸整数更易读；数值哨兵（如 `-1`）统一序列化为 **`null`**。
- 设计面向**本机工具集成**（如 StreamDock、Home Assistant、Rainmeter、自写看板），默认仅监听 `127.0.0.1`，无强认证。

---

## 目录

1. [传输与连接](#1-传输与连接)
2. [认证（可选）](#2-认证可选)
3. [JSON-RPC 2.0 约定](#3-json-rpc-20-约定)
4. [数据模型](#4-数据模型)
5. [方法总览](#5-方法总览)
6. [系统类方法 `system.*`](#6-系统类方法-system)
7. [设备类方法 `devices.*`](#7-设备类方法-devices)
8. [刷新控制 `refresh.*`](#8-刷新控制-refresh)
9. [订阅与推送](#9-订阅与推送)
10. [全局设置 `settings.app.*`](#10-全局设置-settingsapp)
11. [按设备设置 `settings.device.*`](#11-按设备设置-settingsdevice)
12. [通知（Server → Client）](#12-通知server--client)
13. [错误码](#13-错误码)

---

## 1. 传输与连接

| 项目         | 值                                                                     |
| ------------ | ---------------------------------------------------------------------- |
| 协议         | WebSocket（RFC 6455），文本帧（text frame）传输 UTF-8 JSON             |
| 默认监听地址 | `127.0.0.1`（仅本机），可在设置中改为 `0.0.0.0` 开放到所有网卡     |
| 默认端口     | **`19211`**（可配置，范围 1024–65535）                        |
| 连接 URL     | `ws://127.0.0.1:19211/`（根路径）                                    |
| 心跳         | 建议客户端每 ≤ 30s 发送一次 [Ping 帧] 或调用`system.ping`（应用层） |
| 并发         | Server 运行在应用主线程（Qt 事件循环），单请求按到达顺序串行处理       |

> 连接建立后 **不自动推送** 设备数据。需要实时数据请显式调用 [`devices.subscribe`](#9-订阅与推送)。

### 启用方式

Server **默认关闭**，可通过以下任一方式启用：

1. **设置页**：打开「Settings → WebSocket service」开关（持久化到 QSettings，下次启动自动恢复）。同页可配置端口 / host / token。
2. **命令行参数**（本次强制启动，不写盘）：

   ```
   BatteryMonitor.exe --websocket_server                  # 用设置中的端口，默认 19211
   BatteryMonitor.exe --websocket_server --port 8080      # 本次覆盖端口
   BatteryMonitor.exe --minimized --websocket_server      # 静默进托盘并启动 Server
   ```

   - `--websocket_server`：本次启动强制开启 WebSocket Server，**忽略**设置中的开关（但读取 host/token）。
   - `--port <N>`：覆盖监听端口（范围 1024–65535）；仅与 `--websocket_server` 配合或设置开关开启时生效。

---

## 2. 认证（可选）

默认**关闭**（本机回环无需鉴权）。若在设置中配置了 `rpc.token`（非空），则：

- 客户端连接后**第一条消息**必须是 `system.authorize`，或在连接 URL 上附带查询参数 `?token=<你的token>`（如 `ws://127.0.0.1:19211/?token=abc`）。
- 5 秒内未完成认证或 token 不匹配，Server 主动关闭连接（WebSocket Close code `1008 Policy Violated`）。
- 认证失败前发出的任何业务请求都会得到 `error.code = -32001 Unauthorized`。

建议本机工具保持关闭，跨机访问务必启用并配合 `0.0.0.0` 绑定。

---

## 3. JSON-RPC 2.0 约定

严格遵循 [JSON-RPC 2.0](https://www.jsonrpc.org/specification) 规范。

### 请求（Request）

```json
{ "jsonrpc": "2.0", "method": "devices.list", "params": {}, "id": 1 }
```

- `jsonrpc` 必须为 `"2.0"`。
- `method` 必填，点分命名空间。
- `params` 可选，统一使用**对象**（named params）；数组参数不支持。
- `id` 可为整数 / 字符串 / `null`。**省略 `id` 或为 `null` 视为通知**（Server 不回包，仅用于 Server→Client 推送，Client 不应发送通知）。

### 成功响应

```json
{ "jsonrpc": "2.0", "result": { /* ... */ }, "id": 1 }
```

### 错误响应

```json
{ "jsonrpc": "2.0", "error": { "code": -32601, "message": "Method not found", "data": {} }, "id": 1 }
```

- `result` 与 `error` **互斥**。
- `data` 可选，承载附加诊断信息。

---

## 4. 数据模型

### 4.1 `Device` 对象

对应 C++ `BatteryDevice`（`src/core/BatteryDevice.h`），是 API 的核心数据结构。

| 字段             | 类型         | 说明                                                                                                                                                |
| ---------------- | ------------ | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| `id`           | string       | **设备稳定唯一标识**。蓝牙为 WinRT 设备 Id（长串）；Xbox 为 `XInput_<0..3>` 或 `xbox:raw:<id>`；HID 为厂商派生 id。客户端应以它寻址设备。 |
| `name`         | string       | 设备原始可读名（来自系统 / 厂商）。                                                                                                                 |
| `alias`        | string       | 用户自定义别名（来自`DeviceSettings`，空串表示未设置）。                                                                                          |
| `displayName`  | string       | 实际展示名 =`alias` 非空 ? `alias` : `name`。                                                                                                 |
| `type`         | string       | 设备类别：`"bluetooth"` \| `"xbox"` \| `"hid"`                                                                                                |
| `subType`      | string       | 子类别：`"generic"` \| `"airpods"`                                                                                                              |
| `percentage`   | number\|null | 精确电量`0-100`；`null` 表示无精确值（如 Xbox 离散档位），此时应回退到 `level`。AirPods 取左/右/盒三者最低值。                                |
| `level`        | string       | **始终填充**的离散档位，用于着色 / 排序 / 提醒：`"unknown"` \| `"empty"` \| `"low"` \| `"medium"` \| `"full"`                       |
| `leftPercent`  | number\|null | AirPods 左耳电量；`null`=未知（单耳使用/盒盖打开）。仅 `subType=="airpods"` 有意义。                                                            |
| `rightPercent` | number\|null | AirPods 右耳电量。                                                                                                                                  |
| `casePercent`  | number\|null | AirPods 充电盒电量。                                                                                                                                |
| `charging`     | boolean      | 是否正在充电（任一路充电即`true`，目前仅 AirPods 路径会置 `true`）。                                                                            |
| `paired`       | boolean      | 是否已与本机配对。普通设备恒为`true`；AirPods/Beats 广播未配对时为 `false`。                                                                    |
| `wired`        | boolean      | 是否接有线电源（如 Xbox 接 USB 时无电池）。                                                                                                         |
| `connected`    | boolean      | 设备当前是否在线。                                                                                                                                  |
| `stale`        | boolean      | 当前展示值是否来自**粘性缓存**（本轮未读到该设备，沿用上次读数）。UI 据此标灰。                                                               |
| `lastSeen`     | integer      | 上次成功读到该设备的 Unix 毫秒时间戳（epoch ms）。                                                                                                  |

**示例（蓝牙耳机）：**

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

**示例（AirPods，`subType=="airpods"`）：**

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

> AirPods 的 `percentage` 取左/右/盒三路中**有效值最低**者（`-1` 未知路跳过），便于排序与低电量提醒。

### 4.2 `level` 枚举

| 字符串        | 数值 | 含义 |
| ------------- | ---- | ---- |
| `"unknown"` | 0    | 未知 |
| `"empty"`   | 1    | 空   |
| `"low"`     | 2    | 低   |
| `"medium"`  | 3    | 中   |
| `"full"`    | 4    | 满   |

### 4.3 `type` / `subType` 枚举

| `type`        | 含义                                                  | 典型设备                     |
| --------------- | ----------------------------------------------------- | ---------------------------- |
| `"bluetooth"` | 蓝牙（BLE GATT / 经典蓝牙 A2DP / AirPods Continuity） | 蓝牙耳机、手环、AirPods      |
| `"xbox"`      | Xbox 手柄（XInput / WinRT RawGameController）         | Xbox 无线手柄                |
| `"hid"`       | HID 协议 2.4G 接收器 / USB                            | AULA、AJAZZ、VGN、Razer 键鼠 |

| `subType`   | 含义                                        |
| ------------- | ------------------------------------------- |
| `"generic"` | 普通设备（只用`percentage` + `level`）  |
| `"airpods"` | Apple AirPods / Beats（带左/右/盒三路电量） |

---

## 5. 方法总览

| 方法                                                    | 方向             | 说明                                   |
| ------------------------------------------------------- | ---------------- | -------------------------------------- |
| [`system.authorize`](#6-系统类方法-system)             | Req              | （开启鉴权时）认证会话                 |
| [`system.ping`](#6-系统类方法-system)                  | Req              | 应用层心跳，回`pong`                 |
| [`system.getInfo`](#6-系统类方法-system)               | Req              | 服务器元信息（版本、运行时间、能力集） |
| [`devices.list`](#7-设备类方法-devices)                | Req              | 获取当前全部设备快照                   |
| [`devices.get`](#7-设备类方法-devices)                 | Req              | 按`id` 获取单个设备                  |
| [`refresh.now`](#8-刷新控制-refresh)                   | Req              | 触发一次异步刷新（不阻塞）             |
| [`devices.subscribe`](#9-订阅与推送)                   | Req              | 订阅设备变化推送                       |
| [`devices.unsubscribe`](#9-订阅与推送)                 | Req              | 取消订阅                               |
| [`settings.app.get`](#10-全局设置-settingsapp)         | Req              | 读取全局偏好                           |
| [`settings.app.set`](#10-全局设置-settingsapp)         | Req              | 修改全局偏好                           |
| [`settings.device.get`](#11-按设备设置-settingsdevice) | Req              | 读取按设备偏好                         |
| [`settings.device.set`](#11-按设备设置-settingsdevice) | Req              | 修改按设备偏好                         |
| [`devices.updated`](#12-通知server--client)            | **Notify** | 设备列表变化推送                       |
| [`refresh.completed`](#12-通知server--client)          | **Notify** | 一次刷新完成推送（订阅者）             |

---

## 6. 系统类方法 `system.*`

### `system.authorize`

仅在 Server 启用了鉴权（`rpc.token` 非空）时需要。连接后 5 秒内必须完成认证，否则 Server 关闭连接。

**请求参数**：

```json
{ "token": "你的token" }
```

**响应（成功）**：

```json
{ "authorized": true }
```

**响应（失败）**：返回 `-32001 Unauthorized`，并立即关闭连接（Close code `1008`）。

> 也可在连接 URL 上附带 `?token=<你的token>`（如 `ws://127.0.0.1:19211/?token=abc`），此时首条消息无需调用本方法。

### `system.ping`

应用层心跳。Server 立即回包，可用于测往返时延 / 保活。

**请求参数**：`{ "payload"?: string }`（可选，原样回显）

**响应**：

```json
{ "pong": true, "payload": "abc", "timestamp": 1720000000000 }
```

### `system.getInfo`

返回服务器与应用元信息。

**响应**：

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

- `version`：应用版本（`APP_VERSION`）。
- `apiVersion`：本 API 版本。
- `startedAt` / `uptime`：进程启动时间与已运行时长（ms）。
- `capabilities`：已注册的 provider 列表（与 `main.cpp` 中 `addProvider` 对应）。

---

## 7. 设备类方法 `devices.*`

### `devices.list`

返回当前全部设备快照（即 `BatteryManager` 最近一次聚合结果，含粘性缓存项）。

**请求参数**：`{}` 或省略

**响应**：

```json
{
  "devices": [ /* Device[]，见 §4.1 */ ],
  "updatedAt": 1720000000000,
  "count": 3
}
```

- `updatedAt`：本快照最近一次发布时间（对应 `BatteryManager::devicesUpdated` 的触发时刻）。

**示例：**

```jsonc
// → Client
{ "jsonrpc": "2.0", "method": "devices.list", "id": 1 }
// ← Server
{ "jsonrpc": "2.0", "result": { "devices": [ /* ... */ ], "updatedAt": 1720000000000, "count": 2 }, "id": 1 }
```

### `devices.get`

按 `id` 获取单个设备。

**请求参数**：

```json
{ "id": "XInput_0" }
```

**响应**：

```json
{ "device": { /* Device，见 §4.1 */ } }
```

**错误**：设备不存在返回 `-32004 DeviceNotFound`。

---

## 8. 刷新控制 `refresh.*`

### `refresh.now`

请求一次**异步**刷新（映射到 `BatteryManager::refreshNow()`）。立即返回，实际刷新结果通过后续 `devices.updated` 推送或下次 `devices.list` 体现。

**请求参数**：

```json
{ "wait"?: false }
```

- `wait`（可选，默认 `false`）：为 `true` 时阻塞至本次刷新完成（或超时 10s）再返回，响应中携带最新快照。

**响应（`wait=false`）：**

```json
{ "queued": true }
```

**响应（`wait=true`）：**

```json
{ "queued": true, "devices": [ /* Device[] */ ], "updatedAt": 1720000000000 }
```

> 重复快速调用是安全的：`BatteryManager` 内部对每个 provider 有 `inFlight`/`pendingRefresh` 去重。

---

## 9. 订阅与推送

Server 默认不主动推送。订阅后，设备列表发生变化（`BatteryManager::devicesUpdated` 触发）时，Server 会向该连接发送 `devices.updated` 通知。

### `devices.subscribe`

**请求参数**：

```json
{
  "events"?: ["devices.updated", "refresh.completed"],
  "immediate"?: true
}
```

- `events`：可选，订阅的事件白名单；省略表示订阅全部。
- `immediate`：可选，默认 `false`。为 `true` 时订阅成功后**立即**补发一次当前快照（等价于一次 `devices.list` 推送）。

**响应**：

```json
{ "subscribed": ["devices.updated", "refresh.completed"], "subscriptionId": "s-1a2b" }
```

- `subscriptionId`：本次订阅句柄，用于 `unsubscribe`。

**事件类型：**

| 事件                  | 触发条件                                                                                                 |
| --------------------- | -------------------------------------------------------------------------------------------------------- |
| `devices.updated`   | 设备列表 / 任一设备状态字段相对上一版发生变化（`BatteryManager` 内部已做变化检测去重，避免无变化刷屏） |
| `refresh.completed` | 一轮刷新周期完成（含定时器触发的周期刷新）                                                               |

### `devices.unsubscribe`

取消当前连接的**所有**订阅。

**请求参数**：`{}`（无需参数）

**响应**：

```json
{ "unsubscribed": ["s-1a2b"] }
```

> 当前实现为「按连接」管理订阅——每个连接持有一个订阅句柄，`unsubscribe` 会清除该连接全部订阅。连接断开时自动注销，无需显式取消。

---

## 10. 全局设置 `settings.app.*`

对应 `util/AppSettings`（QSettings 持久化，线程安全）。

### `settings.app.get`

**响应：**（所有字段均为当前持久化值）

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

| 字段                    | 类型    | 说明                                                   |
| ----------------------- | ------- | ------------------------------------------------------ |
| `refreshIntervalMs`   | integer | 轮询间隔（ms），默认`10000`                          |
| `staleRetentionSec`   | integer | 粘性缓存保留窗口（秒），默认`180`；`0`=从不缓存    |
| `language`            | string  | 语言代码；`""`=跟随系统（`"zh_CN"` / `"en"` 等） |
| `theme`               | string  | `"system"` \| `"light"` \| `"dark"`              |
| `hideUnpairedAirPods` | boolean | 是否隐藏未与本机配对的 AirPods/Beats 广播              |
| `autoStart`           | boolean | 是否开机自启（读写注册表`Run` 项）                   |
| `rpc`                 | object  | WebSocket 服务自身配置；`token` 为空表示不启用鉴权   |

### `settings.app.set`

**请求参数**（只需提供要修改的字段，其余保持不变）：

```json
{ "refreshIntervalMs": 5000 }
```

可写字段：`refreshIntervalMs`、`staleRetentionSec`、`language`、`theme`、`hideUnpairedAirPods`、`autoStart`，以及 `rpc` 子对象。

**响应**：与 `settings.app.get` 相同结构，返回**更新后**的完整设置，便于客户端对账。

**校验**：

| 字段                  | 约束                               | 不满足时                                                            |
| --------------------- | ---------------------------------- | ------------------------------------------------------------------- |
| `refreshIntervalMs` | `1000`–`3600000`              | `-32602 InvalidParams`（含 `data.field` / `data.constraint`） |
| `staleRetentionSec` | `0`–`86400`                   | `-32602`（含 `data`）                                           |
| `language`          | 任意字符串均可（不校验是否已安装） | 不报错（未知语言代码仅意味着加载不到翻译文件）                      |
| `theme`             | ∈ {`system`,`light`,`dark`} | `-32602`（含 `data`）                                           |
| `rpc.port`          | `1024`–`65535`                | `-32602`（含 `data`）                                           |

> `language` / `hideUnpairedAirPods` / `autoStart` 不做值约束，任意合法类型即可写入。
>
> 修改 `rpc.enabled` / `host` / `port` / `token` 后**需重启** Server 才能完全生效；Server 会在响应顶层追加 `"needRestart": true` 提示。

---

## 11. 按设备设置 `settings.device.*`

对应 `util/DeviceSettings`（按设备 id 的 SHA-256 哈希键持久化）。

### `settings.device.get`

**请求参数**：

```json
{ "id": "XInput_0" }
```

**响应**：

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

| 字段                    | 类型    | 默认       | 说明                                                                |
| ----------------------- | ------- | ---------- | ------------------------------------------------------------------- |
| `alias`               | string  | `""`     | 用户自定义别名；空表示未设置                                        |
| `trayVisible`         | boolean | `true`   | 是否显示到系统托盘                                                  |
| `alertEnabled`        | boolean | `true`   | 是否启用低电量提醒                                                  |
| `lowBatteryThreshold` | integer | `20`     | 触发提醒的电量上限（1–100）                                        |
| `alertPolicy`         | string  | `"once"` | 提醒重复策略                                                        |
| `keepCachedForever`   | boolean | `false`  | 掉线后是否以 stale 值永久保留（不受全局`staleRetentionSec` 约束） |

### `alertPolicy` 枚举

| 字符串       | 含义                                             |
| ------------ | ------------------------------------------------ |
| `"once"`   | 首次跌破阈值提醒一次，回升后才会再次触发（默认） |
| `"always"` | 每次刷新都提醒                                   |
| `"5min"`   | 跌破期间最多每 5 分钟一次                        |
| `"15min"`  | 最多每 15 分钟一次                               |
| `"30min"`  | 最多每 30 分钟一次                               |
| `"60min"`  | 最多每 60 分钟一次                               |

### `settings.device.set`

**请求参数**：

```json
{ "id": "XInput_0", "alias": "我的手柄", "lowBatteryThreshold": 15 }
```

仅需提供 `id` 与要修改的字段。

**响应**：与 `settings.device.get` 相同结构，返回更新后的完整按设备设置。

**校验**：`lowBatteryThreshold` ∈ `1..100`、`alertPolicy` 为合法枚举，否则 `-32602`。

---

## 12. 通知（Server → Client）

通知是 **`id` 字段缺省** 的 JSON-RPC 消息，单向发给已订阅的连接。

### `devices.updated`

设备列表发生变化时推送。

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

> Server 内部已基于「设备签名」（影响显示的关键字段集合）做变化检测，**无变化不推送**，避免周期刷屏。需要每次完整快照请改用轮询 `devices.list`。

### `refresh.completed`

```json
{
  "jsonrpc": "2.0",
  "method": "refresh.completed",
  "params": { "at": 1720000000000, "subscriptionId": "s-1a2b" }
}
```

---

## 13. 错误码

遵循 JSON-RPC 2.0 预定义错误码，并定义应用层错误（`-32xxx` 段）。

| code       | message          | 含义                                                   |
| ---------- | ---------------- | ------------------------------------------------------ |
| `-32700` | Parse error      | JSON 解析失败                                          |
| `-32600` | Invalid Request  | 不是合法的 JSON-RPC 2.0 请求                           |
| `-32601` | Method not found | 方法不存在或未实现                                     |
| `-32602` | Invalid params   | 参数缺失 / 类型错误 / 越界                             |
| `-32603` | Internal error   | Server 内部异常                                        |
| `-32001` | Unauthorized     | 未认证或 token 无效                                    |
| `-32002` | Forbidden        | *（保留）* 已认证但无权限                            |
| `-32003` | NotSubscribed    | *（保留）* `subscriptionId` 不存在或不属于当前连接 |
| `-32004` | DeviceNotFound   | 指定`id` 的设备不存在                                |
| `-32005` | ReadOnlyField    | *（保留）* 试图写入只读字段                          |
| `-32090` | Server busy      | *（保留）* Server 过载，请稍后重试                   |
| `-32099` | Unknown error    | *（保留）* 未分类的服务端错误                        |

> 标注 *（保留）* 的错误码已在代码中定义，但当前实现尚未在任何路径返回，预留给未来扩展。

错误对象可携带 `data`，例如参数校验失败时：

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

> `data` 中 `field` 为越界字段名，`constraint` 为合法区间描述；不包含客户端传入的非法值本身。`DeviceNotFound` 错误的 `data` 则为 `{ "id": "<请求的设备id>" }`。

---

*本文档定义 API 契约。实现细节以 `src/rpc/` 源码为准；任何破坏性变更将提升 `apiVersion` 并在本文顶部「版本」记录。*
