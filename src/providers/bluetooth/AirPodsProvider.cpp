#include "AirPodsProvider.h"
#include "util/Logger.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace winrt::Windows::Devices::Bluetooth::Advertisement;

namespace
{
// RSSI 阈值：低于此值（信号太弱）的广播丢弃。
constexpr short kRssiThreshold = -75;

// Apple Continuity 厂商数据 CompanyId = 0x004C = 76。
constexpr uint16_t kAppleCompanyId = 76;

// AirPods continuity payload 的类型标识。
constexpr uint8_t kAirPodsAdvType = 0x07;

// AirPods Max 系列型号 ID（这类设备的百分比算法与其它不同：+5 偏移）。
// 0x2002 = AirPods Max, 0x200F 之外的有线/无线差异。
bool isMaxModel(uint16_t modelId)
{
    return modelId == 0x2002 || modelId == 0x2017; // 0x2017 = 8223 (AirPods Max USB-C)
}

// nibble(0-15) -> 百分比：
//   15 -> -1（未知）；>=10 -> 100%；<0 -> -1；其余 nib*10（Max 型号 +5）。
int nibbleToPercent(int nib, uint16_t modelId)
{
    if (nib == 15) {
        return -1;
    }
    if (nib >= 10) {
        return 100;
    }
    if (nib < 0) {
        return -1;
    }
    const int base = nib * 10;
    const int v = isMaxModel(modelId) ? base + 5 : base;
    return (v > 100) ? 100 : ((v < 0) ? 0 : v);
}

// 精确百分比 -> 离散档位（与 BluetoothProvider 一致）。
BatteryLevel levelFromPercentage(int pct)
{
    if (pct < 0) {
        return BatteryLevel::Unknown;
    }
    if (pct >= 80) {
        return BatteryLevel::Full;
    }
    if (pct >= 50) {
        return BatteryLevel::Medium;
    }
    if (pct >= 20) {
        return BatteryLevel::Low;
    }
    return BatteryLevel::Empty;
}

// Apple 音频设备型号表：modelId -> 友好名。
// modelId 由 payload[3..4] 解码（data[3] 低字节、data[4] 高字节），值即下表的 key。
const std::map<uint16_t, std::wstring> &modelTable()
{
    static const std::map<uint16_t, std::wstring> kTable = {
        {0x2002, L"AirPods Max"},
        {0x2003, L"PowerBeats 3"},
        {0x200D, L"Beats Solo 3"},
        {0x200E, L"AirPods Pro"},
        {0x200F, L"AirPods 2"},
        {0x2013, L"AirPods Pro 2"},
        {0x2017, L"AirPods Max (USB-C)"},
        {0x2019, L"PowerBeats Pro"},
        {0x2011, L"AirPods 3"},
        {0x201F, L"AirPods Pro 3"},
    };
    return kTable;
}

// 查型号友好名，未命中返回 L"Apple Audio (ID:XXXX)"。
std::wstring modelDisplayName(uint16_t modelId)
{
    const auto &table = modelTable();
    const auto it = table.find(modelId);
    if (it != table.end()) {
        return it->second;
    }
    wchar_t buf[64] = {};
    swprintf(buf, 64, L"Apple Audio (ID:%04X)", modelId);
    return buf;
}

// 最近广播有效期。超过此时长未见广播的设备从快照中剔除。
constexpr auto kStaleTimeout = std::chrono::seconds(35);
} // namespace

AirPodsProvider::AirPodsProvider() = default;

AirPodsProvider::~AirPodsProvider()
{
    if (m_watcherStarted && m_watcher) {
        try {
            m_watcher.Received(m_receivedToken);
            m_watcher.Stop();
        } catch (...) {
        }
    }
}

std::wstring AirPodsProvider::displayName() const
{
    return L"AirPods";
}

void AirPodsProvider::ensureWatcherStarted()
{
    if (m_watcherStarted) {
        return;
    }
    try {
        m_watcher = BluetoothLEAdvertisementWatcher();
        m_watcher.ScanningMode(BluetoothLEScanningMode::Active);
        // 不设 ManufacturerData 过滤（构造空数据在某些 SDK 版本上会失败），
        // 改为接收所有广播，在回调里按 CompanyId 过滤。
        m_receivedToken = m_watcher.Received(
            {this, &AirPodsProvider::onAdvertisementReceived});
        m_watcher.Start();
        m_watcherStarted = true;
        LOG_W(L"[AirPods] watcher started (Apple CompanyId=0x004C, RSSI>=" +
              std::to_wstring(kRssiThreshold) + L"dBm)");
    } catch (const winrt::hresult_error &e) {
        LOG_ERR_W(L"[AirPods] watcher start FAILED: " + std::wstring(e.message()));
    } catch (...) {
        LOG_ERR("[AirPods] watcher start FAILED (unknown)");
    }
}

void AirPodsProvider::onAdvertisementReceived(
    const BluetoothLEAdvertisementWatcher & /*sender*/,
    const BluetoothLEAdvertisementReceivedEventArgs &args)
{
    try {
        const short rssi = args.RawSignalStrengthInDBm();
        if (rssi < kRssiThreshold) {
            return; // 信号太弱，丢弃。
        }
        // 遍历厂商数据，找 Apple 的。
        for (const auto &md : args.Advertisement().ManufacturerData()) {
            if (md.CompanyId() != kAppleCompanyId) {
                continue;
            }
            const auto buf = md.Data();
            if (!buf || buf.Length() < 25) {
                continue;
            }
            std::vector<uint8_t> data(buf.Length());
            auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(buf);
            reader.ReadBytes(winrt::array_view<uint8_t>(data));

            std::wstring name;
            int left = -1, right = -1, casePct = -1;
            bool charging = false;
            if (!parseApplePayload(data.data(), data.size(),
                                   name, left, right, casePct, charging)) {
                continue;
            }

            // 更新缓存。
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                AdvDevice &d = m_devices[args.BluetoothAddress()];
                d.name = name;
                d.leftPercent = left;
                d.rightPercent = right;
                d.casePercent = casePct;
                d.charging = charging;
                d.rssi = rssi;
                d.lastSeen = std::chrono::steady_clock::now();
            }
            LOG_W(L"[AirPods] RX " + name + L" L=" + std::to_wstring(left) +
                  L"% R=" + std::to_wstring(right) + L"% Case=" +
                  std::to_wstring(casePct) + L"% rssi=" + std::to_wstring(rssi) +
                  L"dBm");
            break; // 每个广播包只处理第一个 Apple 厂商数据。
        }
    } catch (const winrt::hresult_error &e) {
        LOG_ERR_W(L"[AirPods] received handler error: " + std::wstring(e.message()));
    } catch (...) {
    }
}

bool AirPodsProvider::parseApplePayload(const uint8_t *data, std::size_t len,
                                        std::wstring &modelName, int &left,
                                        int &right, int &casePct, bool &charging)
{
    if (len < 25) {
        return false;
    }
    if (data[0] != kAirPodsAdvType) {
        return false; // 非 AirPods continuity 类型。
    }
    const uint16_t modelId = static_cast<uint16_t>((data[4] << 8) | data[3]);
    modelName = modelDisplayName(modelId);

    // 电量在字节 6、7 的 4-bit nibble：
    //   leftNibble  = (data[6] & 0xF0) >> 4
    //   rightNibble =  data[6] & 0x0F
    //   caseNibble  =  data[7] & 0x0F
    //   statusNib   = (data[7] & 0xF0) >> 4
    const int leftNibble = (data[6] & 0xF0) >> 4;
    const int rightNibble = data[6] & 0x0F;
    const int caseNibble = data[7] & 0x0F;
    const int statusNib = (data[7] & 0xF0) >> 4;

    // data[10] bit1 表示左右耳帧序翻转（耳机的 L/R 顺序反转）。
    const bool flip = (data[10] & 0x02) != 0;
    const int a = flip ? rightNibble : leftNibble; // 左耳实际值
    const int b = flip ? leftNibble : rightNibble; // 右耳实际值

    // 充电状态（statusNib 的位）：
    //   bit2(4) -> 充电盒充电；flip?bit1:bit0 -> 左耳充电；flip?bit0:bit1 -> 右耳充电。
    const bool caseCharging = (statusNib & 4) != 0;
    const bool leftCharging = (flip ? (statusNib & 2) : (statusNib & 1)) != 0;
    const bool rightCharging = (flip ? (statusNib & 1) : (statusNib & 2)) != 0;
    charging = leftCharging || rightCharging || caseCharging;

    left = nibbleToPercent(a, modelId);
    right = nibbleToPercent(b, modelId);
    casePct = nibbleToPercent(caseNibble, modelId);
    return true;
}

std::vector<BatteryDevice> AirPodsProvider::readDevices()
{
    // 惰性初始化 WinRT apartment（在 worker 线程）。
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    ensureWatcherStarted();

    std::vector<BatteryDevice> devices;
    const auto now = std::chrono::steady_clock::now();

    // 清理过期 + 构建快照。
    std::vector<std::pair<uint64_t, AdvDevice>> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_devices.begin(); it != m_devices.end();) {
            if (now - it->second.lastSeen > kStaleTimeout) {
                LOG_W(L"[AirPods] drop stale device: " + it->second.name);
                it = m_devices.erase(it);
            } else {
                snapshot.emplace_back(it->first, it->second);
                ++it;
            }
        }
    }

    for (const auto &[addr, adv] : snapshot) {
        BatteryDevice device;
        device.id = L"airpods:" + std::to_wstring(addr);
        device.name = adv.name;
        device.type = BatteryDevice::Type::Bluetooth;
        device.subType = BatteryDevice::SubType::AirPods;
        device.leftPercent = adv.leftPercent;
        device.rightPercent = adv.rightPercent;
        device.casePercent = adv.casePercent;
        device.charging = adv.charging;
        // 百分比取三路有效值的最低，用于排序 / 低电量提醒 / 整体进度条。
        int minPct = -1;
        for (int v : {adv.leftPercent, adv.rightPercent, adv.casePercent}) {
            if (v >= 0 && (minPct < 0 || v < minPct)) {
                minPct = v;
            }
        }
        device.percentage = minPct;
        device.level = levelFromPercentage(minPct);
        device.connected = true; // 能收到广播即视为在线。
        devices.push_back(std::move(device));
    }

    LOG_W(L"[AirPods] readDevices total = " + std::to_wstring(devices.size()));
    return devices;
}
