#pragma once

#include "src/core/BatteryDevice.h"

#include <QJsonObject>
#include <QList>
#include <QString>

// BatteryDevice <-> JSON 序列化辅助。
//
// 规约（与 docs/websocket-api.md §4.1 对齐）：
//   - 枚举字段统一输出小写字符串。
//   - percentage == -1 与 AirPods 左/右/盒 -1 输出为 JSON null（而非 0），
//     避免客户端把"未知"误当成"0%"。
//   - name / alias / displayName 三字段同时给出：displayName = alias 非空 ? alias : name，
//     与 MainWindow::deviceDisplayName 一致。
//
// 线程安全：纯函数式工具，无共享状态，任意线程调用均可。
namespace DeviceJson
{
// 单台设备 -> JSON。
QJsonObject toJson(const BatteryDevice &device);

// 设备列表 -> QJsonArray（以 QVariantList 形式返回，便于直接塞进 QJsonObject）。
QJsonObject listToJson(const QList<BatteryDevice> &devices, qint64 updatedAtMs);

// 设备签名：把影响"是否需要向客户端推送"的关键字段拼成一个稳定字符串，
// 用于在 RpcServer 端做变化检测去重（devicesUpdated 信号每轮都会发出，
// 但 RpcServer 不应每次都向客户端重发无变化的快照）。
QString signature(const BatteryDevice &device);

// —— 枚举字符串表（输出侧：枚举 -> 字符串）——
QString levelToString(BatteryLevel level);
QString typeToString(BatteryDevice::Type type);
QString subTypeToString(BatteryDevice::SubType subType);
} // namespace DeviceJson
