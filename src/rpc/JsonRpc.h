#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

// JSON-RPC 2.0 协议辅助。
//
// 只负责构造消息与解析请求结构，不涉及 WebSocket 传输。错误码遵循规范：
//   -32700 Parse error / -32600 Invalid Request / -32601 Method not found
//   -32602 Invalid params / -32603 Internal error
// 应用层错误定义在 -32xxx 段（见 docs §13）。
namespace JsonRpc
{
// 应用层错误码。
enum class ErrorCode
{
    ParseError      = -32700,
    InvalidRequest  = -32600,
    MethodNotFound  = -32601,
    InvalidParams   = -32602,
    InternalError   = -32603,
    Unauthorized    = -32001,
    Forbidden       = -32002,
    NotSubscribed   = -32003,
    DeviceNotFound  = -32004,
    ReadOnlyField   = -32005,
    ServerBusy      = -32090,
    UnknownError    = -32099,
};

// —— 响应构造 ——
QJsonObject okResult(const QJsonObject &result, const QJsonValue &id);
QJsonObject okResult(const QJsonArray &result, const QJsonValue &id);
QJsonObject errorResponse(ErrorCode code, const QString &message,
                          const QJsonValue &id,
                          const QJsonObject &data = QJsonObject());
// 用 id = null 构造"解析/无效请求"错误（id 无法解析时的兜底）。
QJsonObject errorNoId(ErrorCode code, const QString &message,
                      const QJsonObject &data = QJsonObject());

// 通知（Server -> Client 推送，无 id）。
QJsonObject notification(const QString &method, const QJsonObject &params);

// —— 请求解析 ——
struct Request
{
    bool valid = false;          // 是否为合法 JSON-RPC 2.0 请求
    bool isNotification = false; // id 缺省或为 null -> 视为通知，不回包
    QString method;
    QJsonObject params;
    QJsonValue id;
    QString errorMessage;        // valid=false 时的原因（供 -32600）
};

Request parseRequest(const QJsonObject &obj);

// 校验 helper：取命名参数并做类型检查；失败时返回错误字符串。
//   - param<T>(obj, key, out) -> true=成功；false=缺失或类型不符。
bool getString(const QJsonObject &o, const QString &key, QString *out);
bool getBool(const QJsonObject &o, const QString &key, bool *out);
bool getInt(const QJsonObject &o, const QString &key, int *out);
} // namespace JsonRpc
