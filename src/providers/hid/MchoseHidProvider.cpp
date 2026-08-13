#include "MchoseHidProvider.h"
#include "HidApiLock.h"
#include "util/Logger.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <hidapi.h>

namespace
{
constexpr uint8_t kReportId = 0x11;
constexpr uint8_t kDeviceInfoCommand = 0x06;
constexpr size_t kReportLength = 21;
constexpr size_t kFeatureReadLength = 65;
constexpr int kResponseDelayMs = 10;
constexpr uint16_t kUsagePageVendor = 0xFF01;
constexpr uint16_t kUsagePageVendorAlt = 0xFF0B;

struct UsbIdentity
{
    uint16_t vendorId;
    uint16_t productId;
    const wchar_t *name;
};

// 0x100A/0x100B/0x1020 是 A7 V2 系列共用的接收器身份；最终型号以
// 设备信息响应中的逻辑 PID 为准。
constexpr UsbIdentity kUsbIdentities[] = {
    {0x3837, 0x4018, L"MCHOSE A7 V2 Pro"},
    {0x3837, 0x4023, L"MCHOSE A7 V2 Pro+"},
    {0x3837, 0x4019, L"MCHOSE A7 V2 Ultra"},
    {0x3837, 0x4021, L"MCHOSE A7 V2 Ultra+"},
    {0x3837, 0x100A, L"MCHOSE A7 V2 (1K Dongle)"},
    {0x3837, 0x100B, L"MCHOSE A7 V2 (8K Dongle)"},
    {0x5253, 0x1020, L"MCHOSE A7 V2 (8K MagDock)"},
};

struct QueryResult
{
    BatteryDevice device;
    uint16_t logicalVendorId = 0;
    uint16_t logicalProductId = 0;
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

const UsbIdentity *findUsbIdentity(uint16_t vendorId, uint16_t productId)
{
    const auto it = std::find_if(std::begin(kUsbIdentities), std::end(kUsbIdentities),
                                 [&](const UsbIdentity &identity) {
                                     return identity.vendorId == vendorId &&
                                            identity.productId == productId;
                                 });
    return it == std::end(kUsbIdentities) ? nullptr : &*it;
}

const wchar_t *logicalProductName(uint16_t productId)
{
    switch (productId) {
    case 0x4018: return L"MCHOSE A7 V2 Pro";
    case 0x4023: return L"MCHOSE A7 V2 Pro+";
    case 0x4019: return L"MCHOSE A7 V2 Ultra";
    case 0x4021: return L"MCHOSE A7 V2 Ultra+";
    default: return nullptr;
    }
}

bool isControlCollection(const hid_device_info *info)
{
    if (!info || info->interface_number != 2) {
        return false;
    }
    return info->usage_page == kUsagePageVendor ||
           info->usage_page == kUsagePageVendorAlt;
}

uint16_t readLe16(const unsigned char *data)
{
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLe32(const unsigned char *data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

std::wstring hexWord(uint16_t value)
{
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring result(4, L'0');
    for (int index = 3; index >= 0; --index) {
        result[static_cast<size_t>(index)] = digits[value & 0x0F];
        value = static_cast<uint16_t>(value >> 4);
    }
    return result;
}

std::optional<QueryResult> queryDevice(hid_device *dev,
                                       const hid_device_info *info,
                                       const UsbIdentity &identity)
{
    std::array<unsigned char, kReportLength> request{};
    request.fill(0xFF);
    request[0] = kReportId;
    request[1] = static_cast<unsigned char>(kDeviceInfoCommand ^ 0xFF);

    const int written = hid_send_feature_report(dev, request.data(), request.size());
    if (written < static_cast<int>(request.size())) {
        LOG_VERBOSE_W(L"[MCHOSE] Feature query is unsupported by this collection: " +
                      hidErrorString(dev));
        return std::nullopt;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kResponseDelayMs));

    std::array<unsigned char, kFeatureReadLength> response{};
    response[0] = kReportId;
    const int count = hid_get_feature_report(dev, response.data(), response.size());
    if (count <= 0) {
        LOG_VERBOSE_W(L"[MCHOSE] hid_get_feature_report failed: " + hidErrorString(dev));
        return std::nullopt;
    }

    size_t offset = response[0] == kReportId ? 1 : 0;
    if (static_cast<size_t>(count) < offset + 12) {
        LOG_VERBOSE_W(L"[MCHOSE] device information response is too short");
        return std::nullopt;
    }

    std::array<unsigned char, 12> decoded{};
    for (size_t index = 0; index < decoded.size(); ++index) {
        decoded[index] = static_cast<unsigned char>(response[offset + index] ^ 0xFF);
    }
    if (decoded[0] != kDeviceInfoCommand) {
        LOG_VERBOSE_W(L"[MCHOSE] unexpected Feature response command");
        return std::nullopt;
    }

    const uint16_t logicalVendorId = readLe16(decoded.data() + 1);
    const uint16_t logicalProductId = readLe16(decoded.data() + 3);
    const uint32_t firmware = readLe32(decoded.data() + 5);
    const uint8_t flags = decoded[9];
    const int percentage = decoded[10];
    const int chargingStatus = decoded[11];
    if (logicalVendorId == 0 || logicalProductId == 0) {
        LOG_VERBOSE_W(L"[MCHOSE] receiver reports no connected mouse");
        return std::nullopt;
    }
    if (percentage < 0 || percentage > 100) {
        LOG_WARN_W(L"[MCHOSE] invalid battery percentage: " +
                   std::to_wstring(percentage));
        return std::nullopt;
    }

    const wchar_t *modelName = logicalProductName(logicalProductId);
    std::wstring name = modelName ? modelName : identity.name;

    BatteryDevice device;
    device.id = L"mchose:" + hexWord(logicalVendorId) + L":" +
                hexWord(logicalProductId);
    device.name = std::move(name);
    device.type = BatteryDevice::Type::Hid;
    device.percentage = percentage;
    device.level = levelFromPercentage(percentage);
    device.charging = chargingStatus == 1;
    device.connected = true;

    const int connectMode = (flags >> 5) & 0x07;
    LOG_VERBOSE_W(L"[MCHOSE] " + device.name + L" battery=" +
                  std::to_wstring(percentage) + L"% charging=" +
                  (device.charging ? L"1" : L"0") + L" mode=" +
                  std::to_wstring(connectMode) + L" firmware=" +
                  std::to_wstring(firmware) + L" path=" +
                  pathToWString(info->path));

    return QueryResult{std::move(device), logicalVendorId, logicalProductId};
}

bool sameLogicalDevice(const QueryResult &left, const QueryResult &right)
{
    return left.logicalVendorId == right.logicalVendorId &&
           left.logicalProductId == right.logicalProductId;
}
} // namespace

std::wstring MchoseHidProvider::displayName() const
{
    return L"HID";
}

std::vector<BatteryDevice> MchoseHidProvider::readDevices()
{
    std::lock_guard<std::recursive_mutex> hidLock(hidApiMutex());
    std::vector<QueryResult> results;

    if (hid_init() != 0) {
        LOG_ERR("[MCHOSE] hid_init failed");
        return {};
    }

    constexpr uint16_t vendors[] = {0x3837, 0x5253};
    for (const uint16_t vendorId : vendors) {
        hid_device_info *head = hid_enumerate(vendorId, 0);
        std::vector<uint16_t> completedProducts;
        for (hid_device_info *cur = head; cur; cur = cur->next) {
            const UsbIdentity *identity = findUsbIdentity(cur->vendor_id, cur->product_id);
            if (!identity || !isControlCollection(cur) ||
                std::find(completedProducts.begin(), completedProducts.end(),
                          cur->product_id) != completedProducts.end()) {
                continue;
            }

            hid_device *dev = hid_open_path(cur->path);
            if (!dev) {
                LOG_VERBOSE_W(L"[MCHOSE] hid_open_path failed for " +
                              std::wstring(identity->name));
                continue;
            }
            auto result = queryDevice(dev, cur, *identity);
            hid_close(dev);
            if (!result) {
                continue;
            }

            completedProducts.push_back(cur->product_id);
            if (std::none_of(results.begin(), results.end(),
                             [&](const QueryResult &existing) {
                                 return sameLogicalDevice(existing, *result);
                             })) {
                results.push_back(std::move(*result));
            }
        }
        hid_free_enumeration(head);
    }

    std::vector<BatteryDevice> devices;
    devices.reserve(results.size());
    for (auto &result : results) {
        devices.push_back(std::move(result.device));
    }
    LOG_VERBOSE_W(L"[MCHOSE] readDevices total = " + std::to_wstring(devices.size()));
    return devices;
}
