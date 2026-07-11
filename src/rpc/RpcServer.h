#pragma once

#include "JsonRpc.h"
#include "src/core/BatteryDevice.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>

class BatteryManager;
class QWebSocket;
class QWebSocketServer;

// WebSocket JSON-RPC 2.0 Server。
//
// 与 MainWindow 平级：持有 BatteryManager*，连接 devicesUpdated 信号获取设备快照，
// 并通过 QWebSocketServer 向外部客户端暴露 JSON-RPC 接口。
//
// 生命周期：
//   1. 构造（main.cpp）：RpcServer server(&manager, this);
//   2. start(host, port)：listen；失败返回 false（端口被占等）。
//   3. 运行期：devicesUpdated 触发 -> 内部做变化检测 -> 向订阅者推送 devices.updated。
//   4. 析构：自动关闭所有连接。
//
// 线程模型：全部运行在主线程 Qt 事件循环（与 BatteryManager::devicesUpdated 同线程），
// 因此对 m_devices 等成员的访问无需加锁。
//
// 鉴权：rpcToken() 非空时，客户端连接后必须在 5 秒内完成 system.authorize
// （或在 URL 附加 ?token=），否则关闭连接（Close code 4401）。
class RpcServer : public QObject
{
    Q_OBJECT

public:
    explicit RpcServer(BatteryManager *manager, QObject *parent = nullptr);
    ~RpcServer() override;

    // 在给定 host:port 上监听。host 为 "127.0.0.1"（仅本机）或 "0.0.0.0"（全部网卡）。
    // token 为空表示不启用鉴权。失败返回 false。
    bool start(const QString &host, int port, const QString &token);

    // 是否已成功监听。
    bool isListening() const;

    // 实际监听的地址与端口（成功监听后填充）。
    QString listenHost() const { return m_host; }
    int listenPort() const { return m_port; }

    // 关闭服务并断开所有客户端。
    void stop();

signals:
    // 监听成功 / 失败时发出，便于 UI 提示。
    void started(const QString &host, int port);
    void startFailed(const QString &reason);
    void stopped();

private slots:
    void onNewConnection();
    // BatteryManager::devicesUpdated -> 缓存最新快照 + 变化检测 + 推送订阅者。
    void onDevicesUpdated(const QList<BatteryDevice> &devices);

private:
    // 单个连接的会话状态。
    struct Session
    {
        QWebSocket *socket = nullptr;
        bool authorized = true;       // token 为空时恒为 true
        // 已订阅的事件集合（devices.updated / refresh.completed）。
        QSet<QString> subscribedEvents;
        // 本连接的订阅句柄（每个连接一个）。
        QString subscriptionId;
    };

    // —— 处理一条客户端消息 ——
    void handleTextMessage(Session &s, const QString &text);
    // 分发单个已解析请求。
    void dispatch(Session &s, const JsonRpc::Request &req);
    // 向单个连接发送 JSON。
    void sendJson(Session &s, const QJsonObject &obj);

    // —— 各方法实现（返回响应对象；对通知返回空的 QJsonObject() 表示不回包）——
    QJsonObject methodSystemPing(Session &s, const QJsonObject &params);
    QJsonObject methodSystemGetInfo(Session &s, const QJsonObject &params);
    QJsonObject methodSystemAuthorize(Session &s, const QJsonObject &params);
    QJsonObject methodDevicesList(Session &s, const QJsonObject &params);
    QJsonObject methodDevicesGet(Session &s, const QJsonObject &params);
    QJsonObject methodDevicesSubscribe(Session &s, const QJsonObject &params);
    QJsonObject methodDevicesUnsubscribe(Session &s, const QJsonObject &params);
    QJsonObject methodRefreshNow(Session &s, const QJsonObject &params);
    QJsonObject methodSettingsAppGet(Session &s, const QJsonObject &params);
    QJsonObject methodSettingsAppSet(Session &s, const QJsonObject &params);
    QJsonObject methodSettingsDeviceGet(Session &s, const QJsonObject &params);
    QJsonObject methodSettingsDeviceSet(Session &s, const QJsonObject &params);

    // 不需要 Session 上下文的纯构造（供 set 后回读对账与 get 共用）。
    QJsonObject buildAppSettingsObject() const;
    QJsonObject buildDeviceSettingsObject(const QString &id) const;

    // 推送 devices.updated 给所有订阅了该事件的连接。
    void broadcastDevicesUpdated();
    // 推送 refresh.completed（目前在 onDevicesUpdated 内复用）。
    void broadcastRefreshCompleted();

    BatteryManager *m_manager;
    std::unique_ptr<QWebSocketServer> m_server;
    QString m_host;
    int m_port = 0;
    QString m_token;
    bool m_authRequired = false;

    // socket -> Session。主线程访问，无需锁。
    QHash<QWebSocket *, Session> m_sessions;
    // 单调递增的订阅 id 计数。
    int m_nextSubscriptionId = 0;

    // 最新设备快照（由 devicesUpdated 维护）。
    QList<BatteryDevice> m_devices;
    qint64 m_devicesUpdatedAtMs = 0;
    // 上次推送时的签名集合（变化检测）。
    QSet<QString> m_lastPushedSignatures;
};
