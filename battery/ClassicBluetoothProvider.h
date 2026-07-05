#pragma once

#include "IBatteryProvider.h"

#include <set>
#include <string>

// 经典蓝牙（Classic Bluetooth）电量提供者。
//
// 普通蓝牙耳机（XISEM-Olite2 / Redmi Buds 5 Pro 等）走经典蓝牙（A2DP/HFP）而非
// BLE，因此：
//   - BluetoothLEDevice 看不到它们的连接状态（GATT 路径读不到）。
//   - DeviceInformationKind::Device 的 System.Devices.Connected 也不可靠。
//
// 实现思路：
//
// 1. 连接判断（哪些耳机当前在线）：
//    用 BluetoothDevice.GetDeviceSelector()（经典蓝牙全量选择器）枚举，
//    请求 System.Devices.Aep.IsConnected / System.Devices.ContainerId 属性。
//    若 Aep.IsConnected 为 bool 则采纳；否则回退到
//    BluetoothDevice.FromIdAsync(id).ConnectionStatus == Connected。
//    收集到“已连接的设备名集合” + “设备名 -> 规范化ID”映射。
//
// 2. 电量读取（SetupAPI）：
//    SetupDiGetClassDevsW(NULL, L"BTHENUM", NULL, DIGCF_PRESENT) 枚举蓝牙
//    枚举器下的所有设备节点，对每个节点：
//      - 读设备友好名（SPDRP_FRIENDLYNAME / DeviceDesc）；
//      - 若名字含 Bluetooth/Headset/Hands-Free，且与第 1 步某个已连接设备名匹配：
//        用 SetupDiGetDevicePropertyW 读 DEVPROPKEY
//        {104EA319-6EE2-4701-BD47-8DDBF425BBE5}, pid=2（4 字节 int 百分比）。
//    同一物理耳机可能有多个 BTHENUM 节点（A2DP/HFP），按名字合并取一份。
//
// 这样只显示“当前连接的经典蓝牙音频设备”的电量。
class ClassicBluetoothProvider : public IBatteryProvider
{
public:
    std::wstring displayName() const override;
    std::vector<BatteryDevice> readDevices() override;
};
