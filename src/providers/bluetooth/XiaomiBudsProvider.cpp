#include "XiaomiBudsProvider.h"
#include "util/Logger.h"

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace WDB = winrt::Windows::Devices::Bluetooth;
namespace WDE = winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;

namespace
{
    // RSSI 阈值：低于此值（信号太弱）的广播丢弃。与 AirPodsProvider 一致。
    constexpr short kRssiThreshold = -75;

    // 小米厂商数据 CompanyId = 0x038F = 911。
    constexpr uint16_t kXiaomiCompanyId = 0x038F;

    // 小米 MAF 服务数据 UUID = 0xFD2D（广播中小端序为 2D FD）。
    constexpr uint8_t kMafServiceDataUuid[2] = {0x2D, 0xFD};

    // 厂商数据帧 / 服务数据帧的版本字节。
    constexpr uint8_t kAdvVersion = 0x01;

    // 最近广播有效期。超过此时长未见广播的设备从快照中剔除。
    constexpr auto kStaleTimeout = std::chrono::seconds(35);

    // 电量字节解码：0xFF -> 未知(-1)；低 7 位百分比；bit7 = 该路充电中。
    void decodeBatteryByte(uint8_t v, int &percent, bool &charging)
    {
        if (v == 0xFF)
        {
            percent = -1;
            charging = false;
            return;
        }
        percent = v & 0x7F;
        charging = (v & 0x80) != 0;
    }

    // 把 6 字节地址按“先按存储序、再按字节反序”两种解释转成 uint64，
    // 用于和 Windows 蓝牙地址匹配（不同广播对 MAC 字节序的约定不一致）。
    std::vector<uint64_t> addressCandidates(const uint8_t mac[6])
    {
        std::vector<uint64_t> out;
        out.reserve(2);
        uint64_t fwd = 0, rev = 0;
        for (int i = 0; i < 6; ++i)
        {
            fwd = (fwd << 8) | mac[i];
            rev = (rev << 8) | mac[5 - i];
        }
        out.push_back(fwd);
        out.push_back(rev);
        return out;
    }

    // 精确百分比 -> 离散档位（与 BluetoothProvider / AirPodsProvider 一致）。
    BatteryLevel levelFromPercentage(int pct)
    {
        if (pct < 0)
        {
            return BatteryLevel::Unknown;
        }
        if (pct >= 80)
        {
            return BatteryLevel::Full;
        }
        if (pct >= 50)
        {
            return BatteryLevel::Medium;
        }
        if (pct >= 20)
        {
            return BatteryLevel::Low;
        }
        return BatteryLevel::Empty;
    }

    // 已配对蓝牙地址（LE + 经典）快照；附带地址 -> 设备名，
    // 便于用 Windows 里的正式名称展示小米耳机。
    struct PairedSnapshot
    {
        bool available = false;
        std::set<uint64_t> addresses;
        std::map<uint64_t, std::wstring> names;

        bool contains(uint64_t addr) const
        {
            return addresses.find(addr) != addresses.end();
        }
    };

    void collectPairedLeAddresses(PairedSnapshot &snapshot)
    {
        const auto found = WDE::DeviceInformation::FindAllAsync(
            WDB::BluetoothLEDevice::GetDeviceSelectorFromPairingState(true)).get();
        snapshot.available = true;
        for (const auto &info : found)
        {
            try
            {
                const auto device = WDB::BluetoothLEDevice::FromIdAsync(info.Id()).get();
                if (device)
                {
                    const uint64_t addr = device.BluetoothAddress();
                    snapshot.addresses.insert(addr);
                    snapshot.names.emplace(addr, info.Name().c_str());
                }
            }
            catch (...)
            {
            }
        }
    }

    void collectPairedClassicAddresses(PairedSnapshot &snapshot)
    {
        const auto found = WDE::DeviceInformation::FindAllAsync(
            WDB::BluetoothDevice::GetDeviceSelectorFromPairingState(true)).get();
        snapshot.available = true;
        for (const auto &info : found)
        {
            try
            {
                const auto device = WDB::BluetoothDevice::FromIdAsync(info.Id()).get();
                if (device)
                {
                    const uint64_t addr = device.BluetoothAddress();
                    snapshot.addresses.insert(addr);
                    snapshot.names.emplace(addr, info.Name().c_str());
                }
            }
            catch (...)
            {
            }
        }
    }

    PairedSnapshot collectPairedBluetoothAddresses()
    {
        PairedSnapshot snapshot;
        try
        {
            collectPairedLeAddresses(snapshot);
        }
        catch (const winrt::hresult_error &e)
        {
            LOG_VERBOSE_W(L"[XiaomiBuds] paired LE enum failed: " + std::wstring(e.message()));
        }
        catch (...)
        {
            LOG_VERBOSE("[XiaomiBuds] paired LE enum failed");
        }
        try
        {
            collectPairedClassicAddresses(snapshot);
        }
        catch (const winrt::hresult_error &e)
        {
            LOG_VERBOSE_W(L"[XiaomiBuds] paired classic enum failed: " + std::wstring(e.message()));
        }
        catch (...)
        {
            LOG_VERBOSE("[XiaomiBuds] paired classic enum failed");
        }
        return snapshot;
    }
} // namespace

XiaomiBudsProvider::XiaomiBudsProvider() = default;

XiaomiBudsProvider::~XiaomiBudsProvider()
{
    if (m_watcherStarted && m_watcher)
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

std::wstring XiaomiBudsProvider::displayName() const
{
    return L"XiaomiBuds";
}

void XiaomiBudsProvider::ensureWatcherStarted()
{
    if (m_watcherStarted)
    {
        return;
    }
    try
    {
        m_watcher = BluetoothLEAdvertisementWatcher();
        m_watcher.ScanningMode(BluetoothLEScanningMode::Active);
        // 与 AirPodsProvider 相同：不构造 WinRT 过滤器（部分 SDK 版本上构造
        // 空数据会失败），在回调里按 CompanyId / ServiceData UUID 过滤。
        m_receivedToken = m_watcher.Received(
            {this, &XiaomiBudsProvider::onAdvertisementReceived});
        m_watcher.Start();
        m_watcherStarted = true;
        LOG_W(L"[XiaomiBuds] watcher started (CompanyId=0x038F / UUID=0xFD2D, RSSI>=" +
              std::to_wstring(kRssiThreshold) + L"dBm)");
    }
    catch (const winrt::hresult_error &e)
    {
        LOG_ERR_W(L"[XiaomiBuds] watcher start FAILED: " + std::wstring(e.message()));
    }
    catch (...)
    {
        LOG_ERR("[XiaomiBuds] watcher start FAILED (unknown)");
    }
}

void XiaomiBudsProvider::onAdvertisementReceived(
    const BluetoothLEAdvertisementWatcher & /*sender*/,
    const BluetoothLEAdvertisementReceivedEventArgs &args)
{
    try
    {
        const short rssi = args.RawSignalStrengthInDBm();
        if (rssi < kRssiThreshold)
        {
            return; // 信号太弱，丢弃。
        }
        const uint64_t advAddr = args.BluetoothAddress();
        const auto adv = args.Advertisement();

        // —— 1) 厂商数据帧（CompanyId=0x038F）——
        //   p[0]=len-2, p[1]=0x01, p[2]=产品型号, p[5]/[6]/[7]=左/右/盒电量。
        for (const auto &md : adv.ManufacturerData())
        {
            if (md.CompanyId() != kXiaomiCompanyId)
            {
                continue;
            }
            const auto buf = md.Data();
            if (!buf || buf.Length() < 0x18)
            {
                continue;
            }
            std::vector<uint8_t> p(buf.Length());
            auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(buf);
            reader.ReadBytes(winrt::array_view<uint8_t>(p));
            if (p[0] != p.size() - 2 || p[1] != kAdvVersion)
            {
                continue;
            }

            int left = -1, right = -1, casePct = -1;
            bool leftCharging = false, rightCharging = false, caseCharging = false;
            decodeBatteryByte(p[5], left, leftCharging);     // 左耳
            decodeBatteryByte(p[6], right, rightCharging);   // 右耳
            decodeBatteryByte(p[7], casePct, caseCharging);  // 充电盒
            const bool charging = leftCharging || rightCharging || caseCharging;

            // 帧内两个 48bit 地址（字节顺序按帧内位置重排还原）：
            //   addrA <- p[11..16]（存储序 14,15,16,13,11,12）
            //   addrB <- p[18..23]（存储序 21,22,23,20,18,19）
            uint8_t addrA[6] = {};
            uint8_t addrB[6] = {};
            if (p.size() >= 0x18)
            {
                const uint8_t orderA[6] = {14, 15, 16, 13, 11, 12};
                const uint8_t orderB[6] = {21, 22, 23, 20, 18, 19};
                for (int i = 0; i < 6; ++i)
                {
                    addrA[i] = p[orderA[i]];
                    addrB[i] = p[orderB[i]];
                }
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                AdvDevice &d = m_devices[advAddr];
                d.leftPercent = left;
                d.rightPercent = right;
                d.casePercent = casePct;
                d.charging = charging;
                d.leftCharging = leftCharging;
                d.rightCharging = rightCharging;
                d.caseCharging = caseCharging;
                d.podType = p[2];
                std::memcpy(d.embeddedAddrA, addrA, 6);
                std::memcpy(d.embeddedAddrB, addrB, 6);
                d.hasEmbeddedAddr = true;
                d.rssi = rssi;
                d.lastSeen = std::chrono::steady_clock::now();
            }
            LOG_VERBOSE_W(L"[XiaomiBuds] RX mfr L=" + std::to_wstring(left) +
                          (leftCharging ? L"%⚡" : L"%") + L" R=" +
                          std::to_wstring(right) + (rightCharging ? L"%⚡" : L"%") +
                          L" Case=" + std::to_wstring(casePct) +
                          (caseCharging ? L"%⚡" : L"%") + L" pod=" +
                          std::to_wstring(static_cast<int>(p[2])) + L" rssi=" +
                          std::to_wstring(rssi) + L"dBm");
            break; // 每个广播包只处理第一个小米厂商数据。
        }

        // —— 2) 服务数据帧（UUID=0xFD2D MAF）——
        //   去掉 2 字节 UUID 后：p[0]=0x01, p[1]=产品型号,
        //   p[12]/[13]/[14]=右/左/盒电量（左右与厂商数据帧相反）。
        for (const auto &section : adv.DataSections())
        {
            if (section.DataType() != 0x16) // 只看 Service Data - 16bit UUID。
            {
                continue;
            }
            const auto buf = section.Data();
            if (!buf || buf.Length() < 2 + 0x0F)
            {
                continue;
            }
            std::vector<uint8_t> raw(buf.Length());
            auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(buf);
            reader.ReadBytes(winrt::array_view<uint8_t>(raw));
            if (raw[0] != kMafServiceDataUuid[0] || raw[1] != kMafServiceDataUuid[1])
            {
                continue;
            }
            const uint8_t *p = raw.data() + 2;
            const std::size_t len = raw.size() - 2;
            if (len < 0x0F || p[0] != kAdvVersion)
            {
                continue;
            }

            int left = -1, right = -1, casePct = -1;
            bool leftCharging = false, rightCharging = false, caseCharging = false;
            decodeBatteryByte(p[12], right, rightCharging);   // 右耳
            decodeBatteryByte(p[13], left, leftCharging);     // 左耳
            decodeBatteryByte(p[14], casePct, caseCharging);  // 充电盒
            const bool charging = leftCharging || rightCharging || caseCharging;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                AdvDevice &d = m_devices[advAddr];
                d.leftPercent = left;
                d.rightPercent = right;
                d.casePercent = casePct;
                d.charging = charging;
                d.leftCharging = leftCharging;
                d.rightCharging = rightCharging;
                d.caseCharging = caseCharging;
                d.podType = p[1];
                d.rssi = rssi;
                d.lastSeen = std::chrono::steady_clock::now();
            }
            LOG_VERBOSE_W(L"[XiaomiBuds] RX maf L=" + std::to_wstring(left) +
                          (leftCharging ? L"%⚡" : L"%") + L" R=" +
                          std::to_wstring(right) + (rightCharging ? L"%⚡" : L"%") +
                          L" Case=" + std::to_wstring(casePct) +
                          (caseCharging ? L"%⚡" : L"%") + L" rssi=" +
                          std::to_wstring(rssi) + L"dBm");
            break;
        }
    }
    catch (const winrt::hresult_error &e)
    {
        LOG_ERR_W(L"[XiaomiBuds] received handler error: " + std::wstring(e.message()));
    }
    catch (...)
    {
    }
}

std::vector<BatteryDevice> XiaomiBudsProvider::readDevices()
{
    // 惰性初始化 WinRT apartment（在 worker 线程）。
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (...)
    {
    }
    ensureWatcherStarted();

    std::vector<BatteryDevice> devices;
    const auto now = std::chrono::steady_clock::now();
    const PairedSnapshot paired = collectPairedBluetoothAddresses();

    // 清理过期 + 构建快照。
    std::vector<std::pair<uint64_t, AdvDevice>> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_devices.begin(); it != m_devices.end();)
        {
            if (now - it->second.lastSeen > kStaleTimeout)
            {
                it = m_devices.erase(it);
            }
            else
            {
                snapshot.emplace_back(it->first, it->second);
                ++it;
            }
        }
    }

    // —— 稳定设备 id ——
    // BLE 地址最高 2 位标识类型：00=Public、11=Static Random 是固定的；
    // 01=RPA、10=Non-resolvable 会周期轮换（约 15 分钟）。轮换类地址改用
    // 厂商数据帧内嵌的 48bit 身份地址做 id（内嵌地址不随广播地址轮换），
    // 避免历史 / 别名 / 粘性缓存被地址轮换打散；没有内嵌地址时退回广播地址。
    const auto addressRotates = [](uint64_t addr) {
        const uint8_t msb = static_cast<uint8_t>((addr >> 40) & 0xFF);
        const uint8_t top = msb >> 6;
        return top == 0x1 || top == 0x2;
    };
    const auto canonicalAddress = [&addressRotates](uint64_t advAddr, const AdvDevice &adv) -> uint64_t {
        if (!addressRotates(advAddr) || !adv.hasEmbeddedAddr) {
            return advAddr;
        }
        const auto nonzero = [](const uint8_t *mac) {
            for (int i = 0; i < 6; ++i) {
                if (mac[i] != 0) {
                    return true;
                }
            }
            return false;
        };
        for (const uint8_t *mac : {adv.embeddedAddrA, adv.embeddedAddrB}) {
            if (!nonzero(mac)) {
                continue;
            }
            uint64_t cand = 0;
            for (int i = 0; i < 6; ++i) {
                cand = (cand << 8) | mac[i];
            }
            if (!addressRotates(cand)) {
                return cand;
            }
        }
        return advAddr;
    };

    for (const auto &[addr, adv] : snapshot)
    {
        // 配对匹配：广播地址本身，或帧内携带的两个 48bit 地址（含字节反序），
        // 任意一个命中系统配对列表即视为已配对。命中的同时取配对名展示。
        bool pairedFlag = paired.available && paired.contains(addr);
        std::wstring name;
        if (pairedFlag)
        {
            const auto it = paired.names.find(addr);
            if (it != paired.names.end() && !it->second.empty())
            {
                name = it->second;
            }
        }
        if (!pairedFlag && adv.hasEmbeddedAddr && paired.available)
        {
            for (const uint8_t *mac : {adv.embeddedAddrA, adv.embeddedAddrB})
            {
                for (uint64_t cand : addressCandidates(mac))
                {
                    if (paired.contains(cand))
                    {
                        pairedFlag = true;
                        const auto it = paired.names.find(cand);
                        if (it != paired.names.end() && !it->second.empty())
                        {
                            name = it->second;
                        }
                        break;
                    }
                }
                if (pairedFlag)
                {
                    break;
                }
            }
        }
        if (name.empty())
        {
            wchar_t buf[64] = {};
            swprintf(buf, 64, L"Xiaomi Buds (0x%02X)", adv.podType);
            name = buf;
        }

        BatteryDevice device;
        device.id = L"xiaomibuds:" + std::to_wstring(canonicalAddress(addr, adv));
        device.name = name;
        device.type = BatteryDevice::Type::Bluetooth;
        device.subType = BatteryDevice::SubType::XiaomiBuds;
        device.leftPercent = adv.leftPercent;
        device.rightPercent = adv.rightPercent;
        device.casePercent = adv.casePercent;
        device.charging = adv.charging;
        device.leftCharging = adv.leftCharging;
        device.rightCharging = adv.rightCharging;
        device.caseCharging = adv.caseCharging;
        device.paired = pairedFlag;
        // 百分比取三路有效值的最低，用于排序 / 低电量提醒 / 整体进度条。
        int minPct = -1;
        for (int v : {adv.leftPercent, adv.rightPercent, adv.casePercent})
        {
            if (v >= 0 && (minPct < 0 || v < minPct))
            {
                minPct = v;
            }
        }
        device.percentage = minPct;
        device.level = levelFromPercentage(minPct);
        device.connected = true; // 能收到广播即视为在线。

        // 同一副耳机的左右耳可能各自以不同广播地址发包，规范化后得到相同 id；
        // 按 id 合并（保序，取并集：未知路用对方帧补齐，充电位取或）。
        auto it = std::find_if(devices.begin(), devices.end(),
                               [&](const BatteryDevice &d) { return d.id == device.id; });
        if (it == devices.end())
        {
            devices.push_back(std::move(device));
        }
        else
        {
            int vals[3] = {device.leftPercent, device.rightPercent, device.casePercent};
            int *dst[3] = {&it->leftPercent, &it->rightPercent, &it->casePercent};
            bool chg[3] = {device.leftCharging, device.rightCharging, device.caseCharging};
            bool *dstChg[3] = {&it->leftCharging, &it->rightCharging, &it->caseCharging};
            int minPct2 = -1;
            for (int i = 0; i < 3; ++i)
            {
                if (*dst[i] < 0 && vals[i] >= 0)
                {
                    *dst[i] = vals[i];
                }
                *dstChg[i] = *dstChg[i] || chg[i];
                if (*dst[i] >= 0 && (minPct2 < 0 || *dst[i] < minPct2))
                {
                    minPct2 = *dst[i];
                }
            }
            it->charging = it->leftCharging || it->rightCharging || it->caseCharging;
            it->percentage = minPct2;
            it->level = levelFromPercentage(minPct2);
        }
    }

    LOG_VERBOSE_W(L"[XiaomiBuds] readDevices total = " + std::to_wstring(devices.size()));
    return devices;
}
