#pragma once

#include <QString>

// 低电量提醒策略。决定“设备持续低于阈值时，提醒以什么节奏重复”。
enum class AlertPolicy
{
    Once,       // 只在首次跌破阈值时提醒一次；电量回升后才会再次触发。（默认）
    Always,     // 每次轮询刷新都提醒一次（最频繁）。
    Every5Min,  // 跌破阈值期间，最多每 5 分钟提醒一次。
    Every15Min, // 同上，15 分钟。
    Every30Min, // 同上，30 分钟。
    Every60Min, // 同上，60 分钟。
};

// 每台设备的用户偏好（与全局 AppSettings 互补）。
//
// 这些设置按设备 id 持久化：用户在“设备信息”页设置的“是否显示到托盘”
// 以及“是否启用低电量提醒 + 阈值 + 策略”，会在该设备掉线再重连后继续生效——
// 只要它的 id 保持稳定（蓝牙走 WinRT/SetupAPI 的设备 id，Xbox 走 "XInput_<n>"
// 或 RawGameController id）。
//
// 设备 id 通常含有 '\\' 和 '/'（如 Windows 的 BTHENUM 路径），不能直接作为
// QSettings 的键（会被当作分组分隔符）。因此这里把 id 用 SHA-256 做一次哈希，
// 取 hex 前缀作为键名，既稳定又对长度友好。
//
// 键布局（QSettings 子组 "devices/<hash>"）：
//   - TrayVisible         : bool，默认 true（保留升级前“全部进托盘”的行为）。
//   - LowBatteryAlert     : bool，默认 true。是否对该设备做低电量提醒。
//   - LowBatteryThreshold : int 1..100，默认 20。触发提醒的电量百分比上限。
//   - LowBatteryPolicy    : QString，默认 "once"。见 AlertPolicy 枚举。
//
// 注：早期版本没有 LowBatteryAlert 键，那时阈值 = 0 表示“关闭”。
// 为了让老用户升级后行为一致，alertEnabled() 在键缺失时回退到
// “已存在的阈值是否 > 0” 来推断 —— 见实现中的迁移说明。
//
// 全部为静态方法，QSettings 自身线程安全，任意线程调用均可。
class DeviceSettings
{
public:
    // —— 设备 id -> 稳定哈希键 ——
    static QString keyForDeviceId(const QString &deviceId);

    // —— 是否显示到托盘 ——
    static bool trayVisible(const QString &deviceId);
    static void setTrayVisible(const QString &deviceId, bool visible);

    // —— 是否对该设备启用低电量提醒 ——
    static bool alertEnabled(const QString &deviceId);
    static void setAlertEnabled(const QString &deviceId, bool enabled);

    // —— 低电量提醒阈值百分比（1..100）。 ——
    static int lowBatteryThreshold(const QString &deviceId);
    static void setLowBatteryThreshold(const QString &deviceId, int percent);

    // —— 低电量提醒策略 ——
    static AlertPolicy alertPolicy(const QString &deviceId);
    static void setAlertPolicy(const QString &deviceId, AlertPolicy policy);
    // 策略 -> 持久化字符串 / 反向解析。未知字符串回退到默认 Once。
    static QString policyToString(AlertPolicy policy);
    static AlertPolicy stringToPolicy(const QString &value);

    // 默认值常量，供 UI 回填与判断时引用。
    static constexpr bool kDefaultTrayVisible = true;
    static constexpr bool kDefaultAlertEnabled = true;
    static constexpr int kDefaultLowBatteryThreshold = 20;
    static constexpr AlertPolicy kDefaultAlertPolicy = AlertPolicy::Once;
    static constexpr int kMaxThreshold = 100;

private:
    DeviceSettings() = delete;
};
