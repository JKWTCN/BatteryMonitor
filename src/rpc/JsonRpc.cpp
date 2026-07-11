#include "JsonRpc.h"

namespace JsonRpc
{
QJsonObject okResult(const QJsonObject &result, const QJsonValue &id)
{
    return {
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("result"), result},
        {QStringLiteral("id"), id},
    };
}

QJsonObject okResult(const QJsonArray &result, const QJsonValue &id)
{
    return {
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("result"), result},
        {QStringLiteral("id"), id},
    };
}

QJsonObject errorResponse(ErrorCode code, const QString &message,
                          const QJsonValue &id, const QJsonObject &data)
{
    QJsonObject err;
    err["code"] = static_cast<int>(code);
    err["message"] = message;
    if (!data.isEmpty()) {
        err["data"] = data;
    }
    return {
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("error"), err},
        {QStringLiteral("id"), id},
    };
}

QJsonObject errorNoId(ErrorCode code, const QString &message, const QJsonObject &data)
{
    return errorResponse(code, message, QJsonValue::Null, data);
}

QJsonObject notification(const QString &method, const QJsonObject &params)
{
    return {
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    };
}

Request parseRequest(const QJsonObject &obj)
{
    Request r;

    // jsonrpc 字段必须为 "2.0"。
    const QJsonValue v = obj.value(QStringLiteral("jsonrpc"));
    if (v.toString() != QStringLiteral("2.0")) {
        r.errorMessage = QStringLiteral("Missing or invalid 'jsonrpc' field");
        return r;
    }

    // method 必须为非空字符串。
    r.method = obj.value(QStringLiteral("method")).toString();
    if (r.method.isEmpty()) {
        r.errorMessage = QStringLiteral("Missing or invalid 'method' field");
        return r;
    }

    // params 若给出则必须是对象（本服务不支持数组 positional params）。
    const QJsonValue params = obj.value(QStringLiteral("params"));
    if (!params.isUndefined() && !params.isNull()) {
        if (!params.isObject()) {
            r.errorMessage = QStringLiteral("'params' must be an object");
            return r;
        }
        r.params = params.toObject();
    }

    // id：缺省 / null => 通知；否则记录原值用于回包。
    const QJsonValue id = obj.value(QStringLiteral("id"));
    if (id.isUndefined() || id.isNull()) {
        r.isNotification = true;
    } else if (!id.isDouble() && !id.isString()) {
        // id 只允许 number / string / null。
        r.errorMessage = QStringLiteral("'id' must be number, string, or null");
        return r;
    }
    r.id = id;

    r.valid = true;
    return r;
}

bool getString(const QJsonObject &o, const QString &key, QString *out)
{
    const QJsonValue v = o.value(key);
    if (!v.isString()) return false;
    *out = v.toString();
    return true;
}

bool getBool(const QJsonObject &o, const QString &key, bool *out)
{
    const QJsonValue v = o.value(key);
    if (!v.isBool()) return false;
    *out = v.toBool();
    return true;
}

bool getInt(const QJsonObject &o, const QString &key, int *out)
{
    const QJsonValue v = o.value(key);
    if (!v.isDouble()) return false;
    const double d = v.toDouble();
    if (d != static_cast<int>(d)) return false; // 拒绝小数
    *out = static_cast<int>(d);
    return true;
}
} // namespace JsonRpc
