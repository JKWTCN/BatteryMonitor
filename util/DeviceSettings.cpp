#include "DeviceSettings.h"

#include <QSettings>
#include <QCryptographicHash>

namespace
{
// QSettings 子组名。所有设备偏好集中放在下面，便于将来清理 / 列举。
constexpr auto kGroupDevices = "devices";

constexpr auto kKeyAlias = "Alias";
constexpr auto kKeyTrayVisible = "TrayVisible";
constexpr auto kKeyAlertEnabled = "LowBatteryAlert";
constexpr auto kKeyLowBatteryThreshold = "LowBatteryThreshold";
constexpr auto kKeyLowBatteryPolicy = "LowBatteryPolicy";

// 持久化用的策略字符串。改动这些常量会破坏老设置，需谨慎。
constexpr auto kPolicyStrOnce = "once";
constexpr auto kPolicyStrAlways = "always";
constexpr auto kPolicyStr5Min = "5min";
constexpr auto kPolicyStr15Min = "15min";
constexpr auto kPolicyStr30Min = "30min";
constexpr auto kPolicyStr60Min = "60min";

// 把设备 id 转成稳定的 hex 字符串键。
// 用 SHA-256 的前 16 hex 位（64 bit 截断）——对几台到几十台设备的集合，
// 碰撞概率可忽略；同时避免在 QSettings 路径里出现原 id 里的 '\\' '/'。
QString hashKey(const QString &deviceId)
{
    const QByteArray hash = QCryptographicHash::hash(
        deviceId.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(hash.left(8).toHex());
}

// 返回某台设备所在的 QSettings 子组前缀，例如 "devices/1a2b3c4d5e6f7081/"。
QString prefixFor(const QString &deviceId)
{
    return QString::fromLatin1(kGroupDevices) + QLatin1Char('/') +
           hashKey(deviceId) + QLatin1Char('/');
}
} // namespace

QString DeviceSettings::keyForDeviceId(const QString &deviceId)
{
    return hashKey(deviceId);
}

QString DeviceSettings::alias(const QString &deviceId)
{
    if (deviceId.isEmpty()) {
        return {};
    }
    return QSettings().value(prefixFor(deviceId) + kKeyAlias).toString().trimmed();
}

void DeviceSettings::setAlias(const QString &deviceId, const QString &alias)
{
    if (deviceId.isEmpty()) {
        return;
    }

    QSettings s;
    const QString key = prefixFor(deviceId) + kKeyAlias;
    const QString value = alias.trimmed();
    if (value.isEmpty()) {
        s.remove(key);
        return;
    }
    s.setValue(key, value);
}

bool DeviceSettings::trayVisible(const QString &deviceId)
{
    if (deviceId.isEmpty()) {
        return kDefaultTrayVisible;
    }
    QSettings s;
    return s.value(prefixFor(deviceId) + kKeyTrayVisible, kDefaultTrayVisible)
        .toBool();
}

void DeviceSettings::setTrayVisible(const QString &deviceId, bool visible)
{
    if (deviceId.isEmpty()) {
        return;
    }
    QSettings().setValue(prefixFor(deviceId) + kKeyTrayVisible, visible);
}

bool DeviceSettings::alertEnabled(const QString &deviceId)
{
    if (deviceId.isEmpty()) {
        return kDefaultAlertEnabled;
    }
    QSettings s;
    const QString key = prefixFor(deviceId) + kKeyAlertEnabled;
    if (s.contains(key)) {
        // 新版直接读取。
        return s.value(key, kDefaultAlertEnabled).toBool();
    }
    // 迁移：旧版本没有 AlertEnabled 键，那时用 threshold==0 表示“关闭”。
    // 旧阈值缺失（全新设备）-> 默认启用；旧阈值存在且为 0 -> 视为关闭。
    if (s.contains(prefixFor(deviceId) + kKeyLowBatteryThreshold)) {
        const int oldThreshold =
            s.value(prefixFor(deviceId) + kKeyLowBatteryThreshold,
                    kDefaultLowBatteryThreshold).toInt();
        return oldThreshold > 0;
    }
    return kDefaultAlertEnabled;
}

void DeviceSettings::setAlertEnabled(const QString &deviceId, bool enabled)
{
    if (deviceId.isEmpty()) {
        return;
    }
    QSettings().setValue(prefixFor(deviceId) + kKeyAlertEnabled, enabled);
}

int DeviceSettings::lowBatteryThreshold(const QString &deviceId)
{
    if (deviceId.isEmpty()) {
        return kDefaultLowBatteryThreshold;
    }
    const int value = QSettings()
                          .value(prefixFor(deviceId) + kKeyLowBatteryThreshold,
                                 kDefaultLowBatteryThreshold)
                          .toInt();
    // 阈值合法范围 1..100。越界 / 损坏值回退到默认。
    if (value < 1 || value > kMaxThreshold) {
        return kDefaultLowBatteryThreshold;
    }
    return value;
}

void DeviceSettings::setLowBatteryThreshold(const QString &deviceId, int percent)
{
    if (deviceId.isEmpty()) {
        return;
    }
    // 与读取端一致：阈值范围 1..100（启用 / 关闭由独立的 AlertEnabled 控制）。
    const int clamped = qBound(1, percent, kMaxThreshold);
    QSettings().setValue(prefixFor(deviceId) + kKeyLowBatteryThreshold, clamped);
}

AlertPolicy DeviceSettings::alertPolicy(const QString &deviceId)
{
    if (deviceId.isEmpty()) {
        return kDefaultAlertPolicy;
    }
    const QString value = QSettings()
                          .value(prefixFor(deviceId) + kKeyLowBatteryPolicy,
                                 policyToString(kDefaultAlertPolicy))
                          .toString();
    return stringToPolicy(value);
}

void DeviceSettings::setAlertPolicy(const QString &deviceId, AlertPolicy policy)
{
    if (deviceId.isEmpty()) {
        return;
    }
    QSettings().setValue(prefixFor(deviceId) + kKeyLowBatteryPolicy,
                         policyToString(policy));
}

QString DeviceSettings::policyToString(AlertPolicy policy)
{
    switch (policy) {
    case AlertPolicy::Always:     return kPolicyStrAlways;
    case AlertPolicy::Every5Min:  return kPolicyStr5Min;
    case AlertPolicy::Every15Min: return kPolicyStr15Min;
    case AlertPolicy::Every30Min: return kPolicyStr30Min;
    case AlertPolicy::Every60Min: return kPolicyStr60Min;
    case AlertPolicy::Once:
    default:                      return kPolicyStrOnce;
    }
}

AlertPolicy DeviceSettings::stringToPolicy(const QString &value)
{
    if (value == kPolicyStrAlways)     return AlertPolicy::Always;
    if (value == kPolicyStr5Min)       return AlertPolicy::Every5Min;
    if (value == kPolicyStr15Min)      return AlertPolicy::Every15Min;
    if (value == kPolicyStr30Min)      return AlertPolicy::Every30Min;
    if (value == kPolicyStr60Min)      return AlertPolicy::Every60Min;
    if (value == kPolicyStrOnce)       return AlertPolicy::Once;
    return kDefaultAlertPolicy; // 未知字符串（含空串）-> 默认。
}
