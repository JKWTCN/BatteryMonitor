#include "BatteryManager.h"
#include "util/Logger.h"

#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>

#include <utility>

namespace
{
// worker 线程实际对象：在自身线程内初始化并执行刷新。
// 之所以不直接用 QThread::run()，是因为我们需要事件循环来接收
// 主线程投递过来的 queued 调用（refresh()）。
//
// worker 只持有纯 C++ 的 provider 列表；读到 std::vector<BatteryDevice> 后，
// 经 toQt() 把 std::wstring 转 QString（Qt 边界转换），再 emit 给 UI。
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
    void init() { }

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

        LOG("[BatteryManager] refresh done, raw devices = " + std::to_string(raw.size()));

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
    std::vector<IBatteryProvider *> m_providers;
};
} // namespace

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

    // worker 线程启动时先做线程局部初始化。
    connect(&m_workerThread, &QThread::started, worker, &RefreshWorker::init);

    // 主线程 -> worker：用信号驱动刷新。跨线程时 connect 自动为 queued，
    // 比 invokeMethod(worker,"refresh") 字符串形式可靠得多（不受 moc 可见性影响）。
    connect(this, &BatteryManager::requestRefresh, worker, &RefreshWorker::refresh);

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
    }
    return QStringLiteral("Unknown");
}
