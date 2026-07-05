#pragma once

#include "IBatteryProvider.h"

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/base.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>

// AirPods / Beats 电量提供者：解析 Apple Continuity BLE 广播包得到左/右/盒电量。
//
// 背景：AirPods、Beats 等苹果音频设备不暴露标准 BLE GATT Battery Service，
// 只有 Apple 私有“Continuity”协议。其电量信息周期性地以 BLE 厂商数据广播
// （CompanyId = 0x004C），无需配对或 GATT 连接即可被旁听。
//
// 实现：
//   - 用 BluetoothLEAdvertisementWatcher（Active 扫描），过滤 CompanyId=76(0x004C)。
//   - 收到广播后：RSSI 低于阈值丢弃；payload<25 字节丢弃；payload[0]!=0x07
//     （非 AirPods continuity 类型）丢弃。
//   - 解码：modelId = (payload[4]<<8)|payload[3]，查型号表得到设备名；
//     左/右/盒电量从 payload[6]/payload[7] 的 4-bit nibble 取，按 payload[10]&2
//     处理左右翻转；nibble→百分比（nib>=10→100，nib==15→-1 未知，其余 nib*10，
//     AirPods Max 型号 +5）；充电状态从 payload[7] 高 nibble 判定。
//
// 适配现有轮询架构：watcher 在 readDevices() 首次调用时启动并保持运行；
// readDevices() 返回“最近 N 秒内收到过广播”的设备快照。这样事件驱动逻辑
// 融入同步轮询，BatteryManager 无需改动。
class AirPodsProvider : public IBatteryProvider
{
public:
    AirPodsProvider();
    ~AirPodsProvider() override;

    std::wstring displayName() const override;
    std::vector<BatteryDevice> readDevices() override;

private:
    // 单台 Apple 设备的最新解析结果（缓存）。
    struct AdvDevice
    {
        std::wstring name;
        int leftPercent = -1;
        int rightPercent = -1;
        int casePercent = -1;
        bool charging = false;
        short rssi = 0;
        std::chrono::steady_clock::time_point lastSeen; // 最近一次收到广播的时间
    };

    // 启动 watcher（幂等）。
    void ensureWatcherStarted();

    // 广播接收回调（在 WinRT 线程触发）。
    void onAdvertisementReceived(
        const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher &sender,
        const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs &args);

    // 解析 Apple 厂商数据 payload，成功返回 true 并填充各电量字段。
    static bool parseApplePayload(const uint8_t *data, std::size_t len,
                                  std::wstring &modelName, int &left, int &right,
                                  int &casePct, bool &charging);

    // 蓝牙地址 -> 解析结果。由 watcher 回调线程写、readDevices() 读，需加锁。
    std::map<uint64_t, AdvDevice> m_devices;
    std::mutex m_mutex;

    winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher m_watcher{nullptr};
    bool m_watcherStarted = false;
    winrt::event_token m_receivedToken;
};
