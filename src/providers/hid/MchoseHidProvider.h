#pragma once

#include "src/core/IBatteryProvider.h"

// MCHOSE 鼠标 HID 电量提供者。
//
// 支持 A7 V2 系列的有线连接、1K/8K 接收器和 8K MagDock。
// 同一 HID interface 可能暴露多个 collection，Provider 会依次探测并只保留
// 能完成设备信息 Feature 查询的 collection。
class MchoseHidProvider : public IBatteryProvider
{
public:
    std::wstring displayName() const override;
    std::vector<BatteryDevice> readDevices() override;
};
