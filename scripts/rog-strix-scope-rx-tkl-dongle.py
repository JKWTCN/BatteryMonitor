#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import sys
import time
import argparse

try:
    import hid
except ImportError:
    print("未安装 hidapi，请先执行：")
    print("python -m pip install hidapi")
    sys.exit(1)


VID = 0x0B05
PID = 0x1A07

TARGET_INTERFACE = 1
TARGET_USAGE_PAGE = 0xFF00
TARGET_USAGE = 0x0001

REPORT_SIZE = 64
COMMAND_BATTERY = 0x12
SUBCOMMAND_BATTERY = 0x01


def format_bytes(data: list[int] | bytes | bytearray) -> str:
    """将字节数组格式化为十六进制字符串。"""
    return " ".join(f"{value:02X}" for value in data)


def find_battery_interface() -> dict:
    """查找用于 ROG 电量查询的 HID 接口。"""
    devices = hid.enumerate(VID, PID)

    if not devices:
        raise RuntimeError(
            f"未找到设备：VID=0x{VID:04X}, PID=0x{PID:04X}"
        )

    print("检测到以下 HID 接口：")

    target = None

    for device in devices:
        interface_number = device.get("interface_number", -1)
        usage_page = device.get("usage_page", 0)
        usage = device.get("usage", 0)
        path = device.get("path")

        print(
            f"interface={interface_number}, "
            f"usage_page=0x{usage_page:04X}, "
            f"usage=0x{usage:04X}, "
            f"path={path!r}"
        )

        if (
            interface_number == TARGET_INTERFACE
            and usage_page == TARGET_USAGE_PAGE
            and usage == TARGET_USAGE
        ):
            target = device

    if target is None:
        raise RuntimeError(
            "找到了 ROG 设备，但没有找到电量通信接口：\n"
            f"interface={TARGET_INTERFACE}, "
            f"usage_page=0x{TARGET_USAGE_PAGE:04X}, "
            f"usage=0x{TARGET_USAGE:04X}"
        )

    return target


def normalize_response(response: list[int]) -> list[int]:
    """
    统一不同 hidapi 实现的返回格式。

    正常协议响应以 12 01 开头。
    某些实现可能在开头保留 00 Report ID。
    """
    if not response:
        return []

    if len(response) >= 3 and response[0] == 0x00:
        if response[1] == COMMAND_BATTERY:
            return response[1:]

    return response


def read_battery_response(
    device: hid.device,
    timeout_ms: int = 1000,
) -> list[int]:
    """
    读取电量查询响应。

    在超时时间内忽略与 12 01 不匹配的其他 HID 报告。
    """
    deadline = time.monotonic() + timeout_ms / 1000.0
    last_response: list[int] = []

    while time.monotonic() < deadline:
        remaining_ms = max(
            1,
            int((deadline - time.monotonic()) * 1000),
        )

        response = device.read(REPORT_SIZE, remaining_ms)

        if not response:
            continue

        response = normalize_response(response)
        last_response = response

        if (
            len(response) >= 2
            and response[0] == COMMAND_BATTERY
            and response[1] == SUBCOMMAND_BATTERY
        ):
            return response

    if last_response:
        raise RuntimeError(
            "收到 HID 响应，但不是预期的电量响应：\n"
            f"{format_bytes(last_response)}"
        )

    raise RuntimeError("发送查询指令后没有收到设备响应")


def query_battery(
    device_path: bytes,
    timeout_ms: int = 1000,
) -> tuple[int, bool | None, list[int]]:
    """查询一次电量和充电状态。"""
    device = hid.device()

    try:
        device.open_path(device_path)

        # HIDAPI 的 write() 通常要求第一个字节为 Report ID。
        # 该接口没有实际 Report ID，因此在最前面放 0x00。
        packet = bytearray(REPORT_SIZE + 1)
        packet[0] = 0x00
        packet[1] = COMMAND_BATTERY
        packet[2] = SUBCOMMAND_BATTERY

        written = device.write(packet)

        if written <= 0:
            raise RuntimeError("发送电量查询指令失败")

        response = read_battery_response(device, timeout_ms)

        if len(response) < 11:
            raise RuntimeError(
                f"响应长度不足：需要至少 11 字节，实际为 {len(response)} 字节"
            )

        # 根据当前抓包和充电/未充电测试确认：
        # response[5]：电量百分比
        # response[8]：充电状态，0=未充电，1=充电
        battery = response[5]
        charging_value = response[8]

        if not 0 <= battery <= 100:
            raise RuntimeError(
                f"设备返回了无效电量：{battery}，"
                f"原始值为 0x{battery:02X}"
            )

        if charging_value == 0x00:
            charging = False
        elif charging_value == 0x01:
            charging = True
        else:
            charging = None

        return battery, charging, response

    finally:
        device.close()


def print_battery_status(
    battery: int,
    charging: bool | None,
    response: list[int],
    show_raw: bool,
) -> None:
    """输出电池状态。"""
    if show_raw:
        print(f"Response: {format_bytes(response)}")

    print(f"Battery: {battery}%")

    if charging is True:
        print("Charging: Yes")
        print("状态：正在充电")
    elif charging is False:
        print("Charging: No")
        print("状态：未充电")
    else:
        print("Charging: Unknown")
        print(
            f"状态：未知，原始充电字段为 0x{response[8]:02X}"
        )

    # 当前测试中 response[10] 与 response[5] 相同，
    # 暂时作为辅助校验，不作为主电量字段。
    duplicate_battery = response[10]

    if duplicate_battery != battery:
        print(
            "提示：电量字段副本不一致："
            f"response[5]={battery}, "
            f"response[10]={duplicate_battery}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="读取 ROG Strix Scope RX TKL Wireless Deluxe 电量"
    )

    parser.add_argument(
        "--watch",
        action="store_true",
        help="持续查询电量",
    )

    parser.add_argument(
        "--interval",
        type=float,
        default=5.0,
        help="持续查询时间隔，单位为秒，默认 5 秒",
    )

    parser.add_argument(
        "--timeout",
        type=int,
        default=1000,
        help="单次读取超时时间，单位为毫秒，默认 1000 毫秒",
    )

    parser.add_argument(
        "--no-raw",
        action="store_true",
        help="不显示原始 HID 响应",
    )

    args = parser.parse_args()

    if args.interval <= 0:
        print("--interval 必须大于 0")
        return 1

    try:
        target = find_battery_interface()
        device_path = target["path"]

        print("\n已选择电量接口：")
        print(f"path={device_path!r}")
        print()

        while True:
            try:
                battery, charging, response = query_battery(
                    device_path,
                    timeout_ms=args.timeout,
                )

                print_battery_status(
                    battery,
                    charging,
                    response,
                    show_raw=not args.no_raw,
                )

            except OSError as error:
                print(f"HID 通信失败：{error}")

                if not args.watch:
                    return 1

                # 设备可能发生重连，重新枚举路径。
                print("尝试重新查找设备接口……")
                target = find_battery_interface()
                device_path = target["path"]

            except RuntimeError as error:
                print(f"读取失败：{error}")

                if not args.watch:
                    return 1

            if not args.watch:
                break

            print("-" * 72)
            time.sleep(args.interval)

        return 0

    except KeyboardInterrupt:
        print("\n已停止。")
        return 0

    except Exception as error:
        print(f"错误：{error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())