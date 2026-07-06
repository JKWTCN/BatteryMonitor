#pragma once

#include "BatteryDevice.h"
#include "IBatteryProvider.h"

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QThread>
#include <memory>
#include <vector>

class QTimer;

// 聚合所有 IBatteryProvider。每个 provider 在独立 worker 线程里轮询；
// 任一 provider 返回结果后立即合并当前快照并刷新 UI，避免慢扫描阻塞其它设备。
//
// 分层说明：
//   - 核心数据结构 BatteryDevice / IBatteryProvider 刻意不依赖 Qt（用 std::wstring），
//     便于 battery 层独立复用。
//   - BatteryManager 是唯一的 Qt 边界：provider worker 读到 std::vector<BatteryDevice>
//     后转成 QList，再回到主线程聚合、过滤、缓存并发信号给 UI。
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

    // 设置粘性缓存保留窗口（秒）。默认 180（3 分钟）。
    // 设备单轮读不到时，沿用上次读数继续展示的保留时长；0 = 从不缓存。
    // 设备级“永久缓存”不受 0 秒限制，仍会保留。
    // 设置后下一轮 refresh 即生效。
    void setStaleRetention(int sec);

    // 请求立即刷新（异步，触发一次 worker 循环）。
    void refreshNow();

    // 设备类别的人类可读名称，供 UI 翻译 / 展示。
    static QString typeLabel(BatteryDevice::Type type);

signals:
    // 设备列表更新时发出（在主线程触发，可安全更新 UI）。
    void devicesUpdated(const QList<BatteryDevice> &devices);

private slots:
    // 由内部 QTimer 触发：请求 worker 刷新。
    void scheduleRefresh();
    // 单个 provider 完成刷新后合并当前所有 provider 快照并发布。
    void onProviderRefreshed(int providerIndex, const QList<BatteryDevice> &devices);

private:
    struct ProviderRuntime
    {
        std::unique_ptr<QThread> thread;
        QObject *worker = nullptr;
        QList<BatteryDevice> devices;
        bool inFlight = false;
        bool pendingRefresh = false;
        bool hasSnapshot = false;
    };

    // 设备签名：把影响“是否需要再记一条日志”的字段拼成一个可比较的字符串。
    static std::wstring deviceSignature(const BatteryDevice &d);

    // 聚合当前 provider 快照、应用展示过滤与粘性缓存，然后发给 UI。
    void publishCurrentDevices(const QSet<std::wstring> &freshIds);
    // 向单个 provider worker 投递一次刷新；已在刷新时只记录一个待刷新标记。
    void requestProviderRefresh(int providerIndex);

    // 仅当设备列表相对上一版发生变化时，写一条 INFO 摘要；否则静默。
    void logSummaryIfChanged(const std::vector<BatteryDevice> &devices);

    // 粘性缓存合并。freshIds 只包含刚刚完成刷新的 provider 返回的设备；
    // 其它 provider 的旧快照参与展示，但不会刷新 lastSeenMsecs。
    void applyStickyCache(std::vector<BatteryDevice> &raw,
                          const QSet<std::wstring> &freshIds);

    // AirPods 展示过滤必须在所有 provider 当前快照聚合后做。
    void filterUnpairedAirPods(std::vector<BatteryDevice> &raw);

    std::vector<std::unique_ptr<IBatteryProvider>> m_providers;
    std::vector<ProviderRuntime> m_providerRuntimes;
    QTimer *m_timer = nullptr;

    // 上一版设备签名集合（已排序），用于变化检测。
    std::vector<std::wstring> m_lastSignatures;
    bool m_hasSnapshot = false;

    // 粘性缓存：id -> 上次成功读数。只在主线程访问。
    QHash<std::wstring, BatteryDevice> m_cache;
    // 保留窗口（毫秒）。0 = 普通设备不展示缓存，设备级“永久缓存”仍保留。
    quint64 m_staleRetentionMsec = 180 * 1000;
};
