#pragma once

#include "src/core/IBatteryProvider.h"

#include <cstdint>
#include <string>
#include <vector>

// Logitech 接收器设备 HID++ 2.0 电量提供者。
//
// 覆盖 Bolt / Unifying / Nano / Lightspeed 接收器。Windows 会把同一个 HID++ 接口拆成两个
// vendor-defined 顶层集合：usage FF00:0001 用于发送 Output Report，
// usage FF00:0002 用于读取 Input Report。协议流程：
//   1. Root.GetFeature 动态探测 0x1004 / 0x1000 / 0x1001；
//   2. 调用对应 Feature 读取精确电量或电压估算值及充电状态。
//
// Provider 首次扫描接收器槽位，成功后缓存槽位、Feature 索引和集合路径。
// 设备休眠时只查询缓存槽位一次，由 BatteryManager 的粘性缓存保留旧读数。
class LogitechHidProvider : public IBatteryProvider
{
public:
    std::wstring displayName() const override;
    std::vector<BatteryDevice> readDevices() override;

private:
    struct DeviceCache
    {
        int deviceIndex = 0;
        std::uint8_t featureIndex = 0;
        std::uint16_t featureId = 0;
        std::wstring name;
    };

    std::string m_outputPath;
    std::string m_inputPath;
    std::uint16_t m_receiverProductId = 0;
    std::vector<DeviceCache> m_deviceCaches;
};
