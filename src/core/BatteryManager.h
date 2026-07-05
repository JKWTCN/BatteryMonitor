#pragma once

#include "BatteryDevice.h"
#include "IBatteryProvider.h"

#include <QMutex>
#include <QObject>
#include <QThread>
#include <memory>
#include <vector>

class QTimer;

namespace detail { class RefreshWorker; }

// 聚合所有 IBatteryProvider，在独立 worker 线程定时轮询，并通过信号把
// 最新设备列表（按 queued 连接）回送到 UI 线程，保证界面永不卡顿。
//
// 分层说明：
//   - 核心数据结构 BatteryDevice / IBatteryProvider 刻意不依赖 Qt（用 std::wstring），
//     便于 battery 层独立复用。
//   - BatteryManager 是唯一的 Qt 边界：worker 用纯 C++ provider 读到
//     std::vector<BatteryDevice> 后，在此处把 std::wstring 转成 QString，
//     再通过 Qt 信号发给 UI。
class BatteryManager : public QObject
{
    Q_OBJECT

public:
    explicit BatteryManager(QObject *parent = nullptr);
    ~BatteryManager() override;

    // 注册一个新的电量提供者。所有权归 Manager。
    // 应在 start() 之前调用。
    void addProvider(std::unique_ptr<IBatteryProvider> provider);

    // 启动后台轮询（首次立即刷新一次）。
    void start();

    // 设置轮询间隔（毫秒）。默认 10s。
    void setInterval(int msec);

    // 请求立即刷新（异步，触发一次 worker 循环）。
    void refreshNow();

    // 设备类别的人类可读名称，供 UI 翻译 / 展示。
    static QString typeLabel(BatteryDevice::Type type);

signals:
    // 设备列表更新时发出（在主线程触发，可安全更新 UI）。
    void devicesUpdated(const QList<BatteryDevice> &devices);
    // 内部信号：把刷新请求从主线程投递到 worker 线程（queued 连接）。
    void requestRefresh();

private slots:
    // 由内部 QTimer 触发：请求 worker 刷新。
    void scheduleRefresh();

private:
    std::vector<std::unique_ptr<IBatteryProvider>> m_providers;
    QThread m_workerThread;
    QTimer *m_timer = nullptr;
};
