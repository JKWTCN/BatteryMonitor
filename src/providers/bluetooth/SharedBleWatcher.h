#pragma once

// TypedEventHandler 的「对象+成员函数指针」构造器定义在完整的命名空间头里
// （Advertisement.h 只传递引入 impl/*.2.h 的声明，单独用会 LNK2019）。
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

// 进程内共享的 BLE 广播监听器。
//
// 广播类 provider（AirPods / 小米耳机）都需要持续旁听环境中所有 BLE 广播，
// 再各自按 CompanyId / ServiceData UUID 过滤。若每个 provider 各开一个
// BluetoothLEAdvertisementWatcher，同一个广播包会被系统分发、WinRT 投影多次，
// 广播密集的环境（办公室 / 通勤）下 CPU 随包率成倍增长。共用一个 watcher 后
// 每个包只走一次事件分发，再 fan-out 给各订阅回调。
//
// 事件在 WinRT 线程池线程触发，订阅方需自行保证回调的线程安全。
class SharedBleWatcher
{
public:
    using Args = winrt::Windows::Devices::Bluetooth::Advertisement::
        BluetoothLEAdvertisementReceivedEventArgs;
    using Callback = std::function<void(const Args &)>;

    static SharedBleWatcher &instance();

    // 注册广播回调，返回非零 id 供 removeCallback 使用。
    // 首次注册时启动 watcher（调用线程需已 init_apartment）。
    std::uint64_t addCallback(Callback callback);
    // 注销回调；最后一个回调注销后停止 watcher，射频不再扫描。
    void removeCallback(std::uint64_t id);
    // 幂等启动。启动失败（如蓝牙射频尚未打开）时保持未启动状态，
    // 订阅方可在后续轮询里反复调用以重试。
    void ensureStarted();

private:
    SharedBleWatcher() = default;
    ~SharedBleWatcher();

    SharedBleWatcher(const SharedBleWatcher &) = delete;
    SharedBleWatcher &operator=(const SharedBleWatcher &) = delete;

    // 持有 m_mutex 时调用的启动实现。
    void startLocked();

    void onReceived(
        const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher &sender,
        const Args &args);

    std::mutex m_mutex;
    // id -> 回调。仅在持有 m_mutex 时访问。
    std::vector<std::pair<std::uint64_t, Callback>> m_callbacks;
    winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher
        m_watcher{nullptr};
    winrt::event_token m_receivedToken{};
    std::uint64_t m_nextId = 1;
    bool m_started = false;
};
