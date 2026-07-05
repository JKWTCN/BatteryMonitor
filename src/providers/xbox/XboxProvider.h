#pragma once

#include "src/core/IBatteryProvider.h"

// Xbox 手柄电量提供者：多路径读取手柄电量。
//
// 路径：
//   1. XInput（基线）：4 个手柄槽位的离散电量状态（满 / 中 / 低 / 空），
//      按原样映射到 BatteryLevel，无精确百分比。ID 形如 "XInput_<n>"。
//   2. RawGameController（增强，精确百分比）：遍历 Windows.Gaming.Input 的
//      RawGameControllers，仅微软手柄（VID=0x045E=1118），
//      TryGetBatteryReport() 读 RemainingCapacity/FullChargeCapacity 算百分比 +
//      Charging 标志。ID 形如 "xbox:raw:<NonRoamableId>"，与 XInput 路径不冲突。
//   3. 设备属性键（增强）：从 DeviceInformation 的
//      "{104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2"（百分比）属性直接读
//      经典蓝牙手柄(BTH)/XUSB 等的电量，绕开 GATT。
//
// 路径 2/3 拿到的精确百分比优于路径 1 的离散档位，优先采用；XInput 作为兜底。
class XboxProvider : public IBatteryProvider
{
public:
    std::wstring displayName() const override;
    std::vector<BatteryDevice> readDevices() override;
};
