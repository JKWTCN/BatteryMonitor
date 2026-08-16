#include "SharedBleWatcher.h"

#include "util/Logger.h"

using namespace winrt::Windows::Devices::Bluetooth::Advertisement;

SharedBleWatcher &SharedBleWatcher::instance()
{
    static SharedBleWatcher s_instance;
    return s_instance;
}

SharedBleWatcher::~SharedBleWatcher()
{
    if (m_started)
    {
        try
        {
            m_watcher.Received(m_receivedToken);
            m_watcher.Stop();
        }
        catch (...)
        {
        }
    }
}

std::uint64_t SharedBleWatcher::addCallback(Callback callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    startLocked();
    const std::uint64_t id = m_nextId++;
    m_callbacks.emplace_back(id, std::move(callback));
    return id;
}

void SharedBleWatcher::ensureStarted()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    startLocked();
}

void SharedBleWatcher::startLocked()
{
    if (m_started)
    {
        return;
    }
    try
    {
        m_watcher = BluetoothLEAdvertisementWatcher();
        m_watcher.ScanningMode(BluetoothLEScanningMode::Active);
        // 不设 ManufacturerData 过滤（部分 SDK 版本上构造空过滤器会失败），
        // 接收所有广播，由各订阅方在回调里自行按 CompanyId / UUID 过滤。
        m_receivedToken = m_watcher.Received({this, &SharedBleWatcher::onReceived});
        m_watcher.Start();
        m_started = true;
        LOG_W(L"[SharedBleWatcher] watcher started (Active scan, all advertisements)");
    }
    catch (const winrt::hresult_error &e)
    {
        m_watcher = nullptr;
        LOG_ERR_W(L"[SharedBleWatcher] watcher start FAILED: " +
                  std::wstring(e.message()));
    }
    catch (...)
    {
        m_watcher = nullptr;
        LOG_ERR("[SharedBleWatcher] watcher start FAILED (unknown)");
    }
}

void SharedBleWatcher::removeCallback(std::uint64_t id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_callbacks.begin(); it != m_callbacks.end(); ++it)
    {
        if (it->first == id)
        {
            m_callbacks.erase(it);
            break;
        }
    }
    if (m_callbacks.empty() && m_started)
    {
        try
        {
            m_watcher.Received(m_receivedToken);
            m_watcher.Stop();
        }
        catch (...)
        {
        }
        m_watcher = nullptr;
        m_started = false;
    }
}

void SharedBleWatcher::onReceived(
    const BluetoothLEAdvertisementWatcher & /*sender*/, const Args &args)
{
    // 拷贝一份回调表再逐个调用：回调里会拿 provider 自己的锁，不能在持有
    // m_mutex 时进入订阅者代码（避免与 add/removeCallback 相互等待）。
    std::vector<Callback> targets;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_callbacks.empty())
        {
            return;
        }
        targets.reserve(m_callbacks.size());
        for (const auto &entry : m_callbacks)
        {
            targets.push_back(entry.second);
        }
    }
    for (const auto &target : targets)
    {
        try
        {
            target(args);
        }
        catch (...)
        {
            // 单个订阅者异常不影响其余订阅者；订阅者自身负责记录错误。
        }
    }
}
