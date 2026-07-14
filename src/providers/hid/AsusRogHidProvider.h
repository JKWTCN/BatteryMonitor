#pragma once

#include "src/core/IBatteryProvider.h"

// ASUS ROG Strix Scope RX TKL Wireless Deluxe 接收器电量提供者。
//
// 仅匹配 0B05:1A07 的 vendor-defined 电量接口（interface 1，
// usage_page=FF00，usage=0001），通过 HID Output/Input Report 查询：
//   请求：Report ID 0 + 64 字节数据，数据以 12 01 开头；
//   响应：12 01 ...，response[5] 为电量，response[8] 为充电状态。
class AsusRogHidProvider : public IBatteryProvider
{
public:
    std::wstring displayName() const override;
    std::vector<BatteryDevice> readDevices() override;
};
