#include "AsusRogHidProvider.h"
#include "HidApiLock.h"
#include "util/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <hidapi.h>

namespace
{
constexpr uint16_t kVendorId = 0x0B05;
constexpr uint16_t kProductId = 0x1A07;
constexpr int kTargetInterface = 1;
constexpr uint16_t kTargetUsagePage = 0xFF00;
constexpr uint16_t kTargetUsage = 0x0001;
constexpr size_t kReportSize = 64;
constexpr uint8_t kBatteryCommand = 0x12;
constexpr uint8_t kBatterySubcommand = 0x01;
constexpr int kReadTimeoutMs = 1000;

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

// hidapi 后端对无编号报告的处理略有差异：通常 read() 直接返回 64 字节
// payload，部分后端会保留开头的 00 Report ID。这里统一返回协议 payload。
std::vector<unsigned char> normalizedResponse(const std::vector<unsigned char> &raw)
{
    if (raw.size() >= 3 && raw[0] == 0x00 && raw[1] == kBatteryCommand) {
        return {raw.begin() + 1, raw.end()};
    }
    return raw;
}

std::optional<std::vector<unsigned char>> readBatteryResponse(hid_device *dev)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kReadTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int remainingMs = std::max(1, static_cast<int>(remaining.count()));

        // 多留一个字节，以兼容保留 Report ID 的 hidapi 后端。
        std::vector<unsigned char> buffer(kReportSize + 1, 0);
        const int count = hid_read_timeout(dev, buffer.data(), buffer.size(), remainingMs);
        if (count < 0) {
            LOG_VERBOSE_W(L"[ASUS ROG] hid_read_timeout failed: " + hidErrorString(dev));
            return std::nullopt;
        }
        if (count == 0) {
            continue;
        }

        buffer.resize(static_cast<size_t>(count));
        auto response = normalizedResponse(buffer);
        if (response.size() >= 2 && response[0] == kBatteryCommand &&
            response[1] == kBatterySubcommand) {
            return response;
        }
        // 接口也可能上报其它异步消息；在本次查询超时前继续等待目标响应。
        LOG_VERBOSE_W(L"[ASUS ROG] ignored unrelated HID input report");
    }
    return std::nullopt;
}

std::optional<BatteryDevice> queryBattery(hid_device *dev,
                                          const std::wstring &path,
                                          const std::wstring &name)
{
    // hid_write 的第一个字节必须是 Report ID；设备没有实际 Report ID，故填 0。
    std::vector<unsigned char> request(kReportSize + 1, 0);
    request[1] = kBatteryCommand;
    request[2] = kBatterySubcommand;
    if (hid_write(dev, request.data(), request.size()) <= 0) {
        LOG_VERBOSE_W(L"[ASUS ROG] hid_write failed: " + hidErrorString(dev));
        return std::nullopt;
    }

    auto response = readBatteryResponse(dev);
    if (!response || response->size() < 11) {
        LOG_VERBOSE_W(L"[ASUS ROG] battery response missing or shorter than 11 bytes");
        return std::nullopt;
    }

    const int percentage = (*response)[5];
    const int chargingValue = (*response)[8];
    if (percentage < 0 || percentage > 100) {
        LOG_WARN_W(L"[ASUS ROG] invalid battery percentage: " +
                   std::to_wstring(percentage));
        return std::nullopt;
    }
    if (chargingValue != 0 && chargingValue != 1) {
        LOG_WARN_W(L"[ASUS ROG] unknown charging status: " +
                   std::to_wstring(chargingValue));
    }
    if ((*response)[10] != percentage) {
        LOG_VERBOSE_W(L"[ASUS ROG] duplicate battery field differs: primary=" +
                      std::to_wstring(percentage) + L" duplicate=" +
                      std::to_wstring((*response)[10]));
    }

    BatteryDevice device;
    device.id = L"asus-rog:" + path;
    device.name = name;
    device.type = BatteryDevice::Type::Hid;
    device.percentage = percentage;
    device.level = levelFromPercentage(percentage);
    device.charging = chargingValue == 1;
    device.connected = true;

    LOG_VERBOSE_W(L"[ASUS ROG] " + name + L" battery=" +
                  std::to_wstring(percentage) + L"% charging=" +
                  (device.charging ? L"1" : L"0"));
    return device;
}
} // namespace

std::wstring AsusRogHidProvider::displayName() const
{
    return L"HID";
}

std::vector<BatteryDevice> AsusRogHidProvider::readDevices()
{
    std::lock_guard<std::recursive_mutex> hidLock(hidApiMutex());
    std::vector<BatteryDevice> devices;

    if (hid_init() != 0) {
        LOG_ERR("[ASUS ROG] hid_init failed");
        return devices;
    }

    hid_device_info *head = hid_enumerate(kVendorId, kProductId);
    for (hid_device_info *cur = head; cur; cur = cur->next) {
        if (cur->interface_number != kTargetInterface ||
            cur->usage_page != kTargetUsagePage || cur->usage != kTargetUsage) {
            continue;
        }

        const std::wstring path = pathToWString(cur->path);
        std::wstring name = cur->product_string ? cur->product_string : L"";
        if (name.empty()) {
            name = L"ROG Strix Scope RX TKL Wireless Deluxe";
        }

        hid_device *dev = hid_open_path(cur->path);
        if (!dev) {
            LOG_WARN_W(L"[ASUS ROG] hid_open_path failed for " + name);
            continue;
        }
        auto result = queryBattery(dev, path, name);
        hid_close(dev);
        if (result) {
            devices.push_back(std::move(*result));
            break;
        }
    }

    hid_free_enumeration(head);
    LOG_VERBOSE_W(L"[ASUS ROG] readDevices total = " + std::to_wstring(devices.size()));
    return devices;
}
