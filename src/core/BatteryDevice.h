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
    enum class SubType
    {
        Generic,
        AirPods
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

    // —— AirPods 三路电量专用（仅 subType==AirPods 时有意义）——
    // 左耳 / 右耳 / 充电盒电量百分比；-1 表示该路未知（如单耳使用、盒盖打开）。
    int leftPercent = -1;
    int rightPercent = -1;
    int casePercent = -1;
    // 任一路正在充电即置 true（仅 AirPods 路径填写）。
    bool charging = false;

    // 是否接有线电源（Xbox 接有线时无电池）。
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
