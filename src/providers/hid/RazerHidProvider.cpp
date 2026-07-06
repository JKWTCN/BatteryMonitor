#include "RazerHidProvider.h"
#include "util/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <hidapi.h>

namespace
{
constexpr uint16_t kRazerVendorId = 0x1532;
constexpr size_t kReportSize = 90;
constexpr size_t kReportWithIdSize = kReportSize + 1;
constexpr size_t kArgumentOffset = 8;
constexpr int kMaxRetries = 5;

constexpr uint8_t kStatusSuccessful = 0x02;

constexpr uint8_t kCommandClassBattery = 0x07;
constexpr uint8_t kCommandGetBattery = 0x80;
constexpr uint8_t kCommandGetCharging = 0x84;
constexpr uint8_t kBatteryDataSize = 0x02;

enum class RazerKind
{
    Mouse,
    Keyboard,
};

struct RazerDeviceEntry
{
    uint16_t pid;
    const wchar_t *name;
    RazerKind kind;
    uint8_t transactionId;
    int waitMs;
    bool chargingUnsupported;
    bool wired;
};

constexpr int kWaitDefaultMs = 1;
constexpr int kWaitKeyboardMs = 5;
constexpr int kWaitNewReceiverMs = 31;
constexpr int kWaitViperMs = 60;
constexpr int kWaitAtherisMs = 400;

constexpr RazerDeviceEntry kRazerDevices[] = {
    {0x001F, L"Razer Naga Epic", RazerKind::Mouse, 0xFF, kWaitDefaultMs, false, false},
    {0x0024, L"Razer Mamba 2012 (Wired)", RazerKind::Mouse, 0xFF, kWaitDefaultMs, false, true},
    {0x0025, L"Razer Mamba 2012 (Wireless)", RazerKind::Mouse, 0xFF, kWaitDefaultMs, false, false},
    {0x0032, L"Razer Ouroboros", RazerKind::Mouse, 0xFF, kWaitDefaultMs, false, false},
    {0x003E, L"Razer Naga Epic Chroma", RazerKind::Mouse, 0xFF, kWaitDefaultMs, false, false},
    {0x003F, L"Razer Naga Epic Chroma Dock", RazerKind::Mouse, 0xFF, kWaitDefaultMs, false, false},
    {0x0044, L"Razer Mamba (Wired)", RazerKind::Mouse, 0xFF, kWaitDefaultMs, false, true},
    {0x0045, L"Razer Mamba (Wireless)", RazerKind::Mouse, 0xFF, kWaitDefaultMs, false, false},
    {0x0059, L"Razer Lancehead (Wired)", RazerKind::Mouse, 0x3F, kWaitDefaultMs, false, true},
    {0x005A, L"Razer Lancehead (Wireless)", RazerKind::Mouse, 0x3F, kWaitDefaultMs, false, false},
    {0x0062, L"Razer Atheris Receiver", RazerKind::Mouse, 0x1F, kWaitAtherisMs, true, false},
    {0x006F, L"Razer Lancehead Wireless Receiver", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x0070, L"Razer Lancehead Wireless (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x0072, L"Razer Mamba Wireless Receiver", RazerKind::Mouse, 0x3F, kWaitNewReceiverMs, false, false},
    {0x0073, L"Razer Mamba Wireless (Wired)", RazerKind::Mouse, 0x3F, kWaitNewReceiverMs, false, true},
    {0x0077, L"Razer Pro Click Receiver", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x007A, L"Razer Viper Ultimate (Wired)", RazerKind::Mouse, 0xFF, kWaitViperMs, false, true},
    {0x007B, L"Razer Viper Ultimate (Wireless)", RazerKind::Mouse, 0xFF, kWaitViperMs, false, false},
    {0x007C, L"Razer DeathAdder V2 Pro (Wired)", RazerKind::Mouse, 0x3F, kWaitViperMs, false, true},
    {0x007D, L"Razer DeathAdder V2 Pro (Wireless)", RazerKind::Mouse, 0x3F, kWaitViperMs, false, false},
    {0x0080, L"Razer Pro Click (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x0083, L"Razer Basilisk X HyperSpeed", RazerKind::Mouse, 0xFF, kWaitNewReceiverMs, true, false},
    {0x0086, L"Razer Basilisk Ultimate (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x0088, L"Razer Basilisk Ultimate Receiver", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x008F, L"Razer Naga Pro (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x0090, L"Razer Naga Pro (Wireless)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x0094, L"Razer Orochi V2 Receiver", RazerKind::Mouse, 0x1F, kWaitAtherisMs, true, false},
    {0x0095, L"Razer Orochi V2 Bluetooth", RazerKind::Mouse, 0x1F, kWaitAtherisMs, true, false},
    {0x009A, L"Razer Pro Click Mini Receiver", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x009C, L"Razer DeathAdder V2 X HyperSpeed", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, true, false},
    {0x009E, L"Razer Viper Mini Signature Edition (Wired)", RazerKind::Mouse, 0x1F, kWaitViperMs, false, true},
    {0x009F, L"Razer Viper Mini Signature Edition (Wireless)", RazerKind::Mouse, 0x1F, kWaitViperMs, false, false},
    {0x00A5, L"Razer Viper V2 Pro (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x00A6, L"Razer Viper V2 Pro (Wireless)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x00A7, L"Razer Naga V2 Pro (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x00A8, L"Razer Naga V2 Pro (Wireless)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x00AA, L"Razer Basilisk V3 Pro (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x00AB, L"Razer Basilisk V3 Pro (Wireless)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x00AF, L"Razer Cobra Pro (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x00B0, L"Razer Cobra Pro (Wireless)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x00B3, L"Razer HyperPolling Wireless Dongle", RazerKind::Mouse, 0x1F, kWaitViperMs, false, false},
    {0x00B4, L"Razer Naga V2 HyperSpeed Receiver", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, true, false},
    {0x00B6, L"Razer DeathAdder V3 Pro (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x00B7, L"Razer DeathAdder V3 Pro (Wireless)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x00B8, L"Razer Viper V3 HyperSpeed", RazerKind::Mouse, 0x1F, kWaitViperMs, true, false},
    {0x00B9, L"Razer Basilisk V3 X HyperSpeed", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, true, false},
    {0x00BE, L"Razer DeathAdder V4 Pro (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x00BF, L"Razer DeathAdder V4 Pro (Wireless)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x00C0, L"Razer Viper V3 Pro (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x00C1, L"Razer Viper V3 Pro (Wireless)", RazerKind::Mouse, 0x1F, kWaitViperMs, false, false},
    {0x00C2, L"Razer DeathAdder V3 Pro (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x00C3, L"Razer DeathAdder V3 Pro (Wireless)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x00C4, L"Razer DeathAdder V3 HyperSpeed (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x00C5, L"Razer DeathAdder V3 HyperSpeed (Wireless)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x00C7, L"Razer Pro Click V2 Vertical (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x00C8, L"Razer Pro Click V2 Vertical (Wireless)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x00CB, L"Razer Basilisk V3 35K", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, true, true},
    {0x00CC, L"Razer Basilisk V3 Pro 35K (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x00CD, L"Razer Basilisk V3 Pro 35K (Wireless)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x00D0, L"Razer Pro Click V2 (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x00D1, L"Razer Pro Click V2 (Wireless)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},
    {0x00D3, L"Razer Basilisk Mobile (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, true, true},
    {0x00D4, L"Razer Basilisk Mobile Receiver", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, true, false},
    {0x00D6, L"Razer Basilisk V3 Pro 35K Phantom Green Edition (Wired)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, true},
    {0x00D7, L"Razer Basilisk V3 Pro 35K Phantom Green Edition (Wireless)", RazerKind::Mouse, 0x1F, kWaitNewReceiverMs, false, false},

    {0x0258, L"Razer BlackWidow V3 Mini HyperSpeed (Wired)", RazerKind::Keyboard, 0x1F, kWaitKeyboardMs, false, true},
    {0x025A, L"Razer BlackWidow V3 Pro (Wired)", RazerKind::Keyboard, 0x3F, kWaitKeyboardMs, false, true},
    {0x025C, L"Razer BlackWidow V3 Pro (Wireless)", RazerKind::Keyboard, 0x9F, kWaitKeyboardMs, false, false},
    {0x0271, L"Razer BlackWidow V3 Mini HyperSpeed (Wireless)", RazerKind::Keyboard, 0x9F, kWaitKeyboardMs, false, false},
    {0x0290, L"Razer DeathStalker V2 Pro (Wireless)", RazerKind::Keyboard, 0x9F, kWaitKeyboardMs, false, false},
    {0x0292, L"Razer DeathStalker V2 Pro (Wired)", RazerKind::Keyboard, 0x1F, kWaitKeyboardMs, false, true},
    {0x0296, L"Razer DeathStalker V2 Pro TKL (Wireless)", RazerKind::Keyboard, 0x9F, kWaitKeyboardMs, false, false},
    {0x0298, L"Razer DeathStalker V2 Pro TKL (Wired)", RazerKind::Keyboard, 0x1F, kWaitKeyboardMs, false, true},
    {0x02B9, L"Razer BlackWidow V4 Mini HyperSpeed (Wired)", RazerKind::Keyboard, 0x1F, kWaitKeyboardMs, false, true},
    {0x02BA, L"Razer BlackWidow V4 Mini HyperSpeed (Wireless)", RazerKind::Keyboard, 0x9F, kWaitKeyboardMs, false, false},
    {0x02D5, L"Razer BlackWidow V4 Tenkeyless HyperSpeed (Wireless)", RazerKind::Keyboard, 0x9F, kWaitKeyboardMs, false, false},
    {0x02D7, L"Razer BlackWidow V4 Tenkeyless HyperSpeed (Wired)", RazerKind::Keyboard, 0x1F, kWaitKeyboardMs, false, true},
};

const RazerDeviceEntry *findRazerEntry(uint16_t pid)
{
    for (const RazerDeviceEntry &entry : kRazerDevices) {
        if (entry.pid == pid) {
            return &entry;
        }
    }
    return nullptr;
}

std::wstring toHex4(unsigned short v)
{
    wchar_t buf[8];
    swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%04X", v);
    return std::wstring(buf);
}

std::wstring pathToWString(const char *path)
{
    if (!path) {
        return {};
    }
    return std::wstring(path, path + std::strlen(path));
}

BatteryLevel levelFromPercentage(int percentage)
{
    if (percentage < 0) {
        return BatteryLevel::Unknown;
    }
    if (percentage >= 80) {
        return BatteryLevel::Full;
    }
    if (percentage >= 50) {
        return BatteryLevel::Medium;
    }
    if (percentage >= 20) {
        return BatteryLevel::Low;
    }
    return BatteryLevel::Empty;
}

int percentageFromByte(uint8_t raw)
{
    return (static_cast<int>(raw) * 100 + 127) / 255;
}

uint8_t calculateCrc(const std::vector<unsigned char> &payload)
{
    uint8_t crc = 0;
    for (size_t i = 2; i < 88 && i < payload.size(); ++i) {
        crc ^= payload[i];
    }
    return crc;
}

std::vector<unsigned char> makeReport(uint8_t transactionId, uint8_t commandId)
{
    std::vector<unsigned char> payload(kReportSize, 0);
    payload[0] = 0x00; // 新命令
    payload[1] = transactionId;
    payload[4] = 0x00; // 协议类型
    payload[5] = kBatteryDataSize;
    payload[6] = kCommandClassBattery;
    payload[7] = commandId;
    payload[88] = calculateCrc(payload);
    return payload;
}

bool responseMatches(const std::vector<unsigned char> &response,
                     const std::vector<unsigned char> &request)
{
    if (response.size() < kReportSize || request.size() < kReportSize) {
        return false;
    }
    if (response[2] != request[2] || response[3] != request[3]) {
        return false;
    }
    if (response[6] != request[6] || response[7] != request[7]) {
        return false;
    }
    return response[0] == kStatusSuccessful;
}

std::optional<std::vector<unsigned char>> sendReport(hid_device *dev,
                                                     const RazerDeviceEntry &entry,
                                                     uint8_t commandId)
{
    std::vector<unsigned char> request = makeReport(entry.transactionId, commandId);

    for (int retry = 0; retry < kMaxRetries; ++retry) {
        std::vector<unsigned char> out(kReportWithIdSize, 0);
        std::copy(request.begin(), request.end(), out.begin() + 1);

        if (hid_send_feature_report(dev, out.data(), out.size()) < 0) {
            LOG_VERBOSE_W(L"[Razer] hid_send_feature_report failed: " +
                          std::wstring(hid_error(dev) ? hid_error(dev) : L"unknown"));
            continue;
        }

        if (entry.waitMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(entry.waitMs));
        }

        std::vector<unsigned char> in(kReportWithIdSize, 0);
        in[0] = 0;
        const int n = hid_get_feature_report(dev, in.data(), in.size());
        if (n <= 0) {
            LOG_VERBOSE_W(L"[Razer] hid_get_feature_report failed: " +
                          std::wstring(hid_error(dev) ? hid_error(dev) : L"unknown"));
            continue;
        }

        std::vector<unsigned char> response;
        if (static_cast<size_t>(n) >= kReportWithIdSize) {
            response.assign(in.begin() + 1, in.begin() + 1 + kReportSize);
        } else if (static_cast<size_t>(n) >= kReportSize && in[0] != 0) {
            response.assign(in.begin(), in.begin() + kReportSize);
        } else {
            continue;
        }

        if (responseMatches(response, request)) {
            return response;
        }

        LOG_VERBOSE_W(L"[Razer] response mismatch/status=0x" +
                      toHex4(static_cast<unsigned short>(response.empty() ? 0 : response[0])));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return std::nullopt;
}

BatteryDevice makeDevice(const std::wstring &id, const std::wstring &name,
                         const RazerDeviceEntry &entry, int percentage, bool charging)
{
    BatteryDevice device;
    device.id = id;
    device.name = name;
    device.type = BatteryDevice::Type::Hid;
    device.percentage = percentage;
    device.level = levelFromPercentage(percentage);
    device.charging = charging;
    device.wired = entry.wired || charging;
    device.connected = true;
    return device;
}

std::optional<BatteryDevice> queryRazerDevice(hid_device *dev,
                                              const RazerDeviceEntry &entry,
                                              const std::wstring &id,
                                              const std::wstring &name)
{
    auto batteryResp = sendReport(dev, entry, kCommandGetBattery);
    if (!batteryResp || batteryResp->size() <= kArgumentOffset + 1) {
        return std::nullopt;
    }

    const int percentage = percentageFromByte((*batteryResp)[kArgumentOffset + 1]);
    bool charging = false;
    if (!entry.chargingUnsupported) {
        auto chargingResp = sendReport(dev, entry, kCommandGetCharging);
        if (chargingResp && chargingResp->size() > kArgumentOffset + 1) {
            charging = (*chargingResp)[kArgumentOffset + 1] != 0;
        }
    }

    LOG_VERBOSE_W(L"[Razer] " + name + L" battery=" + std::to_wstring(percentage) +
                  L"% charging=" + (charging ? L"1" : L"0") +
                  L" kind=" + std::to_wstring(static_cast<int>(entry.kind)));
    return makeDevice(id, name, entry, percentage, charging);
}
} // namespace

std::wstring RazerHidProvider::displayName() const
{
    return L"HID";
}

std::vector<BatteryDevice> RazerHidProvider::readDevices()
{
    std::vector<BatteryDevice> devices;

    if (hid_init() != 0) {
        LOG_ERR("[Razer] hid_init failed");
        return devices;
    }

    hid_device_info *enumHead = hid_enumerate(kRazerVendorId, 0);
    if (!enumHead) {
        LOG_VERBOSE_W(L"[Razer] hid_enumerate(0x1532) returned 0 devices");
        return devices;
    }

    auto vidPidKey = [](uint16_t vid, uint16_t pid) {
        return (static_cast<uint64_t>(vid) << 16) | pid;
    };

    struct Candidate
    {
        hid_device_info *info;
        const RazerDeviceEntry *entry;
    };

    std::vector<std::pair<uint64_t, std::vector<Candidate>>> grouped;
    std::unordered_set<uint64_t> seenDevKeys;

    int totalInterfaces = 0;
    for (hid_device_info *cur = enumHead; cur; cur = cur->next) {
        ++totalInterfaces;
        const RazerDeviceEntry *entry = findRazerEntry(cur->product_id);
        if (!entry) {
            continue;
        }
        const uint64_t key = vidPidKey(cur->vendor_id, cur->product_id);
        if (seenDevKeys.insert(key).second) {
            grouped.push_back({key, {}});
        }
        for (auto &g : grouped) {
            if (g.first == key) {
                g.second.push_back({cur, entry});
                break;
            }
        }
    }

    LOG_VERBOSE_W(L"[Razer] hid_enumerate(0x1532) returned " +
                  std::to_wstring(totalInterfaces) + L" interface(s), " +
                  std::to_wstring(grouped.size()) + L" supported device group(s)");

    auto tryInterface = [&](const Candidate &cand) -> std::optional<BatteryDevice> {
        hid_device_info *cur = cand.info;
        const RazerDeviceEntry &entry = *cand.entry;
        const std::wstring path = pathToWString(cur->path);

        LOG_VERBOSE_W(L"[Razer] interface: pid=0x" + toHex4(cur->product_id) +
                      L" usage_page=0x" + toHex4(cur->usage_page) +
                      L" usage=0x" + toHex4(cur->usage) +
                      L" interface=" + std::to_wstring(cur->interface_number) +
                      L" path=" + path);

        hid_device *dev = hid_open_path(cur->path);
        if (!dev) {
            LOG_VERBOSE_W(L"[Razer]   hid_open_path failed: " +
                          std::wstring(hid_error(nullptr) ? hid_error(nullptr) : L"unknown"));
            return std::nullopt;
        }

        const std::wstring id = L"razer:" + path;
        std::wstring name = cur->product_string ? cur->product_string : L"";
        if (name.empty()) {
            name = entry.name;
        }

        std::optional<BatteryDevice> result;
        try {
            result = queryRazerDevice(dev, entry, id, name);
        } catch (const std::exception &e) {
            LOG_ERR_W(L"[Razer]   query threw for " + name + L": " +
                      std::wstring(e.what(), e.what() + std::strlen(e.what())));
        } catch (...) {
            LOG_ERR_W(L"[Razer]   query threw unknown exception for " + name);
        }

        hid_close(dev);
        return result;
    };

    auto shouldPublishReading = [&](uint64_t key, const BatteryDevice &device) {
        if (device.percentage > 0) {
            m_lastNonZeroPercentage[key] = device.percentage;
            m_zeroReadStreak[key] = 0;
            return true;
        }

        if (device.percentage != 0) {
            return true;
        }

        const auto lastIt = m_lastNonZeroPercentage.find(key);
        if (lastIt == m_lastNonZeroPercentage.end() || lastIt->second <= 0) {
            return true;
        }

        const int streak = ++m_zeroReadStreak[key];
        if (streak < 3) {
            LOG_WARN_W(L"[Razer]   " + device.name +
                       L" reported 0% after " + std::to_wstring(lastIt->second) +
                       L"%; suppressing transient zero (" +
                       std::to_wstring(streak) + L"/3)");
            return false;
        }

        LOG_WARN_W(L"[Razer]   " + device.name +
                   L" reported 0% for 3 consecutive reads; accepting zero");
        m_lastNonZeroPercentage.erase(key);
        return true;
    };

    for (auto &g : grouped) {
        bool got = false;
        bool suppressed = false;
        const char *successPath = nullptr;

        const auto cacheIt = m_lastGoodPath.find(g.first);
        if (cacheIt != m_lastGoodPath.end()) {
            for (const Candidate &cand : g.second) {
                if (std::strcmp(cand.info->path, cacheIt->second.c_str()) == 0) {
                    LOG_VERBOSE_W(L"[Razer]   trying cached interface path first");
                    if (auto result = tryInterface(cand)) {
                        successPath = cand.info->path;
                        if (shouldPublishReading(g.first, *result)) {
                            devices.push_back(*result);
                            got = true;
                        } else {
                            suppressed = true;
                        }
                    }
                    break;
                }
            }
        }

        if (!got && !suppressed) {
            for (const Candidate &cand : g.second) {
                if (cacheIt != m_lastGoodPath.end() &&
                    std::strcmp(cand.info->path, cacheIt->second.c_str()) == 0) {
                    continue;
                }
                if (auto result = tryInterface(cand)) {
                    successPath = cand.info->path;
                    if (shouldPublishReading(g.first, *result)) {
                        devices.push_back(*result);
                        got = true;
                    } else {
                        suppressed = true;
                    }
                    break;
                }
            }
        }

        if ((got || suppressed) && successPath) {
            m_lastGoodPath[g.first] = successPath;
        }

        if (!got && !suppressed && cacheIt != m_lastGoodPath.end()) {
            bool stillPresent = false;
            for (const Candidate &cand : g.second) {
                if (std::strcmp(cand.info->path, cacheIt->second.c_str()) == 0) {
                    stillPresent = true;
                    break;
                }
            }
            if (!stillPresent) {
                m_lastGoodPath.erase(g.first);
            }
        }

        if (!got && !suppressed && !g.second.empty()) {
            const RazerDeviceEntry &entry = *g.second.front().entry;
            LOG_WARN_W(L"[Razer]   " + std::wstring(entry.name) +
                       L" query returned no data this cycle "
                       L"(tried " + std::to_wstring(g.second.size()) +
                       L" interface(s))");
        }
    }

    hid_free_enumeration(enumHead);
    LOG_VERBOSE_W(L"[Razer] readDevices total = " + std::to_wstring(devices.size()));
    return devices;
}
