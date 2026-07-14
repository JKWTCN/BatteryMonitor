#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
读取 Razer Mouse Dock Pro 所连接鼠标的状态、电量和充电状态。

设备：
    VID = 0x1532
    PID = 0x00A4

依赖：
    python -m pip install hidapi

使用：
    python razer_mouse_dock_battery_v3.py
    python razer_mouse_dock_battery_v3.py --watch --interval 2
    python razer_mouse_dock_battery_v3.py --verbose

说明：
    - hidapi 仅用于枚举 HID 顶层集合路径；
    - 实际 Feature Report 通信使用 Windows HidD_SetFeature/HidD_GetFeature；
    - 自动读取 HIDP_CAPS.FeatureReportByteLength，不再写死 90/91 字节；
    - 系统鼠标集合无法以读写方式打开时，会回退到 desiredAccess=0；
    - 默认不打印原始发送包和接收包。
"""

from __future__ import annotations

import argparse
import ctypes
import itertools
import os
import sys
import time
from dataclasses import dataclass
from typing import Optional

try:
    import hid
except ImportError:
    print("未安装 hidapi，请先执行：")
    print("python -m pip install hidapi")
    raise SystemExit(1)


VID = 0x1532
PID = 0x00A4

# USB 抓包中的 Razer 私有协议报告本体长度，不含 Windows API 缓冲区开头的 Report ID。
RAZER_REPORT_SIZE = 90
REPORT_ID = 0x00

ARGUMENT_OFFSET = 8
ARGUMENT_CAPACITY = 80
CRC_OFFSET = 88

STATUS_NEW_COMMAND = 0x00
STATUS_BUSY = 0x01
STATUS_SUCCESS = 0x02
STATUS_FAILURE = 0x03
STATUS_NOT_SUPPORTED = 0x04
STATUS_TIMEOUT = 0x05

STATUS_NAMES = {
    STATUS_NEW_COMMAND: "新命令",
    STATUS_BUSY: "设备忙",
    STATUS_SUCCESS: "成功",
    STATUS_FAILURE: "失败",
    STATUS_NOT_SUPPORTED: "不支持、鼠标离线或不可用",
    STATUS_TIMEOUT: "超时",
}

# command class, command id, data size
COMMAND_PAIRED_DEVICES = (0x00, 0xBF, 0x50)
COMMAND_BATTERY = (0x07, 0x80, 0x02)
COMMAND_CHARGING = (0x07, 0x84, 0x02)

_TRANSACTION_IDS = itertools.cycle(range(1, 0xFF))


class RazerProtocolError(RuntimeError):
    pass


class WindowsHidError(OSError):
    pass


@dataclass(frozen=True)
class PairedSlot:
    index: int
    online: bool
    pid: int

    @property
    def paired(self) -> bool:
        return self.pid != 0xFFFF


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [
        ("Usage", ctypes.c_ushort),
        ("UsagePage", ctypes.c_ushort),
        ("InputReportByteLength", ctypes.c_ushort),
        ("OutputReportByteLength", ctypes.c_ushort),
        ("FeatureReportByteLength", ctypes.c_ushort),
        ("Reserved", ctypes.c_ushort * 17),
        ("NumberLinkCollectionNodes", ctypes.c_ushort),
        ("NumberInputButtonCaps", ctypes.c_ushort),
        ("NumberInputValueCaps", ctypes.c_ushort),
        ("NumberInputDataIndices", ctypes.c_ushort),
        ("NumberOutputButtonCaps", ctypes.c_ushort),
        ("NumberOutputValueCaps", ctypes.c_ushort),
        ("NumberOutputDataIndices", ctypes.c_ushort),
        ("NumberFeatureButtonCaps", ctypes.c_ushort),
        ("NumberFeatureValueCaps", ctypes.c_ushort),
        ("NumberFeatureDataIndices", ctypes.c_ushort),
    ]


@dataclass(frozen=True)
class HidCaps:
    usage_page: int
    usage: int
    input_report_length: int
    output_report_length: int
    feature_report_length: int


class WindowsFeatureDevice:
    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    FILE_SHARE_READ = 0x00000001
    FILE_SHARE_WRITE = 0x00000002
    OPEN_EXISTING = 3
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

    def __init__(self, path: bytes | str):
        if os.name != "nt":
            raise RuntimeError("此脚本仅支持 Windows")

        if isinstance(path, bytes):
            self.path = path.decode("utf-8", errors="strict")
        else:
            self.path = path

        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.hid_dll = ctypes.WinDLL("hid", use_last_error=True)

        self.handle: Optional[int] = None
        self.open_access: Optional[int] = None
        self.caps: Optional[HidCaps] = None

        self._configure_api()

    def _configure_api(self) -> None:
        self.kernel32.CreateFileW.argtypes = [
            ctypes.c_wchar_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_void_p,
        ]
        self.kernel32.CreateFileW.restype = ctypes.c_void_p

        self.kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
        self.kernel32.CloseHandle.restype = ctypes.c_int

        self.hid_dll.HidD_GetPreparsedData.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        self.hid_dll.HidD_GetPreparsedData.restype = ctypes.c_ubyte

        self.hid_dll.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
        self.hid_dll.HidD_FreePreparsedData.restype = ctypes.c_ubyte

        self.hid_dll.HidP_GetCaps.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(HIDP_CAPS),
        ]
        self.hid_dll.HidP_GetCaps.restype = ctypes.c_long

        self.hid_dll.HidD_SetFeature.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_ulong,
        ]
        self.hid_dll.HidD_SetFeature.restype = ctypes.c_ubyte

        self.hid_dll.HidD_GetFeature.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_ulong,
        ]
        self.hid_dll.HidD_GetFeature.restype = ctypes.c_ubyte

    @staticmethod
    def _format_windows_error(operation: str, code: Optional[int] = None) -> WindowsHidError:
        if code is None:
            code = ctypes.get_last_error()
        message = ctypes.FormatError(code).strip() if code else "未知 Windows 错误"
        return WindowsHidError(code, f"{operation} 失败：{message}")

    def _try_create_file(self, desired_access: int) -> Optional[int]:
        ctypes.set_last_error(0)
        handle = self.kernel32.CreateFileW(
            self.path,
            desired_access,
            self.FILE_SHARE_READ | self.FILE_SHARE_WRITE,
            None,
            self.OPEN_EXISTING,
            0,
            None,
        )

        if handle == self.INVALID_HANDLE_VALUE or handle is None:
            return None
        return int(handle)

    def open(self) -> None:
        # 普通 vendor collection 通常允许读写；系统鼠标/键盘集合经常拒绝读写打开。
        access_attempts = [
            self.GENERIC_READ | self.GENERIC_WRITE,
            0,
        ]
        errors: list[tuple[int, int]] = []

        for desired_access in access_attempts:
            handle = self._try_create_file(desired_access)
            if handle is not None:
                self.handle = handle
                self.open_access = desired_access
                try:
                    self.caps = self._read_caps()
                except Exception:
                    self.close()
                    raise
                return

            errors.append((desired_access, ctypes.get_last_error()))

        details = ", ".join(
            f"access=0x{access:08X}: {ctypes.FormatError(code).strip()}"
            for access, code in errors
        )
        raise WindowsHidError(5, f"CreateFileW 无法打开集合（{details}）")

    def _read_caps(self) -> HidCaps:
        if self.handle is None:
            raise WindowsHidError("设备尚未打开")

        preparsed = ctypes.c_void_p()
        ctypes.set_last_error(0)
        ok = self.hid_dll.HidD_GetPreparsedData(
            ctypes.c_void_p(self.handle),
            ctypes.byref(preparsed),
        )
        if not ok:
            raise self._format_windows_error("HidD_GetPreparsedData")

        try:
            native_caps = HIDP_CAPS()
            status = self.hid_dll.HidP_GetCaps(
                preparsed,
                ctypes.byref(native_caps),
            )
            # HidP_GetCaps 返回 NTSTATUS；负值表示失败。
            if status < 0:
                raise WindowsHidError(
                    status,
                    f"HidP_GetCaps 失败：NTSTATUS=0x{status & 0xFFFFFFFF:08X}",
                )

            return HidCaps(
                usage_page=int(native_caps.UsagePage),
                usage=int(native_caps.Usage),
                input_report_length=int(native_caps.InputReportByteLength),
                output_report_length=int(native_caps.OutputReportByteLength),
                feature_report_length=int(native_caps.FeatureReportByteLength),
            )
        finally:
            self.hid_dll.HidD_FreePreparsedData(preparsed)

    @property
    def feature_report_length(self) -> int:
        if self.caps is None:
            raise WindowsHidError("尚未读取 HID 能力信息")
        return self.caps.feature_report_length

    def close(self) -> None:
        if self.handle is not None:
            self.kernel32.CloseHandle(ctypes.c_void_p(self.handle))
            self.handle = None
            self.open_access = None
            self.caps = None

    def _build_windows_buffer(self, razer_report: Optional[bytes] = None):
        report_length = self.feature_report_length
        minimum_length = RAZER_REPORT_SIZE + 1

        if report_length < minimum_length:
            raise WindowsHidError(
                1,
                "该顶层集合不支持雷蛇 90 字节 Feature Report："
                f"FeatureReportByteLength={report_length}，至少需要 {minimum_length}",
            )

        buffer_type = ctypes.c_ubyte * report_length
        buffer = buffer_type()
        buffer[0] = REPORT_ID

        if razer_report is not None:
            if len(razer_report) != RAZER_REPORT_SIZE:
                raise ValueError(f"Razer 报告必须为 {RAZER_REPORT_SIZE} 字节")
            for index, value in enumerate(razer_report, start=1):
                buffer[index] = value

        return buffer

    def set_feature(self, razer_report: bytes) -> None:
        if self.handle is None:
            raise WindowsHidError("设备尚未打开")

        buffer = self._build_windows_buffer(razer_report)
        ctypes.set_last_error(0)
        ok = self.hid_dll.HidD_SetFeature(
            ctypes.c_void_p(self.handle),
            ctypes.cast(buffer, ctypes.c_void_p),
            self.feature_report_length,
        )
        if not ok:
            raise self._format_windows_error("HidD_SetFeature")

    def get_feature(self) -> bytes:
        if self.handle is None:
            raise WindowsHidError("设备尚未打开")

        buffer = self._build_windows_buffer()
        ctypes.set_last_error(0)
        ok = self.hid_dll.HidD_GetFeature(
            ctypes.c_void_p(self.handle),
            ctypes.cast(buffer, ctypes.c_void_p),
            self.feature_report_length,
        )
        if not ok:
            raise self._format_windows_error("HidD_GetFeature")

        # Windows API 缓冲区的第 0 字节是 Report ID；后面才是抓包中的 90 字节。
        return bytes(buffer[1:1 + RAZER_REPORT_SIZE])

    def __enter__(self) -> "WindowsFeatureDevice":
        self.open()
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()


# -----------------------------------------------------------------------------
# Razer 协议
# -----------------------------------------------------------------------------

def calculate_crc(report: bytes | bytearray) -> int:
    value = 0
    for byte in report[2:88]:
        value ^= byte
    return value


def build_report(
    command_class: int,
    command_id: int,
    data_size: int,
    arguments: bytes = b"",
) -> bytes:
    if not 0 <= data_size <= ARGUMENT_CAPACITY:
        raise ValueError("data_size 超出范围")
    if len(arguments) > data_size:
        raise ValueError("参数长度大于 data_size")

    report = bytearray(RAZER_REPORT_SIZE)
    report[0] = STATUS_NEW_COMMAND
    report[1] = next(_TRANSACTION_IDS)
    report[2] = 0x00
    report[3] = 0x00
    report[4] = 0x00
    report[5] = data_size
    report[6] = command_class
    report[7] = command_id
    report[ARGUMENT_OFFSET:ARGUMENT_OFFSET + len(arguments)] = arguments
    report[CRC_OFFSET] = calculate_crc(report)
    report[89] = 0x00
    return bytes(report)


def response_matches(response: bytes, request: bytes) -> bool:
    return (
        len(response) == RAZER_REPORT_SIZE
        and response[1] == request[1]
        and response[6] == request[6]
        and response[7] == request[7]
    )


def status_text(status: int) -> str:
    return STATUS_NAMES.get(status, f"未知状态 0x{status:02X}")


def send_command(
    device: WindowsFeatureDevice,
    command_class: int,
    command_id: int,
    data_size: int,
    arguments: bytes = b"",
    timeout_ms: int = 1500,
) -> bytes:
    request = build_report(
        command_class=command_class,
        command_id=command_id,
        data_size=data_size,
        arguments=arguments,
    )

    device.set_feature(request)

    deadline = time.monotonic() + timeout_ms / 1000.0
    last_response: Optional[bytes] = None
    last_error: Optional[Exception] = None

    while time.monotonic() < deadline:
        time.sleep(0.01)
        try:
            response = device.get_feature()
        except OSError as error:
            last_error = error
            continue

        last_response = response

        # Synapse 同时运行时可能读到别的事务，按事务号和命令过滤。
        if not response_matches(response, request):
            continue

        if response[0] == STATUS_BUSY:
            continue

        if response[CRC_OFFSET] != calculate_crc(response):
            raise RazerProtocolError("设备响应 CRC 校验失败")

        return response

    if last_response is not None:
        raise RazerProtocolError(
            f"等待响应超时：0x{command_class:02X}/0x{command_id:02X}，"
            "读取到的响应不属于当前事务"
        )

    if last_error is not None:
        raise RazerProtocolError(
            f"等待响应超时：0x{command_class:02X}/0x{command_id:02X}；"
            f"最后一次读取错误：{last_error}"
        )

    raise RazerProtocolError(
        f"等待响应超时：0x{command_class:02X}/0x{command_id:02X}"
    )


def require_success(response: bytes, operation: str) -> None:
    if response[0] != STATUS_SUCCESS:
        raise RazerProtocolError(
            f"{operation}失败：status=0x{response[0]:02X} "
            f"({status_text(response[0])})"
        )


def parse_paired_slots(response: bytes) -> list[PairedSlot]:
    require_success(response, "查询配对设备")

    data_size = min(response[5], ARGUMENT_CAPACITY)
    arguments = response[ARGUMENT_OFFSET:ARGUMENT_OFFSET + data_size]
    if not arguments:
        return []

    slot_count = arguments[0]
    slots: list[PairedSlot] = []

    for slot_index in range(slot_count):
        offset = 1 + slot_index * 3
        if offset + 3 > len(arguments):
            break

        online_value = arguments[offset]
        mouse_pid = (arguments[offset + 1] << 8) | arguments[offset + 2]
        slots.append(
            PairedSlot(
                index=slot_index + 1,
                online=online_value != 0,
                pid=mouse_pid,
            )
        )

    return slots


def parse_battery(response: bytes) -> tuple[int, float, int]:
    require_success(response, "查询电量")

    raw_value = response[ARGUMENT_OFFSET + 1]
    exact_percent = raw_value * 100.0 / 255.0
    display_percent = round(exact_percent)
    return raw_value, exact_percent, display_percent


def parse_charging(response: bytes) -> tuple[Optional[bool], int]:
    require_success(response, "查询充电状态")

    raw_value = response[ARGUMENT_OFFSET + 1]
    if raw_value == 0x00:
        return False, raw_value
    if raw_value == 0x01:
        return True, raw_value
    return None, raw_value


# -----------------------------------------------------------------------------
# 枚举与接口选择
# -----------------------------------------------------------------------------

def path_text(info: dict) -> str:
    value = info.get("path", b"")
    if isinstance(value, bytes):
        return value.decode("ascii", errors="ignore")
    return str(value)


def describe_interface(info: dict) -> str:
    return (
        f"interface={info.get('interface_number', -1)}, "
        f"usage_page=0x{info.get('usage_page', 0):04X}, "
        f"usage=0x{info.get('usage', 0):04X}, "
        f"path={info.get('path')!r}"
    )


def candidate_priority(info: dict) -> tuple[int, int, int, str]:
    text = path_text(info).upper()
    interface = info.get("interface_number", -1)
    usage_page = info.get("usage_page", 0)
    usage = info.get("usage", 0)

    # 抓包中的控制传输 wIndex=0，优先探测 MI_00 的标准鼠标顶层集合。
    if interface == 0 and usage_page == 0x0001 and usage == 0x0002:
        return (0, 0, 0, text)
    if "MI_00" in text and "&COL" not in text:
        return (1, interface, usage, text)
    if "MI_00" in text:
        return (2, interface, usage, text)
    return (3, interface, usage, text)


def enumerate_candidates(verbose: bool) -> list[dict]:
    devices = hid.enumerate(VID, PID)
    if not devices:
        raise RuntimeError(
            f"未找到 Razer Mouse Dock Pro：VID=0x{VID:04X}, PID=0x{PID:04X}"
        )

    candidates = sorted(devices, key=candidate_priority)
    if verbose:
        print("检测到以下 Mouse Dock Pro HID 顶层集合：")
        for info in candidates:
            print(f"  {describe_interface(info)}")
        print()

    return candidates


def access_text(access: Optional[int]) -> str:
    if access is None:
        return "未打开"
    if access == 0:
        return "desiredAccess=0"
    return "读写"


def open_working_interface(
    candidates: list[dict],
    timeout_ms: int,
    verbose: bool,
) -> tuple[WindowsFeatureDevice, dict, bytes]:
    errors: list[str] = []

    for info in candidates:
        path = info.get("path")
        if not path:
            continue

        device = WindowsFeatureDevice(path)
        try:
            device.open()
            caps = device.caps
            assert caps is not None

            if verbose:
                print(
                    "探测："
                    f"{describe_interface(info)}，"
                    f"caps usage=0x{caps.usage_page:04X}/0x{caps.usage:04X}，"
                    f"input={caps.input_report_length}，"
                    f"output={caps.output_report_length}，"
                    f"feature={caps.feature_report_length}，"
                    f"打开方式={access_text(device.open_access)}"
                )

            if caps.feature_report_length < RAZER_REPORT_SIZE + 1:
                errors.append(
                    f"{describe_interface(info)} -> "
                    f"FeatureReportByteLength={caps.feature_report_length}"
                )
                device.close()
                continue

            command_class, command_id, data_size = COMMAND_PAIRED_DEVICES
            response = send_command(
                device,
                command_class,
                command_id,
                data_size,
                timeout_ms=timeout_ms,
            )

            if response[0] == STATUS_SUCCESS:
                return device, info, response

            errors.append(
                f"{describe_interface(info)} -> status=0x{response[0]:02X}"
            )

        except Exception as error:
            caps_text = ""
            if device.caps is not None:
                caps_text = (
                    f" [feature={device.caps.feature_report_length}, "
                    f"access={access_text(device.open_access)}]"
                )
            errors.append(f"{describe_interface(info)}{caps_text} -> {error}")

        device.close()

    raise RuntimeError(
        "找到了底座，但没有顶层集合能完成雷蛇 Feature Report 通信。\n"
        "请先完全退出 Razer Synapse 和所有 Razer 后台程序后重试。\n"
        + "\n".join(errors)
    )


# -----------------------------------------------------------------------------
# 查询与输出
# -----------------------------------------------------------------------------

def query_status(
    device: WindowsFeatureDevice,
    timeout_ms: int,
    first_pair_response: Optional[bytes] = None,
) -> None:
    if first_pair_response is None:
        cls, cmd, size = COMMAND_PAIRED_DEVICES
        pair_response = send_command(device, cls, cmd, size, timeout_ms=timeout_ms)
    else:
        pair_response = first_pair_response

    slots = parse_paired_slots(pair_response)
    paired_slots = [slot for slot in slots if slot.paired]
    online_slots = [slot for slot in paired_slots if slot.online]

    print("=" * 64)
    print(time.strftime("%Y-%m-%d %H:%M:%S"))

    if not paired_slots:
        print("鼠标：未配对")
        return

    for slot in paired_slots:
        state = "在线" if slot.online else "离线或已关机"
        print(f"鼠标：槽位 {slot.index}，PID 0x{slot.pid:04X}，{state}")

    if not online_slots:
        print("电量：无法读取（鼠标不在线）")
        print("充电：无法读取（鼠标不在线）")
        return

    cls, cmd, size = COMMAND_BATTERY
    battery_response = send_command(device, cls, cmd, size, timeout_ms=timeout_ms)
    battery_raw, battery_exact, battery_percent = parse_battery(battery_response)

    cls, cmd, size = COMMAND_CHARGING
    charging_response = send_command(device, cls, cmd, size, timeout_ms=timeout_ms)
    charging, charging_raw = parse_charging(charging_response)

    print(f"电量：{battery_percent}%")

    if charging is True:
        print("充电：是")
    elif charging is False:
        print("充电：否")
    else:
        print(f"充电：未知（原始值 0x{charging_raw:02X}）")

    # 保留精确值供调试，但不输出原始 HID 包。
    print(f"电量原始刻度：{battery_raw}/255（{battery_exact:.2f}%）")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="读取 Razer Mouse Dock Pro 所连接鼠标的电量和充电状态"
    )
    parser.add_argument("--watch", action="store_true", help="持续查询状态")
    parser.add_argument(
        "--interval",
        type=float,
        default=5.0,
        help="持续查询间隔，单位为秒，默认 5 秒",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=1500,
        help="单条命令超时时间，单位为毫秒，默认 1500",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="显示 HID 顶层集合、报告长度和探测过程",
    )
    args = parser.parse_args()

    if args.interval <= 0:
        print("--interval 必须大于 0")
        return 1
    if args.timeout <= 0:
        print("--timeout 必须大于 0")
        return 1

    device: Optional[WindowsFeatureDevice] = None

    try:
        candidates = enumerate_candidates(args.verbose)
        device, selected, first_pair_response = open_working_interface(
            candidates,
            timeout_ms=args.timeout,
            verbose=args.verbose,
        )

        caps = device.caps
        assert caps is not None
        print("已连接 Razer Mouse Dock Pro")
        if args.verbose:
            print(describe_interface(selected))
            print(
                f"FeatureReportByteLength={caps.feature_report_length}，"
                f"打开方式={access_text(device.open_access)}"
            )

        while True:
            try:
                query_status(
                    device=device,
                    timeout_ms=args.timeout,
                    first_pair_response=first_pair_response,
                )
                first_pair_response = None

            except (OSError, RazerProtocolError) as error:
                print(f"读取失败：{error}")
                if not args.watch:
                    return 1

            if not args.watch:
                break

            time.sleep(args.interval)

        return 0

    except KeyboardInterrupt:
        print("\n已停止。")
        return 0

    except Exception as error:
        print(f"错误：{error}")
        print("建议执行带诊断参数的命令：")
        print("python razer_mouse_dock_battery_v3.py --verbose")
        return 1

    finally:
        if device is not None:
            device.close()


if __name__ == "__main__":
    raise SystemExit(main())
