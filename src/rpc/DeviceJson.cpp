#include "DeviceJson.h"

#include "util/DeviceSettings.h"

#include <QJsonArray>

namespace
{
// -1 是"未知"哨兵；序列化时映射为 JSON null，避免与 0% 混淆。
QJsonValue percentValue(int value)
{
    return value < 0 ? QJsonValue::Null : QJsonValue(value);
}
} // namespace

namespace DeviceJson
{
QJsonObject toJson(const BatteryDevice &device)
{
    const QString idQ = QString::fromStdWString(device.id);
    const QString nameQ = QString::fromStdWString(device.name);
    // 设备级 alias 来自 DeviceSettings；为空时 displayName 回退到 name。
    const QString aliasQ = DeviceSettings::alias(idQ);
    const QString displayQ = aliasQ.isEmpty() ? nameQ : aliasQ;

    QJsonObject o;
    o["id"] = idQ;
    o["name"] = nameQ;
    o["alias"] = aliasQ;
    o["displayName"] = displayQ;
    o["type"] = typeToString(device.type);
    o["subType"] = subTypeToString(device.subType);
    o["percentage"] = percentValue(device.percentage);
    o["level"] = levelToString(device.level);
    o["leftPercent"] = percentValue(device.leftPercent);
    o["rightPercent"] = percentValue(device.rightPercent);
    o["casePercent"] = percentValue(device.casePercent);
    o["charging"] = device.charging;
    o["paired"] = device.paired;
    o["wired"] = device.wired;
    o["connected"] = device.connected;
    o["stale"] = device.stale;
    o["lastSeen"] = device.lastSeenMsecs;
    return o;
}

QJsonObject listToJson(const QList<BatteryDevice> &devices, qint64 updatedAtMs)
{
    QJsonArray arr;
    for (const auto &d : devices) {
        arr.append(toJson(d));
    }
    QJsonObject o;
    o["devices"] = arr;
    o["updatedAt"] = updatedAtMs;
    o["count"] = devices.size();
    return o;
}

QString signature(const BatteryDevice &device)
{
    // 仅纳入"客户端可见且会影响展示/判断"的字段。
    // 刻意排除 lastSeenMsecs（每轮都变，会破坏去重）。
    return QString("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10|%11|%12|%13|%14")
        .arg(QString::fromStdWString(device.id))
        .arg(QString::fromStdWString(device.name))
        .arg(static_cast<int>(device.type))
        .arg(static_cast<int>(device.subType))
        .arg(device.percentage)
        .arg(static_cast<int>(device.level))
        .arg(device.leftPercent)
        .arg(device.rightPercent)
        .arg(device.casePercent)
        .arg(device.charging ? 1 : 0)
        .arg(device.paired ? 1 : 0)
        .arg(device.wired ? 1 : 0)
        .arg(device.connected ? 1 : 0)
        .arg(device.stale ? 1 : 0);
}

QString levelToString(BatteryLevel level)
{
    switch (level) {
    case BatteryLevel::Empty:  return QStringLiteral("empty");
    case BatteryLevel::Low:    return QStringLiteral("low");
    case BatteryLevel::Medium: return QStringLiteral("medium");
    case BatteryLevel::Full:   return QStringLiteral("full");
    case BatteryLevel::Unknown:
    default:                   return QStringLiteral("unknown");
    }
}

QString typeToString(BatteryDevice::Type type)
{
    switch (type) {
    case BatteryDevice::Type::Xbox: return QStringLiteral("xbox");
    case BatteryDevice::Type::Hid:  return QStringLiteral("hid");
    case BatteryDevice::Type::Bluetooth:
    default:                        return QStringLiteral("bluetooth");
    }
}

QString subTypeToString(BatteryDevice::SubType subType)
{
    switch (subType) {
    case BatteryDevice::SubType::AirPods: return QStringLiteral("airpods");
    case BatteryDevice::SubType::Generic:
    default:                              return QStringLiteral("generic");
    }
}
} // namespace DeviceJson
