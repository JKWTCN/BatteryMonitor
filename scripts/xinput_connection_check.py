#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
验证 XInput 手柄/接收器在 Windows 中上报为“无线”还是“有线”。

检查内容：
1. XInputGetCapabilities().Flags 中是否包含 XINPUT_CAPS_WIRELESS
2. XInputGetBatteryInformation() 返回的 BatteryType / BatteryLevel
3. 枚举 XInput 用户槽位 0~3

仅依赖 Python 标准库。建议使用 Windows 8/10/11 自带的 xinput1_4.dll。
"""

from __future__ import annotations

import argparse
import ctypes
import os
import sys
import time
from ctypes import wintypes


ERROR_SUCCESS = 0
ERROR_DEVICE_NOT_CONNECTED = 1167

XINPUT_FLAG_GAMEPAD = 0x00000001

XINPUT_CAPS_FFB_SUPPORTED = 0x0001
XINPUT_CAPS_WIRELESS = 0x0002
XINPUT_CAPS_VOICE_SUPPORTED = 0x0004
XINPUT_CAPS_PMD_SUPPORTED = 0x0008
XINPUT_CAPS_NO_NAVIGATION = 0x0010

BATTERY_DEVTYPE_GAMEPAD = 0x00

BATTERY_TYPE_DISCONNECTED = 0x00
BATTERY_TYPE_WIRED = 0x01
BATTERY_TYPE_ALKALINE = 0x02
BATTERY_TYPE_NIMH = 0x03
BATTERY_TYPE_UNKNOWN = 0xFF

BATTERY_LEVEL_EMPTY = 0x00
BATTERY_LEVEL_LOW = 0x01
BATTERY_LEVEL_MEDIUM = 0x02
BATTERY_LEVEL_FULL = 0x03


class XINPUT_GAMEPAD(ctypes.Structure):
    _fields_ = [
        ("wButtons", wintypes.WORD),
        ("bLeftTrigger", wintypes.BYTE),
        ("bRightTrigger", wintypes.BYTE),
        ("sThumbLX", ctypes.c_short),
        ("sThumbLY", ctypes.c_short),
        ("sThumbRX", ctypes.c_short),
        ("sThumbRY", ctypes.c_short),
    ]


class XINPUT_VIBRATION(ctypes.Structure):
    _fields_ = [
        ("wLeftMotorSpeed", wintypes.WORD),
        ("wRightMotorSpeed", wintypes.WORD),
    ]


class XINPUT_CAPABILITIES(ctypes.Structure):
    _fields_ = [
        ("Type", wintypes.BYTE),
        ("SubType", wintypes.BYTE),
        ("Flags", wintypes.WORD),
        ("Gamepad", XINPUT_GAMEPAD),
        ("Vibration", XINPUT_VIBRATION),
    ]


class XINPUT_BATTERY_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BatteryType", wintypes.BYTE),
        ("BatteryLevel", wintypes.BYTE),
    ]


BATTERY_TYPE_NAMES = {
    BATTERY_TYPE_DISCONNECTED: "DISCONNECTED（未连接）",
    BATTERY_TYPE_WIRED: "WIRED（有线/由 USB 供电）",
    BATTERY_TYPE_ALKALINE: "ALKALINE（碱性电池）",
    BATTERY_TYPE_NIMH: "NIMH（镍氢电池）",
    BATTERY_TYPE_UNKNOWN: "UNKNOWN（未知）",
}

BATTERY_LEVEL_NAMES = {
    BATTERY_LEVEL_EMPTY: "EMPTY（空）",
    BATTERY_LEVEL_LOW: "LOW（低）",
    BATTERY_LEVEL_MEDIUM: "MEDIUM（中）",
    BATTERY_LEVEL_FULL: "FULL（满）",
}

SUBTYPE_NAMES = {
    0x00: "UNKNOWN",
    0x01: "GAMEPAD",
    0x02: "WHEEL",
    0x03: "ARCADE_STICK",
    0x04: "FLIGHT_STICK",
    0x05: "DANCE_PAD",
    0x06: "GUITAR",
    0x07: "GUITAR_ALTERNATE",
    0x08: "DRUM_KIT",
    0x0B: "GUITAR_BASS",
    0x13: "ARCADE_PAD",
}


class XInputApi:
    def __init__(self, dll_name: str) -> None:
        self.dll_name = dll_name
        self.dll = ctypes.WinDLL(dll_name)

        self.get_capabilities = self.dll.XInputGetCapabilities
        self.get_capabilities.argtypes = [
            wintypes.DWORD,
            wintypes.DWORD,
            ctypes.POINTER(XINPUT_CAPABILITIES),
        ]
        self.get_capabilities.restype = wintypes.DWORD

        self.get_battery = getattr(self.dll, "XInputGetBatteryInformation", None)
        if self.get_battery is not None:
            self.get_battery.argtypes = [
                wintypes.DWORD,
                wintypes.BYTE,
                ctypes.POINTER(XINPUT_BATTERY_INFORMATION),
            ]
            self.get_battery.restype = wintypes.DWORD


def load_xinput(preferred: str | None = None) -> XInputApi:
    candidates = []
    if preferred:
        candidates.append(preferred)

    candidates.extend(["xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"])

    seen: set[str] = set()
    errors: list[str] = []

    for dll_name in candidates:
        key = dll_name.lower()
        if key in seen:
            continue
        seen.add(key)

        try:
            api = XInputApi(dll_name)
        except (OSError, AttributeError) as exc:
            errors.append(f"{dll_name}: {exc}")
            continue

        if api.get_battery is not None:
            return api

        errors.append(f"{dll_name}: 不导出 XInputGetBatteryInformation")

    raise RuntimeError(
        "无法加载可用的 XInput DLL。\n  " + "\n  ".join(errors)
    )


def format_capability_flags(flags: int) -> str:
    names = []
    known_flags = (
        (XINPUT_CAPS_FFB_SUPPORTED, "FFB"),
        (XINPUT_CAPS_WIRELESS, "WIRELESS"),
        (XINPUT_CAPS_VOICE_SUPPORTED, "VOICE"),
        (XINPUT_CAPS_PMD_SUPPORTED, "PMD"),
        (XINPUT_CAPS_NO_NAVIGATION, "NO_NAVIGATION"),
    )
    for mask, name in known_flags:
        if flags & mask:
            names.append(name)
    return ", ".join(names) if names else "无已知标志"


def classify(reported_wireless: bool, battery_type: int | None) -> str:
    if battery_type is None:
        return (
            "无法读取电池接口；只能依据 Capabilities 标志判断。"
            if reported_wireless
            else "无法读取电池接口，且未设置无线标志；结果不确定。"
        )

    if reported_wireless:
        if battery_type == BATTERY_TYPE_WIRED:
            return "结果矛盾：Capabilities 表示无线，但 BatteryType 表示有线。"
        return "XInput 明确将其上报为无线设备。"

    if battery_type == BATTERY_TYPE_WIRED:
        return (
            "XInput 将其上报为有线设备。若实际使用 2.4G dongle，"
            "说明 dongle 在 XInput 层模拟了有线控制器。"
        )

    if battery_type in (BATTERY_TYPE_ALKALINE, BATTERY_TYPE_NIMH):
        return (
            "未设置无线标志，但返回了无线电池类型；驱动/固件上报不一致，"
            "物理上很可能仍是无线设备。"
        )

    if battery_type == BATTERY_TYPE_DISCONNECTED:
        return "电池接口认为设备未连接，可能是不支持该接口或状态刚发生变化。"

    return (
        "未设置无线标志，电池类型也未知；XInput 无法证明它是无线设备，"
        "真实电量可能位于 dongle 的厂商自定义 HID 报告中。"
    )


def query_slot(api: XInputApi, user_index: int) -> dict[str, object] | None:
    caps = XINPUT_CAPABILITIES()
    result = int(
        api.get_capabilities(
            user_index,
            XINPUT_FLAG_GAMEPAD,
            ctypes.byref(caps),
        )
    )

    if result == ERROR_DEVICE_NOT_CONNECTED:
        return None
    if result != ERROR_SUCCESS:
        return {
            "index": user_index,
            "error": f"XInputGetCapabilities 返回 {result}",
        }

    battery_type: int | None = None
    battery_level: int | None = None
    battery_error: int | None = None

    if api.get_battery is not None:
        battery = XINPUT_BATTERY_INFORMATION()
        battery_result = int(
            api.get_battery(
                user_index,
                BATTERY_DEVTYPE_GAMEPAD,
                ctypes.byref(battery),
            )
        )
        if battery_result == ERROR_SUCCESS:
            battery_type = int(battery.BatteryType)
            battery_level = int(battery.BatteryLevel)
        else:
            battery_error = battery_result

    flags = int(caps.Flags)
    reported_wireless = bool(flags & XINPUT_CAPS_WIRELESS)

    return {
        "index": user_index,
        "type": int(caps.Type),
        "subtype": int(caps.SubType),
        "flags": flags,
        "reported_wireless": reported_wireless,
        "battery_type": battery_type,
        "battery_level": battery_level,
        "battery_error": battery_error,
        "conclusion": classify(reported_wireless, battery_type),
    }


def print_snapshot(api: XInputApi) -> int:
    print(f"使用 DLL: {api.dll_name}")
    print("=" * 72)

    connected = 0
    for index in range(4):
        result = query_slot(api, index)
        if result is None:
            print(f"[槽位 {index}] 未连接")
            continue

        connected += 1
        print(f"[槽位 {index}] 已连接")

        if "error" in result:
            print(f"  错误: {result['error']}")
            continue

        subtype = int(result["subtype"])
        flags = int(result["flags"])
        battery_type = result["battery_type"]
        battery_level = result["battery_level"]
        battery_error = result["battery_error"]

        print(
            f"  Type/SubType       : 0x{int(result['type']):02X} / "
            f"0x{subtype:02X} ({SUBTYPE_NAMES.get(subtype, '未识别')})"
        )
        print(
            f"  Capabilities.Flags : 0x{flags:04X} "
            f"({format_capability_flags(flags)})"
        )
        print(
            "  XINPUT_CAPS_WIRELESS: "
            + ("是" if result["reported_wireless"] else "否")
        )

        if battery_error is not None:
            print(f"  电池查询           : 调用失败，错误码 {battery_error}")
        elif battery_type is None:
            print("  电池查询           : 当前 DLL 不支持")
        else:
            bt = int(battery_type)
            bl = int(battery_level)
            print(
                f"  BatteryType        : 0x{bt:02X} "
                f"({BATTERY_TYPE_NAMES.get(bt, '未识别')})"
            )
            print(
                f"  BatteryLevel       : 0x{bl:02X} "
                f"({BATTERY_LEVEL_NAMES.get(bl, '未识别')})"
            )

        print(f"  结论               : {result['conclusion']}")
        print("-" * 72)

    if connected == 0:
        print("没有发现已连接的 XInput 控制器。")
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="检查 XInput 手柄被上报为无线还是有线。"
    )
    parser.add_argument(
        "--dll",
        help="指定 XInput DLL，例如 xinput1_4.dll",
    )
    parser.add_argument(
        "--watch",
        action="store_true",
        help="持续监测；按 Ctrl+C 退出",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=1.0,
        help="监测间隔秒数，默认 1.0",
    )
    args = parser.parse_args()

    if os.name != "nt":
        print("错误：此脚本只能在 Windows 上运行。", file=sys.stderr)
        return 2

    try:
        api = load_xinput(args.dll)
    except RuntimeError as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 2

    if not args.watch:
        return print_snapshot(api)

    try:
        while True:
            os.system("cls")
            print_snapshot(api)
            print("\n持续监测中，按 Ctrl+C 退出。")
            time.sleep(max(args.interval, 0.1))
    except KeyboardInterrupt:
        print("\n已停止。")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
