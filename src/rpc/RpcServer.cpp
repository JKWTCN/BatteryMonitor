#include "RpcServer.h"

#include "DeviceJson.h"
#include "JsonRpc.h"
#include "src/core/BatteryManager.h"
#include "util/AppSettings.h"
#include "util/DeviceSettings.h"
#include "util/Logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

namespace
{
// 鉴权窗口时长：连接后必须在此时长内完成 system.authorize。
constexpr int kAuthTimeoutMs = 5000;

// 序列化为紧凑 JSON 文本用于发送。
QString compactJson(const QJsonObject &obj)
{
    return QString::fromUtf8(
        QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// 根据客户端 URL 上的 ?token= 解析出 token（不严格 URL 解析，仅取查询段）。
QString tokenFromUrl(const QUrl &url)
{
    const QString query = url.query(QUrl::FullyDecoded);
    for (const QString &pair : query.split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
        const int eq = pair.indexOf(QLatin1Char('='));
        if (eq > 0 && pair.left(eq) == QLatin1String("token")) {
            return pair.mid(eq + 1);
        }
    }
    return {};
}
} // namespace

RpcServer::RpcServer(BatteryManager *manager, QObject *parent)
    : QObject(parent), m_manager(manager)
{
    // 连接 BatteryManager 的设备更新信号，维护最新快照 + 变化检测推送。
    // devicesUpdated 在主线程触发，RpcServer 也在主线程，无需加锁。
    if (m_manager) {
        connect(m_manager, &BatteryManager::devicesUpdated,
                this, &RpcServer::onDevicesUpdated);
    }
}

RpcServer::~RpcServer()
{
    stop();
}

bool RpcServer::start(const QString &host, int port, const QString &token)
{
    if (m_server) {
        return true; // 已启动
    }

    m_host = host.isEmpty() ? QString::fromLatin1(AppSettings::kDefaultRpcHost) : host;
    m_port = qBound(AppSettings::kMinRpcPort, port, AppSettings::kMaxRpcPort);
    m_token = token;
    m_authRequired = !token.isEmpty();

    m_server = std::make_unique<QWebSocketServer>(
        QStringLiteral("BatteryMonitor RPC"),
        QWebSocketServer::NonSecureMode,
        this);

    if (!m_server->listen(QHostAddress(m_host), static_cast<quint16>(m_port))) {
        const QString reason = m_server->errorString();
        LOG_WARN_W(L"RpcServer listen failed on " + m_host.toStdWString() +
                   L":" + std::to_wstring(m_port) + L" - " + reason.toStdWString());
        m_server.reset();
        emit startFailed(reason);
        return false;
    }

    // 监听成功后，端口 0 会由系统分配实际端口；取回真实值。
    m_port = m_server->serverPort();

    connect(m_server.get(), &QWebSocketServer::newConnection,
            this, &RpcServer::onNewConnection);

    LOG_W(L"RpcServer listening on " + m_host.toStdWString() +
          L":" + std::to_wstring(m_port) +
          (m_authRequired ? L" (auth required)" : L" (no auth)"));
    emit started(m_host, m_port);
    return true;
}

bool RpcServer::isListening() const
{
    return m_server && m_server->isListening();
}

void RpcServer::stop()
{
    if (!m_server) {
        return;
    }
    // 主动关闭所有客户端连接。
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        if (it->socket) {
            it->socket->close();
        }
    }
    m_sessions.clear();
    m_server.reset();
    LOG_W(L"RpcServer stopped");
    emit stopped();
}

// ---------------------------------------------------------------------------
// 连接管理
// ---------------------------------------------------------------------------

void RpcServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QWebSocket *socket = m_server->nextPendingConnection();
        if (!socket) {
            continue;
        }

        Session s;
        s.socket = socket;
        // URL 上带 ?token= 时直接校验，通过则免后续 authorize。
        if (m_authRequired) {
            s.authorized = (tokenFromUrl(socket->requestUrl()) == m_token);
        } else {
            s.authorized = true;
        }
        m_sessions.insert(socket, s);

        connect(socket, &QWebSocket::textMessageReceived, this, [this, socket](const QString &msg) {
            auto it = m_sessions.find(socket);
            if (it == m_sessions.end()) return;
            handleTextMessage(it.value(), msg);
        });
        connect(socket, &QWebSocket::disconnected, this, [this, socket]() {
            m_sessions.remove(socket);
            socket->deleteLater();
        });

        // 需要鉴权但 URL 未带 token -> 启动鉴权超时定时器。
        if (m_authRequired && !s.authorized) {
            QTimer::singleShot(kAuthTimeoutMs, this, [this, socket]() {
                auto it = m_sessions.find(socket);
                if (it == m_sessions.end()) return;
                if (!it->authorized) {
                    socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                                  QStringLiteral("authorization timeout"));
                }
            });
        }
    }
}

void RpcServer::handleTextMessage(Session &s, const QString &text)
{
    // 1) JSON 解析。
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        sendJson(s, JsonRpc::errorNoId(JsonRpc::ErrorCode::ParseError,
                                       QStringLiteral("Parse error")));
        return;
    }

    const QJsonObject obj = doc.object();

    // 2) 解析请求结构。
    const JsonRpc::Request req = JsonRpc::parseRequest(obj);
    if (!req.valid) {
        sendJson(s, JsonRpc::errorNoId(JsonRpc::ErrorCode::InvalidRequest,
                                       req.errorMessage));
        return;
    }

    // 3) 鉴权拦截：未授权时只允许 system.authorize。
    if (m_authRequired && !s.authorized && req.method != QStringLiteral("system.authorize")) {
        if (!req.isNotification) {
            sendJson(s, JsonRpc::errorResponse(
                JsonRpc::ErrorCode::Unauthorized,
                QStringLiteral("Unauthorized; call system.authorize first"),
                req.id));
        }
        return;
    }

    // 4) 分发。通知（无 id）不回包；其余错误由 dispatch 内部构造响应。
    dispatch(s, req);
}

void RpcServer::dispatch(Session &s, const JsonRpc::Request &req)
{
    const auto sendErr = [&](JsonRpc::ErrorCode c, const QString &msg) {
        if (!req.isNotification) {
            sendJson(s, JsonRpc::errorResponse(c, msg, req.id));
        }
    };

    static const QHash<QString,
                       QJsonObject (RpcServer::*)(Session &, const QJsonObject &)> table = {
        {QStringLiteral("system.ping"),             &RpcServer::methodSystemPing},
        {QStringLiteral("system.getInfo"),          &RpcServer::methodSystemGetInfo},
        {QStringLiteral("system.authorize"),        &RpcServer::methodSystemAuthorize},
        {QStringLiteral("devices.list"),            &RpcServer::methodDevicesList},
        {QStringLiteral("devices.get"),             &RpcServer::methodDevicesGet},
        {QStringLiteral("devices.subscribe"),       &RpcServer::methodDevicesSubscribe},
        {QStringLiteral("devices.unsubscribe"),     &RpcServer::methodDevicesUnsubscribe},
        {QStringLiteral("refresh.now"),             &RpcServer::methodRefreshNow},
        {QStringLiteral("settings.app.get"),        &RpcServer::methodSettingsAppGet},
        {QStringLiteral("settings.app.set"),        &RpcServer::methodSettingsAppSet},
        {QStringLiteral("settings.device.get"),     &RpcServer::methodSettingsDeviceGet},
        {QStringLiteral("settings.device.set"),     &RpcServer::methodSettingsDeviceSet},
    };

    const auto it = table.find(req.method);
    if (it == table.end()) {
        sendErr(JsonRpc::ErrorCode::MethodNotFound,
                QStringLiteral("Method not found: ") + req.method);
        return;
    }

    // 约定：方法返回的对象若包含 "error" 键则视为失败，原样回包；
    // 否则视为成功，包成 {jsonrpc,result,id}。
    // 注意：方法内部构造 error 时无法拿到 req.id（统一填 0），这里统一用
    // req.id 覆盖，保证客户端能正确匹配请求。
    QJsonObject resp = (this->*(it.value()))(s, req.params);
    if (resp.contains(QStringLiteral("error"))) {
        if (!req.isNotification) {
            resp["id"] = req.id;
            sendJson(s, resp);
        }
    } else {
        if (!req.isNotification) {
            sendJson(s, JsonRpc::okResult(resp, req.id));
        }
    }
}

void RpcServer::sendJson(Session &s, const QJsonObject &obj)
{
    if (s.socket && s.socket->isValid()) {
        s.socket->sendTextMessage(compactJson(obj));
    }
}

// ---------------------------------------------------------------------------
// system.*
// ---------------------------------------------------------------------------

QJsonObject RpcServer::methodSystemAuthorize(Session &s, const QJsonObject &params)
{
    QString token;
    if (!JsonRpc::getString(params, QStringLiteral("token"), &token)) {
        return JsonRpc::errorResponse(JsonRpc::ErrorCode::InvalidParams,
                                      QStringLiteral("Missing 'token'"), 0);
    }
    if (!m_authRequired || token == m_token) {
        s.authorized = true;
        return {{QStringLiteral("authorized"), true}};
    }
    // 鉴权失败：回错误，并关闭连接。
    s.socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                    QStringLiteral("invalid token"));
    return JsonRpc::errorResponse(JsonRpc::ErrorCode::Unauthorized,
                                  QStringLiteral("Invalid token"), 0);
}

QJsonObject RpcServer::methodSystemPing(Session & /*s*/, const QJsonObject &params)
{
    QJsonObject r;
    r["pong"] = true;
    if (params.contains(QStringLiteral("payload"))) {
        r["payload"] = params.value(QStringLiteral("payload"));
    }
    r["timestamp"] = QDateTime::currentMSecsSinceEpoch();
    return r;
}

QJsonObject RpcServer::methodSystemGetInfo(Session & /*s*/, const QJsonObject & /*params*/)
{
    QJsonObject r;
    r["name"] = QStringLiteral("BatteryMonitor");
    r["version"] = QCoreApplication::applicationVersion();
    r["apiVersion"] = QStringLiteral("1.0");
#ifdef Q_OS_WIN
    r["platform"] = QStringLiteral("windows");
#else
    r["platform"] = QStringLiteral("unknown");
#endif
    static const qint64 startedAt = QDateTime::currentMSecsSinceEpoch();
    r["startedAt"] = startedAt;
    r["uptime"] = QDateTime::currentMSecsSinceEpoch() - startedAt;
    r["refreshIntervalMs"] = AppSettings::refreshInterval();
    // capabilities 与 main.cpp 注册的 provider 对应。
    QJsonArray caps = {
        QStringLiteral("bluetooth"), QStringLiteral("classicBluetooth"),
        QStringLiteral("airpods"), QStringLiteral("xbox"),
        QStringLiteral("hid")};
    r["capabilities"] = caps;
    return r;
}

// ---------------------------------------------------------------------------
// devices.*
// ---------------------------------------------------------------------------

QJsonObject RpcServer::methodDevicesList(Session & /*s*/, const QJsonObject & /*params*/)
{
    return DeviceJson::listToJson(m_devices, m_devicesUpdatedAtMs);
}

QJsonObject RpcServer::methodDevicesGet(Session & /*s*/, const QJsonObject &params)
{
    QString id;
    if (!JsonRpc::getString(params, QStringLiteral("id"), &id)) {
        return JsonRpc::errorResponse(JsonRpc::ErrorCode::InvalidParams,
                                      QStringLiteral("Missing 'id'"), 0);
    }
    for (const auto &d : m_devices) {
        if (QString::fromStdWString(d.id) == id) {
            return {{QStringLiteral("device"), DeviceJson::toJson(d)}};
        }
    }
    QJsonObject data;
    data["id"] = id;
    return JsonRpc::errorResponse(JsonRpc::ErrorCode::DeviceNotFound,
                                  QStringLiteral("Device not found"), 0, data);
}

QJsonObject RpcServer::methodDevicesSubscribe(Session &s, const QJsonObject &params)
{
    // 事件白名单。
    QSet<QString> events;
    const QJsonValue ev = params.value(QStringLiteral("events"));
    if (ev.isArray()) {
        for (const auto &v : ev.toArray()) {
            if (v.isString()) events.insert(v.toString());
        }
    } else if (ev.isUndefined()) {
        // 省略 => 订阅全部。
        events.insert(QStringLiteral("devices.updated"));
        events.insert(QStringLiteral("refresh.completed"));
    }
    // 只接受已知事件名。
    events.intersect({QStringLiteral("devices.updated"),
                      QStringLiteral("refresh.completed")});

    s.subscribedEvents = events;
    s.subscriptionId = QStringLiteral("s-%1").arg(++m_nextSubscriptionId);

    QJsonObject r;
    QJsonArray evArr;
    for (const QString &e : events) evArr.append(e);
    r["subscribed"] = evArr;
    r["subscriptionId"] = s.subscriptionId;

    // immediate：订阅后立即补发一帧当前快照。
    bool immediate = false;
    JsonRpc::getBool(params, QStringLiteral("immediate"), &immediate);
    if (immediate && events.contains(QStringLiteral("devices.updated"))) {
        QJsonObject p = DeviceJson::listToJson(m_devices, m_devicesUpdatedAtMs);
        p["subscriptionId"] = s.subscriptionId;
        sendJson(s, JsonRpc::notification(
                     QStringLiteral("devices.updated"), p));
    }
    return r;
}

QJsonObject RpcServer::methodDevicesUnsubscribe(Session &s, const QJsonObject & /*params*/)
{
    s.subscribedEvents.clear();
    const QString id = s.subscriptionId;
    s.subscriptionId.clear();
    QJsonObject r;
    QJsonArray arr;
    if (!id.isEmpty()) arr.append(id);
    r["unsubscribed"] = arr;
    return r;
}

// ---------------------------------------------------------------------------
// refresh.*
// ---------------------------------------------------------------------------

QJsonObject RpcServer::methodRefreshNow(Session & /*s*/, const QJsonObject &params)
{
    if (!m_manager) {
        return JsonRpc::errorResponse(JsonRpc::ErrorCode::InternalError,
                                      QStringLiteral("Manager unavailable"), 0);
    }
    m_manager->refreshNow();

    bool wait = false;
    JsonRpc::getBool(params, QStringLiteral("wait"), &wait);

    QJsonObject r;
    r["queued"] = true;
    if (wait) {
        // 阻塞至下一次 devicesUpdated（最多 10s）。由于信号在同一线程，
        // 用 QEventLoop + QTimer 实现"等下一帧"。
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject scope; // 作为信号连接的作用域宿主，析构时自动断开。
        QObject::connect(m_manager, &BatteryManager::devicesUpdated, &scope,
                         [&loop](const QList<BatteryDevice> &) { loop.quit(); });
        QObject::connect(&timeout, &QTimer::timeout, &scope,
                         [&loop]() { loop.quit(); });
        timeout.start(10000);
        loop.exec(QEventLoop::ExcludeUserInputEvents);
        r["devices"] = DeviceJson::listToJson(m_devices, m_devicesUpdatedAtMs)
                           .value(QStringLiteral("devices")).toArray();
        r["updatedAt"] = m_devicesUpdatedAtMs;
    }
    return r;
}

// ---------------------------------------------------------------------------
// settings.app.* / settings.device.*
// ---------------------------------------------------------------------------

QJsonObject RpcServer::buildAppSettingsObject() const
{
    QJsonObject r;
    r["refreshIntervalMs"] = AppSettings::refreshInterval();
    r["staleRetentionSec"] = AppSettings::staleRetentionSec();
    r["language"] = AppSettings::language();
    r["theme"] = AppSettings::theme();
    r["hideUnpairedAirPods"] = AppSettings::hideUnpairedAirPods();
    r["autoStart"] = AppSettings::startupAutoStart();
    QJsonObject rpc;
    rpc["enabled"] = AppSettings::rpcEnabled();
    rpc["host"] = AppSettings::rpcHost();
    rpc["port"] = AppSettings::rpcPort();
    rpc["token"] = AppSettings::rpcToken(); // 注意：会回显 token
    r["rpc"] = rpc;
    return r;
}

QJsonObject RpcServer::buildDeviceSettingsObject(const QString &id) const
{
    QJsonObject r;
    r["alias"] = DeviceSettings::alias(id);
    r["trayVisible"] = DeviceSettings::trayVisible(id);
    r["alertEnabled"] = DeviceSettings::alertEnabled(id);
    r["lowBatteryThreshold"] = DeviceSettings::lowBatteryThreshold(id);
    r["alertPolicy"] = DeviceSettings::policyToString(DeviceSettings::alertPolicy(id));
    r["keepCachedForever"] = DeviceSettings::keepCachedForever(id);
    return r;
}

QJsonObject RpcServer::methodSettingsAppGet(Session & /*s*/, const QJsonObject & /*params*/)
{
    return buildAppSettingsObject();
}

QJsonObject RpcServer::methodSettingsDeviceGet(Session & /*s*/, const QJsonObject &params)
{
    QString id;
    if (!JsonRpc::getString(params, QStringLiteral("id"), &id)) {
        return JsonRpc::errorResponse(JsonRpc::ErrorCode::InvalidParams,
                                      QStringLiteral("Missing 'id'"), 0);
    }
    return buildDeviceSettingsObject(id);
}

QJsonObject RpcServer::methodSettingsAppSet(Session & /*s*/, const QJsonObject &params)
{
    if (params.contains(QStringLiteral("refreshIntervalMs"))) {
        int v;
        if (!JsonRpc::getInt(params, QStringLiteral("refreshIntervalMs"), &v) ||
            v < 1000 || v > 3600000) {
            QJsonObject d;
            d["field"] = QStringLiteral("refreshIntervalMs");
            d["constraint"] = QStringLiteral("1000..3600000");
            return JsonRpc::errorResponse(JsonRpc::ErrorCode::InvalidParams,
                                     QStringLiteral("Invalid params"), 0, d);
        }
        AppSettings::setRefreshInterval(v);
    }
    if (params.contains(QStringLiteral("staleRetentionSec"))) {
        int v;
        if (!JsonRpc::getInt(params, QStringLiteral("staleRetentionSec"), &v) ||
            v < 0 || v > 86400) {
            QJsonObject d;
            d["field"] = QStringLiteral("staleRetentionSec");
            d["constraint"] = QStringLiteral("0..86400");
            return JsonRpc::errorResponse(JsonRpc::ErrorCode::InvalidParams,
                                     QStringLiteral("Invalid params"), 0, d);
        }
        AppSettings::setStaleRetentionSec(v);
    }
    if (params.contains(QStringLiteral("language"))) {
        AppSettings::setLanguage(params.value(QStringLiteral("language")).toString());
    }
    if (params.contains(QStringLiteral("theme"))) {
        const QString t = params.value(QStringLiteral("theme")).toString();
        if (t != QStringLiteral("system") && t != QStringLiteral("light") &&
            t != QStringLiteral("dark")) {
            QJsonObject d;
            d["field"] = QStringLiteral("theme");
            d["constraint"] = QStringLiteral("system|light|dark");
            return JsonRpc::errorResponse(JsonRpc::ErrorCode::InvalidParams,
                                     QStringLiteral("Invalid params"), 0, d);
        }
        AppSettings::setTheme(t);
    }
    if (params.contains(QStringLiteral("hideUnpairedAirPods"))) {
        bool v;
        if (!JsonRpc::getBool(params, QStringLiteral("hideUnpairedAirPods"), &v)) {
            return JsonRpc::errorResponse(JsonRpc::ErrorCode::InvalidParams,
                                     QStringLiteral("Invalid hideUnpairedAirPods"), 0);
        }
        AppSettings::setHideUnpairedAirPods(v);
    }
    if (params.contains(QStringLiteral("autoStart"))) {
        bool v;
        if (!JsonRpc::getBool(params, QStringLiteral("autoStart"), &v)) {
            return JsonRpc::errorResponse(JsonRpc::ErrorCode::InvalidParams,
                                     QStringLiteral("Invalid autoStart"), 0);
        }
        AppSettings::setStartupAutoStart(v);
    }
    // rpc 子对象：写入后需重启 Server 才完全生效。
    bool needRestart = false;
    if (params.contains(QStringLiteral("rpc"))) {
        const QJsonObject rpc = params.value(QStringLiteral("rpc")).toObject();
        if (rpc.contains(QStringLiteral("enabled"))) {
            AppSettings::setRpcEnabled(rpc.value(QStringLiteral("enabled")).toBool());
            needRestart = true;
        }
        if (rpc.contains(QStringLiteral("host"))) {
            AppSettings::setRpcHost(rpc.value(QStringLiteral("host")).toString());
            needRestart = true;
        }
        if (rpc.contains(QStringLiteral("port"))) {
            int p;
            if (!JsonRpc::getInt(rpc, QStringLiteral("port"), &p) ||
                p < AppSettings::kMinRpcPort || p > AppSettings::kMaxRpcPort) {
                QJsonObject d;
                d["field"] = QStringLiteral("rpc.port");
                d["constraint"] = QStringLiteral("1024..65535");
                return JsonRpc::errorResponse(JsonRpc::ErrorCode::InvalidParams,
                                         QStringLiteral("Invalid params"), 0, d);
            }
            AppSettings::setRpcPort(p);
            needRestart = true;
        }
        if (rpc.contains(QStringLiteral("token"))) {
            AppSettings::setRpcToken(rpc.value(QStringLiteral("token")).toString());
            needRestart = true;
        }
    }

    QJsonObject r = buildAppSettingsObject();
    if (needRestart) {
        r["needRestart"] = true;
    }
    return r;
}

QJsonObject RpcServer::methodSettingsDeviceSet(Session & /*s*/, const QJsonObject &params)
{
    QString id;
    if (!JsonRpc::getString(params, QStringLiteral("id"), &id)) {
        return JsonRpc::errorResponse(JsonRpc::ErrorCode::InvalidParams,
                                 QStringLiteral("Missing 'id'"), 0);
    }
    if (params.contains(QStringLiteral("alias"))) {
        DeviceSettings::setAlias(id, params.value(QStringLiteral("alias")).toString());
    }
    if (params.contains(QStringLiteral("trayVisible"))) {
        bool v;
        if (!JsonRpc::getBool(params, QStringLiteral("trayVisible"), &v)) {
            return JsonRpc::errorResponse(JsonRpc::ErrorCode::InvalidParams,
                                     QStringLiteral("Invalid trayVisible"), 0);
        }
        DeviceSettings::setTrayVisible(id, v);
    }
    if (params.contains(QStringLiteral("alertEnabled"))) {
        bool v;
        if (!JsonRpc::getBool(params, QStringLiteral("alertEnabled"), &v)) {
            return JsonRpc::errorResponse(JsonRpc::ErrorCode::InvalidParams,
                                     QStringLiteral("Invalid alertEnabled"), 0);
        }
        DeviceSettings::setAlertEnabled(id, v);
    }
    if (params.contains(QStringLiteral("lowBatteryThreshold"))) {
        int v;
        if (!JsonRpc::getInt(params, QStringLiteral("lowBatteryThreshold"), &v) ||
            v < 1 || v > DeviceSettings::kMaxThreshold) {
            QJsonObject d;
            d["field"] = QStringLiteral("lowBatteryThreshold");
            d["constraint"] = QStringLiteral("1..100");
            return JsonRpc::errorResponse(JsonRpc::ErrorCode::InvalidParams,
                                     QStringLiteral("Invalid params"), 0, d);
        }
        DeviceSettings::setLowBatteryThreshold(id, v);
    }
    if (params.contains(QStringLiteral("alertPolicy"))) {
        const QString p = params.value(QStringLiteral("alertPolicy")).toString();
        const QStringList valid = {
            QStringLiteral("once"), QStringLiteral("always"),
            QStringLiteral("5min"), QStringLiteral("15min"),
            QStringLiteral("30min"), QStringLiteral("60min")};
        if (!valid.contains(p)) {
            QJsonObject d;
            d["field"] = QStringLiteral("alertPolicy");
            d["constraint"] = valid.join(QLatin1Char('|'));
            return JsonRpc::errorResponse(JsonRpc::ErrorCode::InvalidParams,
                                     QStringLiteral("Invalid params"), 0, d);
        }
        DeviceSettings::setAlertPolicy(id, DeviceSettings::stringToPolicy(p));
    }
    if (params.contains(QStringLiteral("keepCachedForever"))) {
        bool v;
        if (!JsonRpc::getBool(params, QStringLiteral("keepCachedForever"), &v)) {
            return JsonRpc::errorResponse(JsonRpc::ErrorCode::InvalidParams,
                                     QStringLiteral("Invalid keepCachedForever"), 0);
        }
        DeviceSettings::setKeepCachedForever(id, v);
    }
    return buildDeviceSettingsObject(id);
}

// ---------------------------------------------------------------------------
// 推送（devices.updated / refresh.completed）
// ---------------------------------------------------------------------------

void RpcServer::onDevicesUpdated(const QList<BatteryDevice> &devices)
{
    m_devices = devices;
    m_devicesUpdatedAtMs = QDateTime::currentMSecsSinceEpoch();

    // 变化检测：签名集合与上次推送不同才广播 devices.updated。
    QSet<QString> sigs;
    sigs.reserve(devices.size());
    for (const auto &d : devices) {
        sigs.insert(DeviceJson::signature(d));
    }
    const bool changed = sigs != m_lastPushedSignatures;
    m_lastPushedSignatures = sigs;

    if (changed) {
        broadcastDevicesUpdated();
    }
    // refresh.completed 总是推（订阅者用它感知周期边界）。
    broadcastRefreshCompleted();
}

void RpcServer::broadcastDevicesUpdated()
{
    if (m_sessions.isEmpty()) return;
    const QJsonObject payload = DeviceJson::listToJson(m_devices, m_devicesUpdatedAtMs);
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        Session &s = it.value();
        if (s.authorized &&
            s.subscribedEvents.contains(QStringLiteral("devices.updated"))) {
            QJsonObject p = payload;
            p["subscriptionId"] = s.subscriptionId;
            sendJson(s, JsonRpc::notification(
                          QStringLiteral("devices.updated"), p));
        }
    }
}

void RpcServer::broadcastRefreshCompleted()
{
    if (m_sessions.isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        Session &s = it.value();
        if (s.authorized &&
            s.subscribedEvents.contains(QStringLiteral("refresh.completed"))) {
            QJsonObject p;
            p["at"] = now;
            p["subscriptionId"] = s.subscriptionId;
            sendJson(s, JsonRpc::notification(
                          QStringLiteral("refresh.completed"), p));
        }
    }
}
