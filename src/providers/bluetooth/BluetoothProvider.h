#pragma once

#include "src/core/IBatteryProvider.h"

// 蓝牙电量提供者：通过 WinRT 多路径读取蓝牙设备电量。
//
// 读取策略：
//   1. Power::Battery：可读 RemainingCapacity / FullChargeCapacity 的设备（主力）。
//   2. BLE GATT Battery Service：
//      - 设备发现用全量选择器 BluetoothLEDevice.GetDeviceSelector()，
//        再按 System.Devices.Aep.IsConnected 属性过滤连接状态
//        （比 GetDeviceSelectorFromConnectionStatus(Connected) 覆盖面更广）。
//      - GATT 读取“Uncached 优先，Cached 兜底”
//        （很多耳机的 GATT 缓存为空 / 过期，必须 Uncached 才能拿到真值）。
//      - 规范化设备 ID 优先用 System.Devices.ContainerId（container:<guid>），
//        便于跨 provider 去重。
//
// WinRT apartment 在每次 readDevices() 进入时惰性初始化为 multi_threaded，
// 因此本对象可安全地在 BatteryManager 的 worker 线程中被调用。
class BluetoothProvider : public IBatteryProvider
{
public:
    std::wstring displayName() const override;
    std::vector<BatteryDevice> readDevices() override;
};
