#pragma once

#include "src/core/IBatteryProvider.h"

// ASUS 鼠标 HID 电量提供者。
//
// 设备表按 PID 记录目标接口、Report ID、报文长度和电量字段布局。
// 查询 payload 以 12 07 开头。无编号报告仍会在 hid_write 缓冲区
// 首字节放置 0，带编号报告则放置实际 Report ID。
class AsusMouseHidProvider : public IBatteryProvider
{
public:
    std::wstring displayName() const override;
    std::vector<BatteryDevice> readDevices() override;
};
