#pragma once

#include "src/core/IBatteryProvider.h"

#include <cstdint>
#include <string>
#include <unordered_map>

// Razer 鼠标 / 键盘 HID 电量提供者。
//
// 设备按 VID/PID 匹配，并通过 HID Feature Report 查询。Provider 会尝试同一设备的
// 所有候选接口，记住上次成功响应的接口；临时休眠 / 离线抖动交给 BatteryManager
// 的粘性缓存处理。
class RazerHidProvider : public IBatteryProvider
{
public:
    std::wstring displayName() const override;
    std::vector<BatteryDevice> readDevices() override;

private:
    std::unordered_map<std::uint64_t, std::string> m_lastGoodPath;
    std::unordered_map<std::uint64_t, int> m_lastNonZeroPercentage;
    std::unordered_map<std::uint64_t, int> m_zeroReadStreak;
};
