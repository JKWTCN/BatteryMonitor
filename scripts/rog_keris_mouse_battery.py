#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ROG Keris Wireless AimPoint 电量/充电状态协议验证脚本。

抓包中确认：
- USB 有线模式：VID=0x0B05, PID=0x1A66
- 2.4G Dongle：VID=0x0B05, PID=0x1A68
- 厂商 HID 接口：通常 interface_number=0, usage_page=0xFF01, usage=0x0001
- 输出/输入报告均为 64 字节，无 HID Report ID
- hidapi 写入无 Report ID 的报告时，仍需在最前面补一个 0x00

查询：
    12 07 00 00 + 60 个 00

已观察到的响应：
- Dongle 99%：12 07 00 00 63 02 0A 34 10 00 ...
- USB 99% 充电：12 07 00 00 63 02 0A 85 10 01 ...
                   12 07 00 00 63 02 0A 80 10 01 ...

字段推断：
- response[4]：电量百分比，0x63 = 99
- response[7:9]：疑似小端电芯电压，0x1034=4148 mV、0x1085=4229 mV
- response[9]：充电标志，0=未充电，1=正在充电
"""

from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass
from typing import Any, Iterable

try:
    import hid
except ImportError:
    print("缺少 hidapi：请执行  python -m pip install hidapi", file=sys.stderr)
    raise SystemExit(2)

VID_ASUS = 0x0B05
PID_USB = 0x1A66
PID_DONGLE = 0x1A68
KNOWN_PIDS = {
    PID_USB: "USB 有线模式",
    PID_DONGLE: "2.4G Dongle 模式",
}

REPORT_SIZE = 64
BATTERY_QUERY = bytes([0x12, 0x07, 0x00, 0x00]) + bytes(REPORT_SIZE - 4)
BATTERY_RESPONSE_PREFIX = bytes([0x12, 0x07, 0x00, 0x00])


@dataclass(frozen=True)
class Candidate:
    path: Any
    pid: int
    interface_number: int
    usage_page: int
    usage: int
    product: str
    serial: str

    @property
    def mode_name(self) -> str:
        return KNOWN_PIDS.get(self.pid, f"未知模式 PID=0x{self.pid:04X}")


def int_value(value: Any, default: int = -1) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def text_value(value: Any) -> str:
    return "" if value is None else str(value)


def path_text(path: Any) -> str:
    if isinstance(path, bytes):
        return path.decode("utf-8", errors="backslashreplace")
    return str(path)


def enumerate_candidates(pid_filter: int | None = None) -> list[Candidate]:
    """枚举 ROG 鼠标的厂商 HID 接口，并按匹配程度排序。"""
    devices = hid.enumerate(VID_ASUS, pid_filter or 0)
    candidates: list[tuple[int, Candidate]] = []

    for info in devices:
        pid = int_value(info.get("product_id"), 0)
        if pid_filter is not None and pid != pid_filter:
            continue
        if pid_filter is None and pid not in KNOWN_PIDS:
            continue

        interface_number = int_value(info.get("interface_number"))
        usage_page = int_value(info.get("usage_page"), 0)
        usage = int_value(info.get("usage"), 0)

        # 抓包对应 interface 0，报告描述符为 Usage Page 0xFF01 / Usage 0x01。
        # 某些 hidapi 后端不提供 usage_page，因此 interface_number=0 也保留。
        is_vendor_interface = (
            interface_number == 0
            or (usage_page == 0xFF01 and usage == 0x0001)
        )
        if not is_vendor_interface:
            continue

        candidate = Candidate(
            path=info.get("path"),
            pid=pid,
            interface_number=interface_number,
            usage_page=usage_page,
            usage=usage,
            product=text_value(info.get("product_string")),
            serial=text_value(info.get("serial_number")),
        )

        score = 0
        if interface_number == 0:
            score += 100
        if usage_page == 0xFF01:
            score += 50
        if usage == 0x0001:
            score += 20
        candidates.append((score, candidate))

    # 去重，某些后端可能重复返回同一路径。
    result: list[Candidate] = []
    seen: set[str] = set()
    for _, candidate in sorted(candidates, key=lambda item: item[0], reverse=True):
        key = repr(candidate.path)
        if key not in seen:
            seen.add(key)
            result.append(candidate)
    return result


def normalize_input_report(data: Iterable[int]) -> bytes:
    """兼容后端是否把无编号报告前面的 0x00 返回给调用者。"""
    raw = bytes(data)
    if len(raw) >= REPORT_SIZE + 1 and raw[0] == 0x00:
        raw = raw[1:]
    return raw


def flush_input(device: Any) -> None:
    """清理之前遗留的输入响应，避免把旧包当成本次查询结果。"""
    for _ in range(32):
        data = device.read(REPORT_SIZE + 1, 1)
        if not data:
            return


def send_query(device: Any) -> int:
    # hidapi 规定：即使设备没有 Report ID，也要在 write 缓冲区首字节写 0。
    packet = b"\x00" + BATTERY_QUERY
    written = device.write(packet)
    if written < REPORT_SIZE:
        raise OSError(f"HID 写入长度异常：{written}，预期至少 {REPORT_SIZE}")
    return written


def read_battery_response(device: Any, timeout_ms: int, verbose: bool) -> bytes:
    deadline = time.monotonic() + timeout_ms / 1000.0

    while True:
        remaining_ms = int((deadline - time.monotonic()) * 1000)
        if remaining_ms <= 0:
            raise TimeoutError(f"等待电量响应超时（{timeout_ms} ms）")

        data = device.read(REPORT_SIZE + 1, remaining_ms)
        if not data:
            raise TimeoutError(f"等待电量响应超时（{timeout_ms} ms）")

        raw = normalize_input_report(data)
        if raw.startswith(BATTERY_RESPONSE_PREFIX):
            return raw

        if verbose:
            print(f"  忽略其他输入报告：{raw.hex(' ')}")


def parse_battery_response(response: bytes) -> dict[str, Any]:
    if len(response) < 10:
        raise ValueError(f"响应过短：{len(response)} 字节")
    if not response.startswith(BATTERY_RESPONSE_PREFIX):
        raise ValueError("响应前缀不是 12 07 00 00")

    battery_percent = response[4]
    suspected_voltage_mv = int.from_bytes(response[7:9], "little")
    charge_flag = response[9]

    return {
        "battery_percent": battery_percent,
        "charging": charge_flag != 0,
        "charge_flag": charge_flag,
        "suspected_voltage_mv": suspected_voltage_mv,
        "byte5": response[5],
        "byte6": response[6],
    }


def print_candidate(index: int, candidate: Candidate) -> None:
    print(f"[{index}] {candidate.mode_name}")
    print(f"    VID:PID       = {VID_ASUS:04X}:{candidate.pid:04X}")
    print(f"    interface     = {candidate.interface_number}")
    print(f"    usage         = 0x{candidate.usage_page:04X}:0x{candidate.usage:04X}")
    print(f"    product       = {candidate.product or '(空)'}")
    print(f"    serial        = {candidate.serial or '(空)'}")
    print(f"    path          = {path_text(candidate.path)}")


def poll_candidate(
    candidate: Candidate,
    count: int,
    interval: float,
    timeout_ms: int,
    verbose: bool,
) -> bool:
    device = hid.device()
    try:
        device.open_path(candidate.path)
        print(f"\n已打开：{candidate.mode_name}，PID=0x{candidate.pid:04X}")

        for sample_index in range(1, count + 1):
            flush_input(device)
            send_query(device)
            response = read_battery_response(device, timeout_ms, verbose)
            parsed = parse_battery_response(response)

            voltage = parsed["suspected_voltage_mv"]
            voltage_text = (
                f"{voltage} mV"
                if 2500 <= voltage <= 5000
                else f"{voltage}（数值不像常见单节锂电池电压）"
            )
            state = "正在充电" if parsed["charging"] else "未充电"

            print(f"\n样本 {sample_index}/{count}")
            print(f"  电量              : {parsed['battery_percent']}%")
            print(f"  充电状态          : {state}")
            print(f"  充电原始标志      : 0x{parsed['charge_flag']:02X}")
            print(f"  疑似电芯电压      : {voltage_text}")
            print(f"  未知字段 byte5/6  : 0x{parsed['byte5']:02X} 0x{parsed['byte6']:02X}")
            print(f"  原始响应          : {response.hex(' ')}")

            if sample_index < count:
                time.sleep(interval)
        return True

    except Exception as exc:
        print(f"\n查询失败：{candidate.mode_name} PID=0x{candidate.pid:04X}: {exc}", file=sys.stderr)
        return False
    finally:
        try:
            device.close()
        except Exception:
            pass


def parse_hex_pid(value: str) -> int:
    value = value.strip().lower()
    if value.startswith("0x"):
        value = value[2:]
    try:
        pid = int(value, 16)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("PID 必须是十六进制，例如 1A66") from exc
    if not 0 <= pid <= 0xFFFF:
        raise argparse.ArgumentTypeError("PID 超出 16 位范围")
    return pid


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="验证 ROG Keris Wireless AimPoint HID 电量与充电状态协议"
    )
    parser.add_argument(
        "--pid",
        type=parse_hex_pid,
        help="只查询指定 PID，例如 USB=1A66，Dongle=1A68；默认查询两者",
    )
    parser.add_argument("--count", type=int, default=5, help="每个接口查询次数，默认 5")
    parser.add_argument("--interval", type=float, default=1.0, help="查询间隔秒数，默认 1.0")
    parser.add_argument("--timeout", type=int, default=1200, help="单次读取超时毫秒，默认 1200")
    parser.add_argument("--list", action="store_true", help="只列出匹配接口，不发送命令")
    parser.add_argument("--verbose", action="store_true", help="显示被忽略的其他输入报告")
    return parser


def main() -> int:
    args = build_argument_parser().parse_args()
    if args.count <= 0:
        print("--count 必须大于 0", file=sys.stderr)
        return 2
    if args.interval < 0:
        print("--interval 不能小于 0", file=sys.stderr)
        return 2
    if args.timeout <= 0:
        print("--timeout 必须大于 0", file=sys.stderr)
        return 2

    candidates = enumerate_candidates(args.pid)
    if not candidates:
        print("没有找到匹配的 ROG 厂商 HID 接口。", file=sys.stderr)
        print("预期设备：0B05:1A66（USB）或 0B05:1A68（Dongle）。", file=sys.stderr)
        return 1

    print(f"找到 {len(candidates)} 个候选接口：")
    for index, candidate in enumerate(candidates):
        print_candidate(index, candidate)

    if args.list:
        return 0

    successes = 0
    for candidate in candidates:
        if poll_candidate(
            candidate=candidate,
            count=args.count,
            interval=args.interval,
            timeout_ms=args.timeout,
            verbose=args.verbose,
        ):
            successes += 1

    return 0 if successes else 1


if __name__ == "__main__":
    raise SystemExit(main())
