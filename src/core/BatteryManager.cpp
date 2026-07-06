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

// worker 线程实际对象：在自身线程内初始化并执行刷新。
// 之所以不直接用 QThread::run()，是因为我们需要事件循环来接收
// 主线程投递过来的 queued 调用（refresh()）。
//
// worker 只持有纯 C++ 的 provider 列表；读到 std::vector<BatteryDevice> 后，
// 经 toQt() 把 std::wstring 转 QString（Qt 边界转换），再 emit 给 UI。
//
// 粘性缓存（stale-while-revalidate）：HID / 蓝牙设备单轮通信失败是常态
// （接收器被系统驱动独占、无线短暂丢包、设备休眠等），但设备并未真正离线。
// worker 维护一份 m_cache（id -> 上次成功读数），本轮没读到的设备只要还在
// 保留窗口内就沿用旧值、标 stale 后继续展示，避免设备在列表里反复横跳。
class RefreshWorker : public QObject
{
    Q_OBJECT

public:
    explicit RefreshWorker(std::vector<IBatteryProvider *> providers)
        : m_providers(std::move(providers))
    {
    }

public slots:
    // 在 worker 线程首次启动时做线程局部初始化（例如 WinRT apartment）。
    // 当前 BluetoothProvider 在自身 readDevices() 里惰性处理 apartment，这里留作扩展点。
    // 同时从持久化设置读入粘性缓存保留窗口——在 worker 线程内读取，
    // 与后续 refresh() 同线程，无需加锁。
    void init()
    {
        m_staleRetentionMsec = quint64(qMax(0, AppSettings::staleRetentionSec())) * 1000;
    }

    // 主线程通过 queued 连接设置粘性缓存保留窗口（秒）。下一轮 refresh 即生效。
    void setStaleRetentionSec(int sec) { m_staleRetentionMsec = quint64(sec) * 1000; }

    // 遍历所有 provider 聚合设备，把结果回送到主线程。
    void refresh()
    {
        std::vector<BatteryDevice> raw;
        for (IBatteryProvider *provider : m_providers) {
            if (!provider) {
                continue;
            }
            try {
                auto partial = provider->readDevices();
                for (auto &device : partial) {
                    raw.push_back(std::move(device));
                }
            } catch (const std::exception &e) {
                LOG_ERR(std::string("Provider '") +
                        std::string(provider->displayName().begin(), provider->displayName().end()) +
                        "' threw: " + e.what());
            } catch (...) {
                LOG_ERR("[BatteryManager] provider threw unknown exception");
            }
        }

        filterUnpairedAirPods(raw);

        // 与粘性缓存合并：本轮读到的设备刷新时间戳并入缓存；本轮没读到的
        // 设备若还在保留窗口内，以 stale=true 沿用旧值补回。详见 applyStickyCache。
        applyStickyCache(raw);

        // 变化检测：只有设备集合 / 电量 / 连接状态发生变化时才写一条 INFO 摘要。
        // 平时（设备读数稳定）完全静默，避免每 10s 刷屏撑爆日志文件。
        // 排障需要看每轮明细？把 Logger 等级调到 Verbose 即可（各 provider 的逐设备
        // 读数日志都是 LOG_VERBOSE）。
        logSummaryIfChanged(raw);

        // Qt 边界转换：std::wstring -> QString。此处是 battery 层与 Qt 的唯一接触点。
        QList<BatteryDevice> qtDevices;
        qtDevices.reserve(static_cast<int>(raw.size()));
        for (const auto &d : raw) {
            qtDevices.append(d); // BatteryDevice 含 std::wstring，直接拷贝进 QList。
        }
        emit refreshed(qtDevices);
    }

signals:
    void refreshed(const QList<BatteryDevice> &devices);

private:
    // 设备签名：把影响“是否需要再记一条日志”的字段拼成一个可比较的字符串。
    // 刻意不包含 name（重命名不影响电量状态）、不包含 wired/charging 之外的瞬态量。
    // 形如 "id|type|subType|pct|level|L|R|case|charging|wired|connected|stale"。
    static std::wstring deviceSignature(const BatteryDevice &d);

    // 仅当本轮设备列表相对上一轮发生变化时，写一条 INFO 摘要；否则静默。
    // 首次调用（空快照）总会写一条，便于启动后在日志里看到初始设备状态。
    void logSummaryIfChanged(const std::vector<BatteryDevice> &devices);

    // 粘性缓存合并：见上方「粘性缓存」说明。
    //   - 本轮 raw 里的设备：刷新 lastSeenMsecs、清 stale、写入 / 覆盖 m_cache；
    //   - 本轮 raw 里没有、但 m_cache 里有且 lastSeenMsecs 在保留窗口内的设备：
    //     以 stale=true 沿用旧值补回到 raw 末尾；
    //   - 超出保留窗口的缓存条目：从 m_cache 移除（设备真正离开）。
    // m_staleRetentionMsec==0 时跳过整个合并（等价于「从不缓存」旧行为）。
    void applyStickyCache(std::vector<BatteryDevice> &raw);

    // AirPods 展示过滤必须在所有 provider 聚合后做：
    // 普通蓝牙 provider 可能先给出一个已连接但电量未知的 AirPods 记录，
    // AirPodsProvider 再给出三路电量广播记录；后者需要借前者兜底证明“本机已配对”。
    void filterUnpairedAirPods(std::vector<BatteryDevice> &raw);

    std::vector<IBatteryProvider *> m_providers;

    // 上一轮的设备签名集合（已排序），用于变化检测。
    std::vector<std::wstring> m_lastSignatures;
    // 是否已经至少成功记录过一次（首轮无条件打印初始状态）。
    bool m_hasSnapshot = false;

    // 粘性缓存：id -> 上次成功读数。worker 线程独占，无需加锁。
    QHash<std::wstring, BatteryDevice> m_cache;
    // 保留窗口（毫秒）。0 = 从不缓存。
    quint64 m_staleRetentionMsec = 180 * 1000;
};
} // namespace

// —— RefreshWorker 私有辅助实现 ——

std::wstring RefreshWorker::deviceSignature(const BatteryDevice &d)
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

void RefreshWorker::logSummaryIfChanged(const std::vector<BatteryDevice> &devices)
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

void RefreshWorker::applyStickyCache(std::vector<BatteryDevice> &raw)
{
    // 0 = 从不缓存：清空缓存，行为完全等价于改造前的「瞬时失败即移除」。
    if (m_staleRetentionMsec == 0) {
        if (!m_cache.isEmpty()) {
            m_cache.clear();
        }
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // 1) 把本轮读到的设备刷入缓存，并更新时间戳。
    QSet<std::wstring> seenIds;
    seenIds.reserve(static_cast<int>(raw.size()));
    for (auto &d : raw) {
        d.lastSeenMsecs = now;
        d.stale = false;
        m_cache.insert(d.id, d);
        seenIds.insert(d.id);
    }

    // 2) 缓存里本轮未出现的设备：保留窗口内沿用旧值并标 stale，超时则移除。
    //    特例：用户对该设备勾选了“永久缓存”时，无视超时窗口，永久以 stale 值保留。
    //    （注：即便全局粘性缓存被关闭 m_staleRetentionMsec==0，本函数已在最上方早退，
    //     因此“永久缓存”只在全局开启缓存的前提下生效，避免与用户“从不缓存”的意图冲突。）
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
        if (age <= m_staleRetentionMsec || keepForever) {
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

void RefreshWorker::filterUnpairedAirPods(std::vector<BatteryDevice> &raw)
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
    // worker 运行在独立线程，拥有自己的事件循环。
    m_workerThread.setObjectName(QStringLiteral("BatteryRefreshWorker"));
    Q_UNUSED(kBatteryDeviceListMetaId);
}

BatteryManager::~BatteryManager()
{
    if (m_timer) {
        m_timer->stop();
    }
    m_workerThread.quit();
    m_workerThread.wait();
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
    // 通过 queued 信号投递到 worker 线程（与 refresh() 同线程，避免与 m_cache 竞争）。
    // sec<=0 视为「从不缓存」，worker 端把 m_staleRetentionMsec 置 0。
    emit requestStaleRetention(sec > 0 ? sec : 0);
}

void BatteryManager::start()
{
    // 把裸指针列表交给 worker（worker 不持有所有权，所有权仍在 Manager）。
    std::vector<IBatteryProvider *> providerPtrs;
    providerPtrs.reserve(m_providers.size());
    for (const auto &provider : m_providers) {
        providerPtrs.push_back(provider.get());
    }

    // 用具名局部指针：RefreshWorker 定义在本文件匿名命名空间里，
    // 用具体类型 connect() 才能正确解析成员函数指针重载。
    auto *worker = new RefreshWorker(std::move(providerPtrs));
    worker->moveToThread(&m_workerThread);

    // worker 线程启动时先做线程局部初始化（含从设置读入粘性缓存窗口）。
    connect(&m_workerThread, &QThread::started, worker, &RefreshWorker::init);

    // 主线程 -> worker：用信号驱动刷新。跨线程时 connect 自动为 queued，
    // 比 invokeMethod(worker,"refresh") 字符串形式可靠得多（不受 moc 可见性影响）。
    connect(this, &BatteryManager::requestRefresh, worker, &RefreshWorker::refresh);

    // 主线程 -> worker：运行时调整粘性缓存窗口（queued）。
    connect(this, &BatteryManager::requestStaleRetention, worker, &RefreshWorker::setStaleRetentionSec);

    // worker -> 主线程：刷新完成回送结果（AutoConnection -> 跨线程为 queued）。
    connect(worker, &RefreshWorker::refreshed, this, [this](const QList<BatteryDevice> &devices) {
        emit devicesUpdated(devices);
    });

    // worker 线程结束时清理 worker 对象。
    connect(&m_workerThread, &QThread::finished, worker, &QObject::deleteLater);

    m_timer = new QTimer(this);
    m_timer->setInterval(10000);
    connect(m_timer, &QTimer::timeout, this, &BatteryManager::scheduleRefresh);

    m_workerThread.start();
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
    // 通过信号把工作投递到 worker 线程（queued 连接），避免阻塞 UI 线程。
    emit requestRefresh();
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
