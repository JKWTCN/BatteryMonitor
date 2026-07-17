#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""通过 hidapi 读取 Logitech MX Master 4 的电量和充电状态。

设备是 Logitech Bolt 接收器（VID 046D、PID C548）。协议为
HID++ 2.0 Unified Battery（Feature ID 0x1004）。脚本会动态查询 Feature
索引和在线的接收器槽位，不依赖固定槽位 3/Feature 索引 9。

依赖：python -m pip install hidapi
"""

from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass

try:
    import hid
except ImportError:
    print("未安装 hidapi，请先执行：python -m pip install hidapi")
    raise SystemExit(1)


DEFAULT_VID = 0x046D
DEFAULT_PID = 0xC548

HIDPP_USAGE_PAGE = 0xFF00
HIDPP_OUTPUT_USAGE = 0x0001
HIDPP_INPUT_USAGE = 0x0002

REPORT_ID_SHORT = 0x10
REPORT_ID_LONG = 0x11
REPORT_ID_ERROR = 0x8F
SHORT_REPORT_LENGTH = 7
LONG_REPORT_LENGTH = 20

ROOT_FEATURE_INDEX = 0x00
UNIFIED_BATTERY_FEATURE_ID = 0x1004
SOFTWARE_ID = 0x07
GET_FEATURE_FUNCTION = 0x00
GET_STATUS_FUNCTION = 0x01

HIDPP_ERROR_NAMES = {
    0x01: "未知错误",
    0x02: "无效参数",
    0x03: "超出范围",
    0x04: "硬件错误",
    0x05: "Logitech 内部错误",
    0x06: "无效 Feature 索引",
    0x07: "无效函数",
    0x08: "忙碌",
    0x09: "不支持",
}

BATTERY_STATUS_NAMES = {
    0x00: "放电中",
    0x01: "充电中",
    0x02: "接近充满",
    0x03: "已充满",
    0x04: "慢速充电",
    0x05: "电池无效",
    0x06: "温度异常",
    0x07: "充电错误",
}

EXTERNAL_POWER_NAMES = {
    0x00: "未接入",
    0x01: "已接入",
    0x02: "外部供电异常",
}


class HidppError(RuntimeError):
    pass


@dataclass(frozen=True)
class BatteryResult:
    device_index: int
    percent: int
    next_level: int
    status: int
    external_power: int
    raw_response: bytes

    @property
    def charging(self) -> bool | None:
        if self.status in (0x01, 0x02, 0x04):
            return True
        if self.status in (0x00, 0x03):
            return False
        return None


def hex_bytes(data: bytes) -> str:
    return " ".join(f"{value:02X}" for value in data)


def describe_collection(info: dict) -> str:
    return (
        f"interface={info.get('interface_number', -1)}, "
        f"usage_page=0x{info.get('usage_page', 0):04X}, "
        f"usage=0x{info.get('usage', 0):04X}, "
        f"path={info.get('path')!r}"
    )


def select_collections(vid: int, pid: int, verbose: bool) -> tuple[dict, dict]:
    devices = hid.enumerate(vid, pid)
    if not devices:
        raise HidppError(f"未找到 Logitech 接收器：VID={vid:04X}, PID={pid:04X}")

    if verbose:
        print("检测到以下 HID 顶层集合：")
        for info in devices:
            print(f"  {describe_collection(info)}")
        print()

    output_candidates = [
        info
        for info in devices
        if info.get("usage_page") == HIDPP_USAGE_PAGE
        and info.get("usage") == HIDPP_OUTPUT_USAGE
    ]
    input_candidates = [
        info
        for info in devices
        if info.get("usage_page") == HIDPP_USAGE_PAGE
        and info.get("usage") == HIDPP_INPUT_USAGE
    ]

    # Bolt C548 的 HID++ 接口为 interface 2；排序后仍能兼容接口号不同的接收器。
    output_candidates.sort(key=lambda info: info.get("interface_number") != 2)
    input_candidates.sort(key=lambda info: info.get("interface_number") != 2)

    for output_info in output_candidates:
        for input_info in input_candidates:
            if output_info.get("interface_number") == input_info.get("interface_number"):
                return output_info, input_info

    raise HidppError(
        "找到了接收器，但没有找到 HID++ 输入/输出集合 "
        "(usage_page=FF00, usage=1/2)"
    )


class HidppTransport:
    def __init__(self, output_path: bytes, input_path: bytes, verbose: bool = False):
        self.output_path = output_path
        self.input_path = input_path
        self.verbose = verbose
        self.writer = hid.device()
        self.reader = hid.device()

    def open(self) -> None:
        # Windows 将同一 HID++ 接口暴露成两个顶层集合：usage 1 用于写，usage 2 用于读。
        self.writer.open_path(self.output_path)
        try:
            self.reader.open_path(self.input_path)
        except Exception:
            self.writer.close()
            raise

    def close(self) -> None:
        self.reader.close()
        self.writer.close()

    def __enter__(self) -> "HidppTransport":
        self.open()
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()

    def drain(self) -> None:
        for _ in range(256):
            if not self.reader.read(LONG_REPORT_LENGTH, 1):
                return

    def write(self, report: bytes) -> None:
        if len(report) != SHORT_REPORT_LENGTH or report[0] != REPORT_ID_SHORT:
            raise ValueError("HID++ 短报告必须是以 0x10 开头的 7 字节")

        if self.verbose:
            print(f"TX: {hex_bytes(report)}")

        written = self.writer.write(report)
        if written != len(report):
            raise HidppError(
                f"HID 写入失败：期望 {len(report)} 字节，实际返回 {written}"
            )

    def read(self, timeout_ms: int) -> bytes:
        report = bytes(self.reader.read(LONG_REPORT_LENGTH, timeout_ms))
        if report and self.verbose:
            print(f"RX: {hex_bytes(report)}")
        return report


def function_byte(function: int) -> int:
    return ((function & 0x0F) << 4) | SOFTWARE_ID


def build_request(
    device_index: int,
    feature_index: int,
    function: int,
    parameters: bytes = b"\x00\x00\x00",
) -> bytes:
    if len(parameters) != 3:
        raise ValueError("HID++ 短报告参数必须为 3 字节")
    return bytes(
        [
            REPORT_ID_SHORT,
            device_index,
            feature_index,
            function_byte(function),
            *parameters,
        ]
    )


def response_matches(report: bytes, request: bytes) -> bool:
    return (
        len(report) >= 4
        and report[0] in (REPORT_ID_SHORT, REPORT_ID_LONG)
        and report[1:4] == request[1:4]
    )


def error_matches(report: bytes, request: bytes) -> bool:
    return (
        len(report) >= 5
        and report[0] == REPORT_ID_ERROR
        and report[1] == request[1]
        and report[2] == request[2]
        and report[3] == request[3]
    )


def send_command(
    transport: HidppTransport,
    request: bytes,
    timeout_ms: int,
) -> bytes:
    transport.drain()
    transport.write(request)
    deadline = time.monotonic() + timeout_ms / 1000.0

    while time.monotonic() < deadline:
        remaining_ms = max(1, int((deadline - time.monotonic()) * 1000))
        report = transport.read(min(remaining_ms, 100))
        if not report:
            continue
        if response_matches(report, request):
            return report
        if error_matches(report, request):
            code = report[4]
            name = HIDPP_ERROR_NAMES.get(code, "未知 HID++ 错误")
            raise HidppError(f"设备返回错误 0x{code:02X}（{name}）")

    raise TimeoutError("等待 HID++ 响应超时")


def find_unified_battery_feature(
    transport: HidppTransport,
    device_index: int,
    timeout_ms: int,
) -> int:
    parameters = UNIFIED_BATTERY_FEATURE_ID.to_bytes(2, "big") + b"\x00"
    request = build_request(
        device_index,
        ROOT_FEATURE_INDEX,
        GET_FEATURE_FUNCTION,
        parameters,
    )
    response = send_command(transport, request, timeout_ms)
    if len(response) < 7 or response[4] == 0:
        raise HidppError("设备不支持 Unified Battery Feature 0x1004")
    return response[4]


def query_battery(
    transport: HidppTransport,
    device_index: int,
    timeout_ms: int,
) -> BatteryResult:
    feature_index = find_unified_battery_feature(
        transport, device_index, timeout_ms
    )
    request = build_request(
        device_index,
        feature_index,
        GET_STATUS_FUNCTION,
    )
    response = send_command(transport, request, timeout_ms)
    if len(response) < 8:
        raise HidppError(f"电量响应长度不足：{hex_bytes(response)}")

    percent = response[4]
    if percent > 100:
        raise HidppError(f"设备返回无效电量 {percent}：{hex_bytes(response)}")

    return BatteryResult(
        device_index=device_index,
        percent=percent,
        next_level=response[5],
        status=response[6],
        external_power=response[7],
        raw_response=response,
    )


def scan_batteries(
    transport: HidppTransport,
    device_indices: list[int],
    timeout_ms: int,
) -> tuple[list[BatteryResult], list[str]]:
    results: list[BatteryResult] = []
    errors: list[str] = []

    for device_index in device_indices:
        try:
            results.append(query_battery(transport, device_index, timeout_ms))
        except (TimeoutError, HidppError, OSError) as error:
            errors.append(f"槽位 {device_index}: {error}")

    return results, errors


def print_result(result: BatteryResult, show_raw: bool) -> None:
    status_name = BATTERY_STATUS_NAMES.get(
        result.status, f"未知状态 0x{result.status:02X}"
    )
    power_name = EXTERNAL_POWER_NAMES.get(
        result.external_power, f"未知 0x{result.external_power:02X}"
    )

    print(f"接收器槽位：{result.device_index}")
    print(f"电量：{result.percent}%")
    if result.charging is True:
        print("充电：是")
    elif result.charging is False:
        print("充电：否")
    else:
        print("充电：未知")
    print(f"电池状态：{status_name}")
    print(f"外部供电：{power_name}")

    if show_raw:
        print(f"原始响应：{hex_bytes(result.raw_response)}")
        print(f"下一电量阈值：{result.next_level}%")


def parse_hex(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"无效整数：{value}") from error


def main() -> int:
    parser = argparse.ArgumentParser(
        description="通过 Logitech Bolt 接收器读取 MX Master 4 电量和充电状态"
    )
    parser.add_argument("--vid", type=parse_hex, default=DEFAULT_VID)
    parser.add_argument("--pid", type=parse_hex, default=DEFAULT_PID)
    parser.add_argument(
        "--device-index",
        type=int,
        help="指定接收器槽位 1-6；默认自动扫描",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=1200,
        help="每条 HID++ 命令的超时毫秒数，默认 1200",
    )
    parser.add_argument("--watch", action="store_true", help="持续读取")
    parser.add_argument(
        "--interval", type=float, default=5.0, help="持续读取间隔秒数，默认 5"
    )
    parser.add_argument("--raw", action="store_true", help="显示原始响应")
    parser.add_argument("--verbose", action="store_true", help="显示 HID 通信诊断")
    args = parser.parse_args()

    if args.device_index is not None and not 1 <= args.device_index <= 6:
        parser.error("--device-index 必须在 1-6 之间")
    if args.timeout <= 0:
        parser.error("--timeout 必须大于 0")
    if args.interval <= 0:
        parser.error("--interval 必须大于 0")

    try:
        output_info, input_info = select_collections(args.vid, args.pid, args.verbose)
        if args.verbose:
            print(f"输出集合：{describe_collection(output_info)}")
            print(f"输入集合：{describe_collection(input_info)}")
            print()

        indices = [args.device_index] if args.device_index else list(range(1, 7))

        with HidppTransport(
            output_info["path"], input_info["path"], args.verbose
        ) as transport:
            while True:
                results, errors = scan_batteries(transport, indices, args.timeout)
                if not results:
                    detail = "\n".join(errors) if args.verbose else ""
                    raise HidppError(
                        "没有找到支持 Unified Battery 的在线设备。"
                        "请移动一下鼠标唤醒后重试。"
                        + (f"\n{detail}" if detail else "")
                    )

                print(time.strftime("%Y-%m-%d %H:%M:%S"))
                for position, result in enumerate(results):
                    if position:
                        print("-" * 48)
                    print_result(result, args.raw or args.verbose)

                if not args.watch:
                    break
                print("=" * 48)
                time.sleep(args.interval)

        return 0
    except KeyboardInterrupt:
        print("\n已停止。")
        return 0
    except Exception as error:
        print(f"错误：{error}")
        print("可执行 --verbose 查看接口与原始通信信息。")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
