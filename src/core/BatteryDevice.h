#pragma once

#include <string>
#include <vector>

#include <QtGlobal> // qint64

// 统一的电量档位。所有设备都有 level，用于着色 / 托盘 / 排序 / 低电量提醒。
// 档位取值参考 XInput 的离散电量状态；蓝牙的精确百分比会派生到对应的档位。
enum class BatteryLevel
{
    Unknown = 0,
    Empty = 1,
    Low = 2,
    Medium = 3,
    Full = 4
};

// 单台设备的电量信息。跨 provider 统一模型，便于 UI 聚合展示。
//
// 注意：本结构刻意不依赖 Qt（用 std::wstring），使 battery 核心层可独立复用。
// Qt 边界（BatteryManager / MainWindow）在需要时把 std::wstring 转 QString。
struct BatteryDevice
{
    // 设备类别。
    enum class Type
    {
        Bluetooth,
        Xbox,
        // USB / 2.4G 接收器走 HID 协议的设备（AULA 键盘 / 鼠标等）。
        Hid
    };

    // 设备子类别。用于区分同 Type 下的不同展示形态：
    //   Generic —— 普通蓝牙耳机 / 手柄，只用 percentage + level。
    //   AirPods —— Apple AirPods / Beats，有独立的左/右/充电盒三路电量，
    //              写到 leftPercent / rightPercent / casePercent；percentage 取三路最低值。
    //   XiaomiBuds —— 小米 / Redmi Buds 系列真无线耳机，同为左/右/盒三路电量
    //              （解析小米私有 BLE 广播，见 XiaomiBudsProvider），展示方式与 AirPods 一致。
    enum class SubType
    {
        Generic,
        AirPods,
        XiaomiBuds
    };

    // 设备唯一标识。
    //   蓝牙: 使用 WinRT 返回的设备 Id（较长字符串）。
    //   Xbox: 使用 "XInput_<0..3>"，因为手柄没有稳定硬件序列号。
    std::wstring id;

    // 用户可读的设备名称。
    std::wstring name;

    // 设备类别。
    Type type = Type::Bluetooth;

    // 设备子类别（默认 Generic）。见 SubType 说明。
    SubType subType = SubType::Generic;

    // 精确电量百分比（0-100）。
    //   蓝牙: 真实读数；AirPods 取三路电量最低值，便于排序 / 低电量提醒。
    //   Xbox: -1 表示“无精确值”（XInput 离散档位），增强路径拿到精确值时填 0-100。
    int percentage = -1;

    // 离散电量档位，始终填充，用于显示 / 着色 / 排序 / 低电量提醒。
    BatteryLevel level = BatteryLevel::Unknown;

    // —— 三路电量专用（仅 isThreeChannelAudio(subType) 时有意义）——
    // 左耳 / 右耳 / 充电盒电量百分比；-1 表示该路未知（如单耳使用、盒盖打开）。
    int leftPercent = -1;
    int rightPercent = -1;
    int casePercent = -1;
    // 任一路正在充电即置 true（三路电量路径填写）。
    bool charging = false;
    // —— 三路充电粒度（仅 isThreeChannelAudio(subType) 时有意义）——
    // 左耳 / 右耳 / 充电盒各自是否正在充电；charging 恒等于三者之或，
    // 保留单一布尔是为排序 / 提醒 / RPC 兼容。
    bool leftCharging = false;
    bool rightCharging = false;
    bool caseCharging = false;
    // AirPods / 小米耳机广播是否能对应到本机已配对蓝牙地址。
    // 普通设备默认 true；只有广播类 provider 会填实际匹配结果。
    bool paired = true;

    // Windows / 设备协议是否报告为有线供电。该标记可以和有效电量同时存在：
    // 部分兼容 XInput 的 2.4 GHz dongle 会伪装成 USB 有线手柄，同时上报电量。
    bool wired = false;

    // 设备当前是否在线。
    bool connected = false;

    // —— 粘性缓存字段（仅 BatteryManager 维护，provider 不写）——
    //
    // 上次「成功读到」本设备的时间戳（QClock::currentMSecsSinceEpoch()）。
    // BatteryManager 每轮刷新时更新；用于判断缓存是否还在保留窗口内。
    qint64 lastSeenMsecs = 0;
    // 当前展示值是否来自缓存（本轮 provider 没返回该设备，沿用上次读数）。
    // UI 据此把整行标灰并显示「过期」标识。
    bool stale = false;
};

// 设备列表（核心层用 std::vector；Qt 边界自行转换为 QList/QVector）。
using BatteryDeviceList = std::vector<BatteryDevice>;

// 两台设备的“用户可见内容”是否一致（按同一下标的两台设备比较）。
// 供消费方判断列表是否需要重建：排除 lastSeenMsecs（每轮刷新都会变，
// 但用户看不到），其余展示相关字段全部参与比较。
inline bool sameDisplayContent(const BatteryDevice &a, const BatteryDevice &b)
{
    return a.id == b.id && a.name == b.name && a.type == b.type &&
           a.subType == b.subType && a.percentage == b.percentage &&
           a.level == b.level && a.leftPercent == b.leftPercent &&
           a.rightPercent == b.rightPercent && a.casePercent == b.casePercent &&
           a.charging == b.charging && a.leftCharging == b.leftCharging &&
           a.rightCharging == b.rightCharging && a.caseCharging == b.caseCharging &&
           a.paired == b.paired && a.wired == b.wired &&
           a.connected == b.connected && a.stale == b.stale;
}

// 是否“三路电量”音频设备（AirPods / 小米耳机）。
// UI 列表 / 详情页 / 历史曲线 / 过滤逻辑据此决定是否按左/右/盒三列展示。
inline bool isThreeChannelAudio(BatteryDevice::SubType subType)
{
    return subType == BatteryDevice::SubType::AirPods ||
           subType == BatteryDevice::SubType::XiaomiBuds;
}
