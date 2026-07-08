#pragma once

#include "src/core/IBatteryProvider.h"

#include <cstdint>
#include <string>
#include <unordered_map>

// HID 电量提供者：通过 hidapi 读取 AULA 品牌（VID=0x0C45）2.4G 接收器
// 后挂接的键盘 / 鼠标的电量。
//
// 协议要点：
//   - 传输：HID Output / Input Report，Report ID = 0（非 Feature Report）。
//     单包字节数按设备报告描述符动态查询（AULA 存在 32 / 64 等多种报告大小），
//     查询失败时回退 32 字节默认值。
//   - 设备匹配：内置 162 个 AULA / AJAZZ PID 的型号表（VID=0x0C45）。每条记录该 PID 期望
//     匹配的顶层 usage：
//       * 标准簇 usage_page=0xFF68 / usage=0x0061（80 个 PID，绝大多数键盘 / 鼠标）；
//       * 特殊簇 usage_page=0xFF80 / usage=0x0001（0xFEFC）；
//       * 通配簇（81 个 PID，含 0xFEFE/F87 Pro Dongle）——不限定 usage，取该 PID 下
//         任一 vendor-defined 接口。
//   - 请求包（32 字节）：
//       [0]=0xAA, [1]=cmd, [2]=contentSize, [3]=addrLo, [4]=addrHi,
//       [5..7]=otherHeader / 尾包标记, [8..]=payload。无校验和。
//   - 响应帧头：[0x55, cmd, lenOrType, addrLo, addrHi, ...]，每包 8 字节头 + 24 字节
//     payload。多包请求（addr=0/24/48...）拼接 response[8..] 得完整 payload。
//   - 字段：payload[16] = workMode，payload[17] = battery(0-100)，
//     payload[18] = chargeStatus(≠0 即充电)；payload[4..7] = 设备自报 vid/pid
//     （小端，仅用于日志）。
//   - 有线模式不提供有效电池百分比，展示为有线供电而非 0%。
//
// —— 双协议支持（重要）——
// AULA / 关联品牌存在两代固件协议，命令号、contentSize、尾包标记都不同，
// readDevices() 对每个候选接口先尝试新协议，无效再尝试紧凑长度，最后回退旧协议：
//   - 新协议（F87ProV2D 等新设备）：
//       cmd = 0x10, contentSize = 56 或 48，请求头 byte[6] = 尾包标志（最后一包置 1）。
//   - 旧协议（早期型号）：
//       cmd = 0xA0, contentSize = 64, 请求头 byte[6] 恒 0。
// 协议选择依据「响应有效性」：用错误协议查询时设备常回帧但 payload 全 0
// （vid/pid/battery 都是 0），据此判无效并回退，避免误报电量 0%。
//
// 与 BatteryManager 现有 10s 轮询模型契合：每次 readDevices() 主动发一次
// GET_DEVICE_INFO 查询，同步读取响应。异步电量通知（旧协议 0xFB /
// 新协议 0xFA）在事件驱动模型下有用，但与桌面轮询模型重复，此处不实现，留作后续增强。
class AulaHidProvider : public IBatteryProvider
{
public:
    std::wstring displayName() const override;
    std::vector<BatteryDevice> readDevices() override;

private:
    // 上次查询成功的接口 path，按 PID 缓存（VID 固定 0x0C45）。
    // 命中则优先只查该接口；失败/失效回退该设备其余接口的全量遍历，成功后更新。
    std::unordered_map<std::uint16_t, std::string> m_lastGoodPath;
};
