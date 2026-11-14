#pragma once

#include "src/core/IBatteryProvider.h"

#include <string>
#include <vector>

// Sony DualSense / DualSense Edge（PS5 手柄）电量提供者。
//
// 协议参考 Linux 内核 drivers/hid/hid-playstation.c 中 DS5 输入报告的定义。
// DS5 不使用厂商私有查询命令，电量就在它持续上报的标准 Input Report 里，
// 因此这里只做「被动读取」：
//
//   USB       Report ID 0x01，64 字节（含 Report ID），电量状态字节在 [53]
//   蓝牙      Report ID 0x31，78 字节（含 Report ID），电量状态字节在 [54]
//             末尾 4 字节是 CRC32（种子字节 0xA1 + 报告体），校验失败一律丢弃
//
// 状态字节低 4 位是 0-10 的电量档位，高 4 位是充电状态
// （0=放电 / 1=充电中 / 2=已充满）。硬件只按 10% 分档上报，因此设备模型
// 同时保存区间下界与上界，界面显示真实区间；已充满显示确定的 100%。
//
// 只匹配 VID 0x054C / PID 0x0CE6、0x0DF2，优先打开 Generic Desktop
// Game Pad（usage_page=0x0001 / usage=0x0005）collection；驱动未填 usage
// 时才兜底尝试，键盘 / 鼠标 / 音频 / 厂商控制接口一律跳过。
// 蓝牙简化报告（不含电量）与休眠设备不会返回设备行，由 BatteryManager
// 的粘性缓存沿用上一次有效读数。
//
// 注意：不向手柄发送任何模式切换命令，不改变设备当前工作模式。
class DualSenseHidProvider : public IBatteryProvider
{
public:
    std::wstring displayName() const override;
    std::vector<BatteryDevice> readDevices() override;

private:
    // 仅用于失败日志去重：默认 10s 一轮轮询，避免每轮都写同一条告警。
    // Provider 由 BatteryManager 的单个 worker 线程独占调用，无需加锁。
    bool m_lastReadSucceeded = true;
};
