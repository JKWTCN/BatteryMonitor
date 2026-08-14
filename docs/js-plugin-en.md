# BatteryMonitor JavaScript Plugin Guide

This guide explains how to write Windows HID battery plugins for BatteryMonitor. The repository's
[`plugins/razer/razer-basilisk-x-hyperspeed.js`](../plugins/razer/razer-basilisk-x-hyperspeed.js)
has been tested with a real Razer Basilisk X HyperSpeed 2.4 GHz receiver and is the primary example
used here.

JavaScript plugins are intended for devices whose battery can be queried through HID output/input
reports or feature reports. Bluetooth GATT, XInput, and protocols requiring filesystem, network, or
process access are outside the current plugin API.

## Installation

1. Open `plugins\` next to `BatteryMonitor.exe`. Create the directory if it does not exist.
2. Put the plugin `.js` file there or in a subdirectory at any depth. Scanning is recursive.
3. Use a lowercase `.js` extension and a filename that does not start with `.` or `_`.
4. Restart BatteryMonitor. Plugins are discovered only during application startup.

During a CMake build, the repository's complete `plugins\` tree is copied next to the executable.
The tested Razer plugin keeps this structure:

```text
plugins\razer\razer-basilisk-x-hyperspeed.js
    -> <executable directory>\plugins\razer\razer-basilisk-x-hyperspeed.js
```

The global plugin switch is enabled by default. A plugin ID is its extensionless path relative to the
`plugins\` root, with `/` as the separator. The ID above is
`razer/razer-basilisk-x-hyperspeed`. Identically named plugins in different vendor directories
therefore do not conflict.

> A plugin can send packets to matching HID devices. Incorrect commands may change device settings
> or make a device temporarily unresponsive. Install plugins only from trusted sources and test new
> protocols on non-critical hardware first.

## Plugin structure

A plugin is a UTF-8 ES module. It must export `VendorId()`, `ProductId()`, and `GetBattery()`.
`Name()` is optional.

```js
export function Name()      { return "My Battery Device"; }
export function VendorId()  { return 0x1234; }
export function ProductId() { return [0x0001, 0x0002]; }

export function GetBattery() {
    device.write([0x00, 0x04, 0x59], 65);
    const response = device.read(65, 500);
    if (!response || response[1] !== 0x04) {
        return null;
    }
    return { percentage: response[2], charging: response[3] !== 0 };
}
```

| Export | Required | Return value |
| --- | --- | --- |
| `VendorId()` | Yes | One USB VID or a non-empty VID array; each value is `1..0xffff` |
| `ProductId()` | Yes | One USB PID or a non-empty PID array; each value is `1..0xffff` |
| `GetBattery()` | Yes | A battery object, an array of battery objects, or `null`/`undefined` |
| `Name()` | No | Plugin name used in logs; defaults to `js:<plugin ID>` |

The host enumerates matching VID/PID interfaces, opens a handle, calls `GetBattery()`, and always
closes the handle afterward. Plugins do not manage enumeration or handle lifetimes. When one device
exposes several HID interfaces, the host tries candidates and remembers the last successful path.

## Returning battery data

A single-device result supports these fields:

| Field | Type | Description |
| --- | --- | --- |
| `percentage` | number | Exact battery percentage in `0..100` |
| `level` | string | `"empty"`, `"low"`, `"medium"`, or `"full"` |
| `charging` | boolean | Optional; defaults to `false` |
| `wired` | boolean | Optional; defaults to `false` |
| `name` | string | Optional replacement for the HID product string |

At least one valid `percentage` or `level` is required. A device that reports only discrete levels
can return:

```js
return { level: "medium", charging: false };
```

For a receiver managing multiple child devices, return an array. Each entry may include a string
`id` of at most 64 bytes as its stable device-ID suffix:

```js
return [
    { id: "mouse", name: "Mouse", percentage: 72 },
    { id: "keyboard", name: "Keyboard", percentage: 48, charging: true },
];
```

Returning `null`, `undefined`, or an empty array means that no device was read during this round and
is a normal result. Throwing an exception contributes to the failure counter.

## The `device` API

The host injects only one global object, `device`. All lengths include the Report ID byte. An
unnumbered report normally uses `0x00` as its first byte.

### Output/input reports

```js
device.write(packet[, length])
device.read([length[, timeoutMs]])
```

- `packet` may be an ordinary array or `Uint8Array`; every item must be in `0..255`.
- `write` zero-pads a short packet to `length` and returns the number of bytes written. Omitting the
  length uses the interface's detected report length.
- `read` returns a `Uint8Array`, or `null` on timeout/no data. Its default and maximum timeout are
  both 500 ms.
- A report is limited to 4096 bytes. Invalid arguments and HID failures throw exceptions.

### Feature reports

```js
device.sendFeature(packet[, length])
device.getFeature([length])
```

The tested Basilisk X plugin uses a 90-byte Razer payload plus one Report ID byte, for a total of 91:

```js
const REPORT_SIZE = 90;
const REPORT_WITH_ID = 91;
const request = [0x00].concat(
    makeRequest(CLASS_BATTERY, CMD_GET_BATTERY, DATA_SIZE)
);

device.sendFeature(request, REPORT_WITH_ID);
device.pause(31);
const response = device.getFeature(REPORT_WITH_ID);
```

`sendFeature` follows the same padding, length, and return-value rules as `write`. `getFeature`
returns a `Uint8Array` and throws on failure. A device or driver may return data with or without a
Report ID prefix. The tested example accepts both layouts and validates response length, status,
command class, and command ID before parsing the battery byte.

### Helpers

| API | Description |
| --- | --- |
| `device.pause(ms)` | Synchronous delay, capped at 1000 ms |
| `device.log(...values)` | Write a VERBOSE log entry |
| `device.logWarn(...values)` | Write a WARNING log entry |
| `device.logError(...values)` | Write an ERROR log entry |
| `device.productId()` | PID of the current interface |
| `device.usagePage()` / `device.usage()` | Top-level HID usage information |
| `device.name()` | HID product string |
| `device.path()` | Current HID interface path; useful for diagnostics, not as a portable ID |

## How the tested example is organized

The Basilisk X plugin demonstrates a recommended feature-report layout:

1. `VendorId()` returns Razer VID `0x1532`, while `ProductId()` matches only `0x0083`.
2. `makeRequest()` constructs the fixed-size payload and calculates the protocol CRC.
3. `send()` handles write, delay, read, Report ID normalization, response validation, and up to five
   retries.
4. `GetBattery()` calls the protocol helper and converts the raw `0..255` value to `0..100`.

This keeps transport, validation, and battery decoding separate. New plugins should validate frame
headers, lengths, echoed commands, and checksums before accepting a battery byte.

## Debugging and failures

Logs are written to `%APPDATA%\BatteryMonitor\BatteryMonitor.log`. Search for
`[js:<plugin ID>]`, `discovered plugin`, `loaded`, or `GetBattery threw`.

Common problems:

- Plugin not discovered: place it under `plugins\` next to the executable, use lowercase `.js`, and
  restart the application.
- Plugin fails to load: check the three required exports, VID/PID ranges, and JavaScript syntax.
- Device opens but produces no battery data: log `usagePage()`, `usage()`, and `path()`; verify the
  Report ID, total report length, and response offsets.
- Feature call fails: one physical device may expose multiple interfaces. Catch expected interface
  errors in `GetBattery()` and return `null` so the host can try another interface.
- Duplicate device entries: do not support the same VID/PID through both a native provider and a JS
  plugin.

One polling round has a 10-second total deadline and may return at most 32 devices. Each plugin has a
16 MB memory limit and a 256 KB stack limit. If a plugin throws and the entire round produces no
devices for five consecutive rounds, it is disabled for the current process. Restarting the
application causes it to be tried again.

## Sandbox restrictions

Plugins have no filesystem, network, or process access. There is no `std`, `os`, `fetch`, or module
loader. `Date` is removed and `Math.random()` always returns `0`. `GetBattery()` must be synchronous;
the host manages polling, enumeration, retry timing, and handle closure.
