#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Read battery and charging information from compatible MCHOSE HID mice.

Dependency:
    python -m pip install hidapi

Examples:
    python scripts/mchose_mouse_battery.py
    python scripts/mchose_mouse_battery.py --pid 0x4018 --verbose
    python scripts/mchose_mouse_battery.py --count 10 --interval 1
"""

from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass
from typing import Any

try:
    import hid
except ImportError:
    print("缺少 hidapi，请执行：python -m pip install hidapi", file=sys.stderr)
    raise SystemExit(2)


DEFAULT_VIDS = (0x3837, 0x5253)
REPORT_ID = 0x11
REPORT_LENGTH = 21
FEATURE_READ_LENGTH = 65
LOGICAL_COMMAND = 0x06

# Feature 报告的第一个字节为 Report ID，后续命令字节按位取反。
DEVICE_INFO_QUERY = bytes([REPORT_ID, LOGICAL_COMMAND ^ 0xFF]) + bytes(
    [0xFF] * (REPORT_LENGTH - 2)
)

KNOWN_PRODUCTS = {
    (0x3837, 0x4018): "MCHOSE A7 V2 Pro（有线）",
    (0x3837, 0x100A): "MCHOSE A7 V2 系列（1K Dongle）",
    (0x3837, 0x100B): "MCHOSE A7 V2 系列（8K Dongle）",
    (0x5253, 0x1020): "MCHOSE A7 V2 系列（8K MagDock）",
}


@dataclass(frozen=True)
class Candidate:
    path: Any
    vid: int
    pid: int
    interface_number: int
    usage_page: int
    usage: int
    product: str
    serial: str

    @property
    def name(self) -> str:
        return KNOWN_PRODUCTS.get((self.vid, self.pid), self.product or "MCHOSE HID")


def auto_int(value: str) -> int:
    return int(value, 0)


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


def is_control_interface(info: dict[str, Any]) -> bool:
    interface_number = int_value(info.get("interface_number"))
    usage_page = int_value(info.get("usage_page"), 0)
    path = path_text(info.get("path", "")).lower()
    identity = (
        int_value(info.get("vendor_id"), 0),
        int_value(info.get("product_id"), 0),
    )

    return (
        usage_page == 0xFF01
        or (
            identity in KNOWN_PRODUCTS
            and (interface_number == 2 or "mi_02" in path)
        )
    )


def enumerate_candidates(vid: int, pid: int | None) -> list[Candidate]:
    """Return likely vendor-control HID collections, best match first."""
    ranked: list[tuple[int, Candidate]] = []

    for info in hid.enumerate(vid, pid or 0):
        device_vid = int_value(info.get("vendor_id"), 0)
        device_pid = int_value(info.get("product_id"), 0)
        if device_vid != vid or (pid is not None and device_pid != pid):
            continue
        if not is_control_interface(info):
            continue

        interface_number = int_value(info.get("interface_number"))
        usage_page = int_value(info.get("usage_page"), 0)
        usage = int_value(info.get("usage"), 0)
        candidate = Candidate(
            path=info.get("path"),
            vid=device_vid,
            pid=device_pid,
            interface_number=interface_number,
            usage_page=usage_page,
            usage=usage,
            product=text_value(info.get("product_string")),
            serial=text_value(info.get("serial_number")),
        )

        score = 0
        if interface_number == 2:
            score += 100
        if usage_page == 0xFF01:
            score += 50
        if "mi_02" in path_text(candidate.path).lower():
            score += 25
        ranked.append((score, candidate))

    result: list[Candidate] = []
    seen: set[str] = set()
    for _, candidate in sorted(ranked, key=lambda item: item[0], reverse=True):
        key = repr(candidate.path)
        if key not in seen:
            seen.add(key)
            result.append(candidate)
    return result


def decode_feature_report(data: Any) -> dict[str, Any]:
    raw = bytes(data)
    if raw and raw[0] == REPORT_ID:
        encoded = raw[1:]
    else:
        encoded = raw

    if len(encoded) < 12:
        raise ValueError(f"Feature 响应过短：{len(raw)} 字节")

    payload = bytes(value ^ 0xFF for value in encoded)
    if payload[0] != LOGICAL_COMMAND:
        raise ValueError(
            f"响应命令不匹配：0x{payload[0]:02X}，预期 0x{LOGICAL_COMMAND:02X}"
        )

    protocol_vid = int.from_bytes(payload[1:3], "little")
    protocol_pid = int.from_bytes(payload[3:5], "little")
    firmware = int.from_bytes(payload[5:9], "little")
    flags = payload[9]
    battery_raw = payload[10]
    charge_status = payload[11]

    if battery_raw > 100:
        raise ValueError(f"电量字段越界：{battery_raw}")

    battery_bucket = 100 if battery_raw == 100 else (battery_raw // 10) * 10
    return {
        "raw": raw,
        "decoded": payload,
        "vid": protocol_vid,
        "pid": protocol_pid,
        "firmware": firmware,
        "connect_mode": (flags >> 5) & 0x07,
        "connect_status": (flags >> 4) & 0x01,
        "flags": flags,
        "battery_raw": battery_raw,
        "battery_bucket": battery_bucket,
        "charge_status": charge_status,
        "charging": charge_status == 1,
    }


def query_device(device: Any, delay_ms: int) -> dict[str, Any]:
    written = device.send_feature_report(DEVICE_INFO_QUERY)
    if written < REPORT_LENGTH:
        raise OSError(f"Feature 写入长度异常：{written}，预期至少 {REPORT_LENGTH}")

    time.sleep(delay_ms / 1000.0)
    response = device.get_feature_report(REPORT_ID, FEATURE_READ_LENGTH)
    return decode_feature_report(response)


def print_candidate(index: int, candidate: Candidate, verbose: bool) -> None:
    print(
        f"[{index}] {candidate.name}  "
        f"VID:PID={candidate.vid:04X}:{candidate.pid:04X}  "
        f"interface={candidate.interface_number}"
    )
    if verbose:
        print(f"    usage  = 0x{candidate.usage_page:04X}:0x{candidate.usage:04X}")
        print(f"    serial = {candidate.serial or '(空)'}")
        print(f"    path   = {path_text(candidate.path)}")


def poll_candidate(
    candidate: Candidate,
    count: int,
    interval: float,
    delay_ms: int,
    verbose: bool,
) -> bool:
    device = hid.device()
    try:
        device.open_path(candidate.path)
        for sample in range(1, count + 1):
            result = query_device(device, delay_ms)
            mode = "有线" if result["connect_mode"] == 0 else str(result["connect_mode"])
            charge = "充电中" if result["charging"] else f"否（状态 {result['charge_status']}）"
            print(
                f"    样本 {sample}: 电量原始值 {result['battery_raw']}%，"
                f"10% 显示档位 {result['battery_bucket']}%，充电 {charge}，连接模式 {mode}"
            )
            print(
                f"             设备字段 {result['vid']:04X}:{result['pid']:04X}，"
                f"固件 0x{result['firmware']:08X}"
            )
            if verbose:
                print(f"             raw     = {result['raw'].hex(' ')}")
                print(f"             decoded = {result['decoded'].hex(' ')}")
                print(
                    f"             flags   = 0x{result['flags']:02X}, "
                    f"connect_status={result['connect_status']}"
                )
            if sample < count:
                time.sleep(interval)
        return True
    except (OSError, ValueError) as error:
        print(f"    查询失败：{error}", file=sys.stderr)
        return False
    finally:
        device.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="读取 MCHOSE 鼠标电量与充电状态")
    parser.add_argument("--vid", type=auto_int, help="只查询指定 VID；默认查询 0x3837 和 0x5253")
    parser.add_argument("--pid", type=auto_int, help="只查询指定 PID，例如 0x4018")
    parser.add_argument("--count", type=int, default=1, help="每个接口的查询次数")
    parser.add_argument("--interval", type=float, default=1.0, help="连续查询间隔（秒）")
    parser.add_argument("--delay-ms", type=int, default=10, help="写入后读取延迟（毫秒）")
    parser.add_argument("--verbose", action="store_true", help="显示 HID 路径与原始数据")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.count < 1 or args.interval < 0 or args.delay_ms < 0:
        print("count 必须大于 0，interval 和 delay-ms 不能为负数", file=sys.stderr)
        return 2

    vids = (args.vid,) if args.vid is not None else DEFAULT_VIDS
    candidates = [
        candidate
        for vid in vids
        for candidate in enumerate_candidates(vid, args.pid)
    ]
    if not candidates:
        pid_text = "任意 PID" if args.pid is None else f"PID=0x{args.pid:04X}"
        vid_text = "/".join(f"0x{vid:04X}" for vid in vids)
        print(f"未找到 VID={vid_text}、{pid_text} 的控制接口", file=sys.stderr)
        return 1

    print(f"找到 {len(candidates)} 个候选 HID 接口：")
    success = False
    for index, candidate in enumerate(candidates, 1):
        print_candidate(index, candidate, args.verbose)
        if poll_candidate(
            candidate,
            args.count,
            args.interval,
            args.delay_ms,
            args.verbose,
        ):
            success = True

    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
