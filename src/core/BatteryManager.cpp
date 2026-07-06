#include "BatteryManager.h"
#include "util/AppSettings.h"
#include "util/DeviceSettings.h"
#include "util/Logger.h"

#include <QDateTime>
#include <QHash>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <cwctype>
#include <set>
#include <sstream>
#include <utility>

namespace
{
std::wstring lowerCopy(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

bool looksLikeAppleAudioName(const std::wstring &name)
{
    const std::wstring lower = lowerCopy(name);
    return lower.find(L"airpods") != std::wstring::npos ||
           lower.find(L"airpod") != std::wstring::npos ||
           lower.find(L"beats") != std::wstring::npos ||
           lower.find(L"powerbeats") != std::wstring::npos;
}

// 单 provider worker：每个 provider 独占一个线程，互不阻塞。
// BatteryManager 在主线程中合并各 worker 的最新快照，并维护粘性缓存。
class ProviderWorker : public QObject
{
    Q_OBJECT

public:
    ProviderWorker(int index, IBatteryProvider *provider)
        : m_index(index), m_provider(provider)
    {
    }

public slots:
    // 调用单个 provider。调用方保证同一 provider 不会并发进入 refresh()。
    void refresh()
    {
        QList<BatteryDevice> qtDevices;
        if (!m_provider) {
            emit refreshed(m_index, qtDevices);
            return;
        }

        try {
            auto partial = m_provider->readDevices();
            qtDevices.reserve(static_cast<int>(partial.size()));
            for (auto &device : partial) {
                qtDevices.append(std::move(device));
            }
        } catch (const std::exception &e) {
            const std::wstring name = m_provider->displayName();
            LOG_ERR_W(L"[BatteryManager] provider '" + name +
                      L"' threw: " + QString::fromUtf8(e.what()).toStdWString());
        } catch (...) {
            const std::wstring name = m_provider->displayName();
            LOG_ERR_W(L"[BatteryManager] provider '" + name +
                      L"' threw unknown exception");
        }

        emit refreshed(m_index, qtDevices);
    }

signals:
    void refreshed(int providerIndex, const QList<BatteryDevice> &devices);

private:
    int m_index = -1;
    IBatteryProvider *m_provider = nullptr;
};
} // namespace

// —— BatteryManager 私有辅助实现 ——

std::wstring BatteryManager::deviceSignature(const BatteryDevice &d)
{
    // 用一个不可能出现在字段里的分隔符 '|'，避免歧义拼接。
    // 仅纳入“对日志/状态展示有意义的”字段：电量、连接、充电、类型。
    // 不含 name —— 改名不算状态变化，不应当作“变化”刷屏。
    // 含 stale —— “进入过期” / “退出过期”也要记一条 INFO，便于观察缓存行为。
    std::wostringstream ss;
    ss << d.id << L'|'
       << static_cast<int>(d.type) << L'|'
       << static_cast<int>(d.subType) << L'|'
       << d.percentage << L'|'
       << static_cast<int>(d.level) << L'|'
       << d.leftPercent << L'|'
       << d.rightPercent << L'|'
       << d.casePercent << L'|'
       << (d.charging ? 1 : 0) << L'|'
       << (d.paired ? 1 : 0) << L'|'
       << (d.wired ? 1 : 0) << L'|'
       << (d.connected ? 1 : 0) << L'|'
       << (d.stale ? 1 : 0);
    return ss.str();
}

void BatteryManager::logSummaryIfChanged(const std::vector<BatteryDevice> &devices)
{
    // 收集本轮所有设备签名，排序后整体比较 —— 设备增删、顺序变化都能识别。
    std::vector<std::wstring> sigs;
    sigs.reserve(devices.size());
    for (const auto &d : devices) {
        sigs.push_back(deviceSignature(d));
    }
    std::sort(sigs.begin(), sigs.end());

    const bool changed = !m_hasSnapshot || (sigs != m_lastSignatures);
    m_lastSignatures = sigs;
    m_hasSnapshot = true;

    if (!changed) {
        return; // 设备读数完全稳定，本轮静默，不再写任何行。
    }

    // 发生变化：写一条 INFO 摘要。把每台设备压缩成 "name=pct%"，
    // 既足以排查“谁变了”，又不至于像各 provider 明细那样每轮刷几十行。
    std::wostringstream ss;
    ss << L"[BatteryManager] devices changed (" << devices.size() << L"): ";
    for (size_t i = 0; i < devices.size(); ++i) {
        if (i > 0) {
            ss << L", ";
        }
        const auto &d = devices[i];
        ss << d.name;
        if (d.subType == BatteryDevice::SubType::AirPods) {
            ss << L"[L" << d.leftPercent
               << L"/R" << d.rightPercent
               << L"/C" << d.casePercent << L"]";
        } else if (d.percentage >= 0) {
            ss << L"=" << d.percentage << L"%";
        } else {
            ss << L"=?"; // 离散档位无精确值（如 Xbox 离线）
        }
        if (!d.connected) {
            ss << L"(offline)";
        }
        if (d.stale) {
            ss << L"(stale)";
        }
    }
    LOG_W(ss.str()); // 摘要走 INFO：变化时才到这里，平时整轮静默
}

void BatteryManager::applyStickyCache(std::vector<BatteryDevice> &raw,
                                      const QSet<std::wstring> &freshIds)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // 1) 把刚完成刷新、且本次读到的设备刷入缓存，并更新时间戳。
    //    其它 provider 的旧快照可以参与展示，但不刷新 lastSeenMsecs，
    //    避免快 provider 的频繁刷新让慢 provider 的旧值永不过期。
    QSet<std::wstring> seenIds;
    seenIds.reserve(static_cast<int>(raw.size()));
    for (auto &d : raw) {
        seenIds.insert(d.id);
        if (freshIds.contains(d.id)) {
            d.lastSeenMsecs = now;
            d.stale = false;
            m_cache.insert(d.id, d);
        }
    }

    // 2) 缓存里当前聚合快照未出现的设备：保留窗口内沿用旧值并标 stale，超时则移除。
    //    特例：用户对该设备勾选了“永久缓存”时，无视超时窗口，永久以 stale 值保留。
    //    即便全局粘性缓存被关闭（m_staleRetentionMsec==0），永久缓存设备仍保留；
    //    普通设备则不展示旧值，保持“从不缓存”的瞬时移除语义。
    std::vector<BatteryDevice> staleRevived;
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (seenIds.contains(it.key())) {
            ++it;
            continue;
        }
        if (AppSettings::hideUnpairedAirPods()
            && it.value().subType == BatteryDevice::SubType::AirPods
            && !it.value().paired) {
            it = m_cache.erase(it);
            continue;
        }
        const quint64 age = static_cast<quint64>(now - it.value().lastSeenMsecs);
        const bool keepForever = DeviceSettings::keepCachedForever(
            QString::fromStdWString(it.key()));
        if (keepForever || (m_staleRetentionMsec > 0 && age <= m_staleRetentionMsec)) {
            BatteryDevice revived = it.value();
            revived.stale = true;
            staleRevived.push_back(revived);
            ++it;
        } else {
            // 超出保留窗口：设备真正离开了，从缓存移除。
            it = m_cache.erase(it);
        }
    }

    // 3) stale 设备补回到 raw 末尾（实时设备在前，stale 在后，避免横跳时位置抖动）。
    raw.insert(raw.end(),
               std::make_move_iterator(staleRevived.begin()),
               std::make_move_iterator(staleRevived.end()));
}

void BatteryManager::filterUnpairedAirPods(std::vector<BatteryDevice> &raw)
{
    int unpairedAirPodsCount = 0;
    int airPodsBroadcastCount = 0;
    bool hasConnectedAppleAudioPlaceholder = false;
    for (const auto &d : raw) {
        if (d.subType == BatteryDevice::SubType::AirPods) {
            ++airPodsBroadcastCount;
            if (!d.paired) {
                ++unpairedAirPodsCount;
            }
        }
        if (d.subType == BatteryDevice::SubType::Generic &&
            d.type == BatteryDevice::Type::Bluetooth &&
            d.connected &&
            looksLikeAppleAudioName(d.name)) {
            hasConnectedAppleAudioPlaceholder = true;
        }
    }

    // 如果广播地址和 Windows 配对地址对不上，但当前只有一个 AirPods 广播，
    // 且系统里已有一个已连接的 AirPods/Beats 蓝牙记录，则把它视为本机设备。
    const bool allowSingleByConnectedPlaceholder =
        AppSettings::hideUnpairedAirPods() &&
        airPodsBroadcastCount == 1 &&
        unpairedAirPodsCount == 1 &&
        hasConnectedAppleAudioPlaceholder;

    raw.erase(std::remove_if(raw.begin(), raw.end(),
        [allowSingleByConnectedPlaceholder](BatteryDevice &d) {
            if (d.subType != BatteryDevice::SubType::AirPods) {
                return false;
            }
            // BLE 广播没提供任何一路有效电量时，不展示未知电量行。
            // 如果缓存里有上一轮有效值，applyStickyCache() 会在后面补回。
            if (d.percentage < 0) {
                LOG_VERBOSE_W(L"[BatteryManager] hide AirPods broadcast without battery: " + d.name);
                return true;
            }
            if (d.paired || !AppSettings::hideUnpairedAirPods()) {
                return false;
            }
            if (allowSingleByConnectedPlaceholder) {
                d.paired = true;
                return false;
            }
            LOG_VERBOSE_W(L"[BatteryManager] hide unpaired AirPods broadcast: " + d.name);
            return true;
        }), raw.end());

    // 隐藏普通 BluetoothProvider/ClassicBluetoothProvider 产生的
    // “已连接但电量未知”AirPods 占位项。它只用于上面的匹配兜底，不进入 UI。
    // 如果曾经收到过有效 AirPods 广播，applyStickyCache() 会补回最后一次有效电量；
    // 如果从未收到过，就不显示 AirPods 电量行。
    raw.erase(std::remove_if(raw.begin(), raw.end(),
        [](const BatteryDevice &d) {
            return d.subType == BatteryDevice::SubType::Generic &&
                   d.type == BatteryDevice::Type::Bluetooth &&
                   d.connected &&
                   d.percentage < 0 &&
                   d.level == BatteryLevel::Unknown &&
                   looksLikeAppleAudioName(d.name);
        }), raw.end());
}

void BatteryManager::publishCurrentDevices(const QSet<std::wstring> &freshIds)
{
    std::vector<BatteryDevice> raw;
    for (const auto &runtime : m_providerRuntimes) {
        if (!runtime.hasSnapshot) {
            continue;
        }
        raw.reserve(raw.size() + static_cast<size_t>(runtime.devices.size()));
        for (const auto &device : runtime.devices) {
            raw.push_back(device);
        }
    }

    filterUnpairedAirPods(raw);

    // 与粘性缓存合并：刚返回的 provider 读到的设备刷新时间戳；
    // 当前聚合快照里没有的旧设备按保留窗口以 stale=true 补回。
    applyStickyCache(raw, freshIds);

    // 变化检测：只有设备集合 / 电量 / 连接状态发生变化时才写一条 INFO 摘要。
    logSummaryIfChanged(raw);

    QList<BatteryDevice> qtDevices;
    qtDevices.reserve(static_cast<int>(raw.size()));
    for (const auto &d : raw) {
        qtDevices.append(d);
    }
    emit devicesUpdated(qtDevices);
}

#include "BatteryManager.moc"

// 注册元类型，使 QList<BatteryDevice> 可跨 queued 连接传递。
// 必须在使用该类型的 queued 信号之前调用。
static const int kBatteryDeviceListMetaId = []() {
    qRegisterMetaType<QList<BatteryDevice>>("QList<BatteryDevice>");
    return 0;
}();

BatteryManager::BatteryManager(QObject *parent)
    : QObject(parent)
{
    m_staleRetentionMsec = quint64(qMax(0, AppSettings::staleRetentionSec())) * 1000;
    Q_UNUSED(kBatteryDeviceListMetaId);
}

BatteryManager::~BatteryManager()
{
    if (m_timer) {
        m_timer->stop();
    }
    for (auto &runtime : m_providerRuntimes) {
        if (runtime.thread) {
            runtime.thread->quit();
        }
    }
    for (auto &runtime : m_providerRuntimes) {
        if (runtime.thread) {
            runtime.thread->wait();
        }
    }
}

void BatteryManager::addProvider(std::unique_ptr<IBatteryProvider> provider)
{
    if (provider) {
        m_providers.push_back(std::move(provider));
    }
}

void BatteryManager::setInterval(int msec)
{
    if (!m_timer) {
        return;
    }
    m_timer->setInterval(msec > 0 ? msec : 10000);
}

void BatteryManager::setStaleRetention(int sec)
{
    // sec<=0 视为「从不缓存」；设备级“永久缓存”仍会保留。
    m_staleRetentionMsec = quint64(sec > 0 ? sec : 0) * 1000;
}

void BatteryManager::start()
{
    if (!m_providerRuntimes.empty()) {
        return;
    }

    m_providerRuntimes.reserve(m_providers.size());
    for (int i = 0; i < static_cast<int>(m_providers.size()); ++i) {
        auto thread = std::make_unique<QThread>();
        thread->setObjectName(QStringLiteral("BatteryProviderWorker-%1").arg(i));

        auto *worker = new ProviderWorker(i, m_providers[static_cast<size_t>(i)].get());
        worker->moveToThread(thread.get());

        connect(thread.get(), &QThread::finished, worker, &QObject::deleteLater);
        connect(worker, &ProviderWorker::refreshed,
                this, &BatteryManager::onProviderRefreshed);

        ProviderRuntime runtime;
        runtime.thread = std::move(thread);
        runtime.worker = worker;
        m_providerRuntimes.push_back(std::move(runtime));
    }

    m_timer = new QTimer(this);
    m_timer->setInterval(10000);
    connect(m_timer, &QTimer::timeout, this, &BatteryManager::scheduleRefresh);

    for (auto &runtime : m_providerRuntimes) {
        if (runtime.thread) {
            runtime.thread->start();
        }
    }
    m_timer->start();

    // 启动后立即刷新一次。
    scheduleRefresh();
}

void BatteryManager::refreshNow()
{
    scheduleRefresh();
}

void BatteryManager::scheduleRefresh()
{
    for (int i = 0; i < static_cast<int>(m_providerRuntimes.size()); ++i) {
        requestProviderRefresh(i);
    }
}

void BatteryManager::requestProviderRefresh(int providerIndex)
{
    if (providerIndex < 0 ||
        providerIndex >= static_cast<int>(m_providerRuntimes.size())) {
        return;
    }

    auto &runtime = m_providerRuntimes[static_cast<size_t>(providerIndex)];
    if (!runtime.worker) {
        return;
    }
    if (runtime.inFlight) {
        runtime.pendingRefresh = true;
        return;
    }

    runtime.inFlight = true;
    const bool invoked = QMetaObject::invokeMethod(
        runtime.worker, "refresh", Qt::QueuedConnection);
    if (!invoked) {
        runtime.inFlight = false;
        LOG_WARN_W(L"[BatteryManager] failed to queue provider refresh #" +
                   std::to_wstring(providerIndex));
    }
}

void BatteryManager::onProviderRefreshed(int providerIndex,
                                         const QList<BatteryDevice> &devices)
{
    if (providerIndex < 0 ||
        providerIndex >= static_cast<int>(m_providerRuntimes.size())) {
        LOG_WARN_W(L"[BatteryManager] ignored provider result with invalid index " +
                   std::to_wstring(providerIndex));
        return;
    }

    auto &runtime = m_providerRuntimes[static_cast<size_t>(providerIndex)];
    runtime.inFlight = false;
    runtime.devices = devices;
    runtime.hasSnapshot = true;

    QSet<std::wstring> freshIds;
    freshIds.reserve(devices.size());
    for (const auto &device : devices) {
        freshIds.insert(device.id);
    }
    publishCurrentDevices(freshIds);

    if (runtime.pendingRefresh) {
        runtime.pendingRefresh = false;
        requestProviderRefresh(providerIndex);
    }
}

QString BatteryManager::typeLabel(BatteryDevice::Type type)
{
    switch (type) {
    case BatteryDevice::Type::Bluetooth:
        return QStringLiteral("Bluetooth");
    case BatteryDevice::Type::Xbox:
        return QStringLiteral("Xbox");
    case BatteryDevice::Type::Hid:
        return QStringLiteral("HID");
    }
    return QStringLiteral("Unknown");
}
