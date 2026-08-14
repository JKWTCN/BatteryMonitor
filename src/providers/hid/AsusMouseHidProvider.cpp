#include "AsusMouseHidProvider.h"
#include "HidApiLock.h"
#include "util/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <hidapi.h>

namespace
{
constexpr uint16_t kVendorId = 0x0B05;
constexpr uint8_t kBatteryCommand = 0x12;
constexpr int kReadTimeoutMs = 1000;
constexpr size_t kMaximumReportSize = 65;

struct AsusMouseDeviceEntry
{
    uint16_t productId;
    int interfaceNumber;
    uint16_t usagePage;
    uint16_t usage;
    uint8_t reportId;
    size_t reportSize;
    uint8_t batterySubcommand;
    size_t percentageOffset;
    int percentageMultiplier;
    size_t chargingOffset;
    const wchar_t *name;
    const wchar_t *pathMarker;
};

// usagePage/usage 为 0 时仅按接口号匹配。老款鼠标以 0..4 档表示电量，
// 新款直接返回百分比；少数型号的电量字段位置不同。
constexpr AsusMouseDeviceEntry kDevices[] = {
    {0x1A1A, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Chakram X (Wireless)", L""},
    {0x1A18, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Chakram X (Wired)", L""},
    {0x1A72, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Gladius III AimPoint (Wireless)", L""},
    {0x1A70, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Gladius III AimPoint (Wired)", L""},
    {0x18A0, 2, 0, 0, 0x00, 65, 0x07, 4, 25, 9, L"ROG Gladius II Wireless", L""},
    {0x1960, 0, 0, 0, 0x00, 65, 0x07, 4, 25, 9, L"ROG Keris Wireless", L""},
    {0x195E, 0, 0, 0, 0x00, 65, 0x07, 4, 25, 9, L"ROG Keris Wireless (Wired)", L""},
    {0x1A59, 0, 0, 0, 0x00, 65, 0x07, 4, 25, 9, L"ROG Keris Wireless EVA Edition", L""},
    {0x1A57, 0, 0, 0, 0x00, 65, 0x07, 4, 25, 9, L"ROG Keris Wireless EVA Edition (Wired)", L""},
    {0x19F4, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"TUF Gaming M4 Wireless", L""},
    {0x1A8D, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"TUF Gaming M4 Wireless (CN)", L""},
    {0x1AF5, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"TX Gaming Mini (Wireless)", L""},
    {0x1AF3, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"TX Gaming Mini (Wired)", L""},
    {0x1C57, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"TUF Gaming Mini Miku (Wireless)", L""},
    {0x1C56, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"TUF Gaming Mini Miku (Wired)", L""},
    {0x1949, 0, 0, 0, 0x00, 65, 0x07, 4, 25, 9, L"ROG Strix Impact II (Wireless)", L""},
    {0x1947, 0, 0, 0, 0x00, 65, 0x07, 4, 25, 9, L"ROG Strix Impact II Wireless (Wired)", L""},
    {0x197F, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Gladius III (Wireless)", L""},
    {0x197D, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Gladius III (Wired)", L""},
    {0x1B0C, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Gladius III EVA-02 Edition (Wireless)", L""},
    {0x1B0A, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Gladius III EVA-02 Edition (Wired)", L""},
    {0x1A94, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Harpe Ace Aim Lab Edition (Wireless)", L""},
    {0x1A92, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Harpe Ace Aim Lab Edition (Wired)", L""},
    {0x1B67, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Harpe Ace Extreme (Wired)", L""},
    {0x1B63, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Harpe Ace Mini (Wired)", L""},
    {0x1AD0, 2, 0, 0, 0x03, 64, 0x07, 4, 1, 9, L"ROG Harpe II Ace (Wireless)", L"col03"},
    {0x1C69, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Harpe II Ace (Wired)", L""},
    {0x1A68, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Keris Wireless AimPoint (Wireless)", L""},
    {0x1A66, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Keris Wireless AimPoint (Wired)", L""},
    {0x1B16, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Keris II Ace (Wired)", L""},
    {0x1C0C, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Keris II Origin (Wired)", L""},
    {0x1D4C, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Keris II Origin KJP (Wired)", L""},
    {0x1908, 0, 0, 0, 0x00, 65, 0x07, 4, 25, 9, L"ROG Pugio II (Wireless)", L""},
    {0x1906, 0, 0, 0, 0x00, 65, 0x07, 4, 25, 9, L"ROG Pugio II (Wired)", L""},
    {0x18E5, 0, 0, 0, 0x00, 65, 0x07, 4, 25, 9, L"ROG Chakram (Wireless)", L""},
    {0x18E3, 0, 0, 0, 0x00, 65, 0x07, 4, 25, 9, L"ROG Chakram (Wired)", L""},
    {0x1979, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Spatha X (Wireless)", L""},
    {0x1977, 0, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ROG Spatha X (Wired)", L""},
    {0x18B4, 1, 0, 0, 0x00, 65, 0x07, 6, 25, 9, L"ROG Strix Carry", L""},
    {0x1A24, 2, 0, 0, 0x00, 65, 0x07, 4, 1, 9, L"ASUS SmartO Mouse MD200", L""},

    // 多设备接收器的鼠标配置通道。具体型号由当前配对设备决定。
    {0x1ACE, 2, 0, 0, 0x03, 64, 0x07, 4, 1, 9, L"ROG Omni Receiver Mouse", L"col03"},
};

std::wstring pathToWString(const char *path)
{
    if (!path) {
        return {};
    }
    return std::wstring(path, path + std::strlen(path));
}

std::wstring hidErrorString(hid_device *dev)
{
    if (!dev) {
        return L"unavailable";
    }
    const wchar_t *message = hid_error(dev);
    return std::wstring(message ? message : L"unknown");
}

BatteryLevel levelFromPercentage(int percentage)
{
    if (percentage >= 80) return BatteryLevel::Full;
    if (percentage >= 50) return BatteryLevel::Medium;
    if (percentage >= 20) return BatteryLevel::Low;
    return BatteryLevel::Empty;
}

bool containsIgnoreCase(const std::wstring &text, const std::wstring &needle)
{
    if (needle.empty()) {
        return true;
    }
    auto lower = [](wchar_t value) { return std::towlower(value); };
    return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                       [&](wchar_t left, wchar_t right) {
                           return lower(left) == lower(right);
                       }) != text.end();
}

const AsusMouseDeviceEntry *findEntry(const hid_device_info *info)
{
    const std::wstring path = pathToWString(info->path);
    for (const auto &entry : kDevices) {
        if (info->product_id != entry.productId ||
            info->interface_number != entry.interfaceNumber) {
            continue;
        }
        if (entry.usagePage != 0 && info->usage_page != entry.usagePage) {
            continue;
        }
        if (entry.usage != 0 && info->usage != entry.usage) {
            continue;
        }
        if (!containsIgnoreCase(path, entry.pathMarker)) {
            continue;
        }
        return &entry;
    }
    return nullptr;
}

// hidapi 后端可能保留 Report ID，也可能只返回协议 payload。
std::vector<unsigned char> normalizedResponse(const std::vector<unsigned char> &raw,
                                              const AsusMouseDeviceEntry &entry)
{
    if (raw.size() >= 3 && raw[0] == entry.reportId &&
        raw[1] == kBatteryCommand && raw[2] == entry.batterySubcommand) {
        return {raw.begin() + 1, raw.end()};
    }
    return raw;
}

void drainInput(hid_device *dev)
{
    std::vector<unsigned char> buffer(kMaximumReportSize + 1, 0);
    for (int i = 0; i < 32; ++i) {
        if (hid_read_timeout(dev, buffer.data(), buffer.size(), 1) <= 0) {
            return;
        }
    }
}

std::optional<std::vector<unsigned char>> readBatteryResponse(
    hid_device *dev, const AsusMouseDeviceEntry &entry)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kReadTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int remainingMs = std::max(1, static_cast<int>(remaining.count()));

        // 多留一个字节，以兼容保留 Report ID 的 hidapi 后端。
        std::vector<unsigned char> buffer(kMaximumReportSize + 1, 0);
        const int count = hid_read_timeout(dev, buffer.data(), buffer.size(), remainingMs);
        if (count < 0) {
            LOG_VERBOSE_W(L"[ASUS Mouse] hid_read_timeout failed: " + hidErrorString(dev));
            return std::nullopt;
        }
        if (count == 0) {
            continue;
        }

        buffer.resize(static_cast<size_t>(count));
        auto response = normalizedResponse(buffer, entry);
        if (response.size() >= 2 && response[0] == kBatteryCommand &&
            response[1] == entry.batterySubcommand) {
            return response;
        }
        // 接口也可能上报其它异步消息；在本次查询超时前继续等待目标响应。
        LOG_VERBOSE_W(L"[ASUS Mouse] ignored unrelated HID input report");
    }
    return std::nullopt;
}

std::optional<BatteryDevice> queryBattery(hid_device *dev,
                                          const std::wstring &path,
                                          const AsusMouseDeviceEntry &entry)
{
    drainInput(dev);

    // hid_write 缓冲区的第一个字节始终是 Report ID。
    std::vector<unsigned char> request(entry.reportSize, 0);
    request[0] = entry.reportId;
    request[1] = kBatteryCommand;
    request[2] = entry.batterySubcommand;
    if (hid_write(dev, request.data(), request.size()) <= 0) {
        LOG_VERBOSE_W(L"[ASUS Mouse] hid_write failed: " + hidErrorString(dev));
        return std::nullopt;
    }

    auto response = readBatteryResponse(dev, entry);
    const size_t requiredSize = std::max(entry.percentageOffset, entry.chargingOffset) + 1;
    if (!response || response->size() < requiredSize) {
        LOG_VERBOSE_W(L"[ASUS Mouse] battery response missing or too short");
        return std::nullopt;
    }

    const int rawPercentage = (*response)[entry.percentageOffset];
    const int percentage = rawPercentage * entry.percentageMultiplier;
    const int chargingValue = (*response)[entry.chargingOffset];
    if (percentage > 100) {
        LOG_WARN_W(L"[ASUS Mouse] invalid battery percentage: " +
                   std::to_wstring(percentage));
        return std::nullopt;
    }
    BatteryDevice device;
    device.id = L"asus-mouse:" + path;
    device.name = entry.name;
    device.type = BatteryDevice::Type::Hid;
    device.percentage = percentage;
    device.level = levelFromPercentage(percentage);
    device.charging = chargingValue != 0;
    device.connected = true;

    LOG_VERBOSE_W(L"[ASUS Mouse] " + std::wstring(entry.name) + L" battery=" +
                  std::to_wstring(percentage) + L"% charging=" +
                  (device.charging ? L"1" : L"0"));
    return device;
}
} // namespace

std::wstring AsusMouseHidProvider::displayName() const
{
    return L"HID";
}

std::vector<BatteryDevice> AsusMouseHidProvider::readDevices()
{
    std::lock_guard<std::recursive_mutex> hidLock(hidApiMutex());
    std::vector<BatteryDevice> devices;

    if (hid_init() != 0) {
        LOG_ERR("[ASUS Mouse] hid_init failed");
        return devices;
    }

    hid_device_info *head = hid_enumerate(kVendorId, 0);
    std::vector<uint16_t> completedProducts;
    for (hid_device_info *cur = head; cur; cur = cur->next) {
        const AsusMouseDeviceEntry *entry = findEntry(cur);
        if (!entry || std::find(completedProducts.begin(), completedProducts.end(),
                                cur->product_id) != completedProducts.end()) {
            continue;
        }

        const std::wstring path = pathToWString(cur->path);
        hid_device *dev = hid_open_path(cur->path);
        if (!dev) {
            LOG_WARN_W(L"[ASUS Mouse] hid_open_path failed for " +
                       std::wstring(entry->name));
            continue;
        }
        auto result = queryBattery(dev, path, *entry);
        hid_close(dev);
        if (result) {
            devices.push_back(std::move(*result));
            completedProducts.push_back(cur->product_id);
        }
    }

    hid_free_enumeration(head);
    LOG_VERBOSE_W(L"[ASUS Mouse] readDevices total = " + std::to_wstring(devices.size()));
    return devices;
}
