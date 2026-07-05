#pragma once

#include "BatteryDevice.h"

#include <vector>

// 电量提供者抽象接口（纯 C++，不依赖 Qt）。
//
// 每种设备来源（蓝牙 / Xbox / 未来的 USB HID）实现一个 Provider，
// 由 BatteryManager 聚合并统一轮询。新增设备类型只需：
//   1. 新增一个实现 IBatteryProvider 的类；
//   2. 在 BatteryManager 构造里 addProvider(std::make_unique<XxxProvider>());
// UI、轮询、托盘逻辑无需改动。
class IBatteryProvider
{
public:
    virtual ~IBatteryProvider() = default;

    // 提供者名称（如 L"Bluetooth" / L"Xbox"），用于日志与诊断。
    virtual std::wstring displayName() const = 0;

    // 同步读取当前所有设备的电量。在 BatteryManager 的 worker 线程调用，
    // 实现可执行阻塞 / 慢速 I/O（如 WinRT GATT 读操作），不会卡住 UI 线程。
    virtual std::vector<BatteryDevice> readDevices() = 0;
};
