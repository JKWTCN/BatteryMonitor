#include "LogitechHidProvider.h"
#include "HidApiLock.h"
#include "util/Logger.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <hidapi.h>

namespace
{
constexpr unsigned short kVendorId = 0x046D;
constexpr unsigned short kUsagePage = 0xFF00;
constexpr unsigned short kOutputUsage = 0x0001;
constexpr unsigned short kInputUsage = 0x0002;

constexpr unsigned char kShortReportId = 0x10;
constexpr unsigned char kLongReportId = 0x11;
constexpr unsigned char kErrorReportId = 0x8F;
constexpr std::size_t kShortReportLength = 7;
constexpr std::size_t kLongReportLength = 20;

constexpr unsigned char kRootFeatureIndex = 0x00;
constexpr unsigned short kBatteryStatusFeatureId = 0x1000;
constexpr unsigned short kBatteryVoltageFeatureId = 0x1001;
constexpr unsigned short kUnifiedBatteryFeatureId = 0x1004;
constexpr unsigned short kDeviceNameFeatureId = 0x0005;
// 低 4 bit 是 Software ID。使用与抓包中的 Logitech Options 不同的值，
// 以便同一输入流中准确过滤属于本 Provider 的响应。
constexpr unsigned char kSoftwareId = 0x07;
constexpr unsigned char kGetFeatureFunction = 0x00;

constexpr int kCommandTimeoutMs = 400;

struct ReceiverEntry
{
    unsigned short productId;
    int interfaceNumber;
    int maxDevices;
    const wchar_t *name;
    unsigned short vendorId = kVendorId;
};

// 已知 HID++ 接收器及其 vendor-defined 接口。具体外设的电池能力在运行时探测。
constexpr ReceiverEntry kReceiverEntries[] = {
    {0xC548, 2, 6, L"Logitech Bolt Receiver"},
    {0xC52B, 2, 6, L"Logitech Unifying Receiver"},
    {0xC532, 2, 6, L"Logitech Unifying Receiver"},
    {0xC52F, 1, 1, L"Logitech Nano Receiver"},
    {0xC518, 1, 1, L"Logitech Nano Receiver"},
    {0xC51A, 1, 1, L"Logitech Nano Receiver"},
    {0xC51B, 1, 1, L"Logitech Nano Receiver"},
    {0xC521, 1, 1, L"Logitech Nano Receiver"},
    {0xC525, 1, 1, L"Logitech Nano Receiver"},
    {0xC526, 1, 1, L"Logitech Nano Receiver"},
    {0xC52E, 1, 1, L"Logitech Nano Receiver"},
    {0xC531, 1, 1, L"Logitech Nano Receiver"},
    {0xC534, 1, 2, L"Logitech Nano Receiver"},
    {0xC535, 1, 1, L"Logitech Nano Receiver"},
    {0xC537, 1, 1, L"Logitech Nano Receiver"},
    {0x6042, 1, 1, L"Lenovo Nano Receiver", 0x17EF},
    {0xC539, 2, 1, L"Logitech Lightspeed Receiver"},
    {0xC53A, 2, 1, L"Logitech Lightspeed Receiver"},
    {0xC53D, 2, 1, L"Logitech Lightspeed Receiver"},
    {0xC53F, 2, 1, L"Logitech Lightspeed Receiver"},
    {0xC541, 2, 1, L"Logitech Lightspeed Receiver"},
    {0xC545, 2, 1, L"Logitech Lightspeed Receiver"},
    {0xC547, 2, 1, L"Logitech Lightspeed Receiver"},
    {0xC54D, 2, 1, L"Logitech Lightspeed Receiver"},
};

using Report = std::array<unsigned char, kLongReportLength>;
using Request = std::array<unsigned char, kShortReportLength>;

struct HidCollectionPaths
{
    std::string output;
    std::string input;
};

class HidHandles
{
public:
    HidHandles(hid_device *writer, hid_device *reader)
        : writer(writer), reader(reader)
    {
    }

    ~HidHandles()
    {
        if (reader) hid_close(reader);
        if (writer) hid_close(writer);
    }

    HidHandles(const HidHandles &) = delete;
    HidHandles &operator=(const HidHandles &) = delete;

    hid_device *writer = nullptr;
    hid_device *reader = nullptr;
};

std::wstring hidErrorString(hid_device *device)
{
    const wchar_t *message = hid_error(device);
    return message ? message : L"unknown";
}

BatteryLevel levelFromPercentage(int percentage)
{
    if (percentage >= 80) return BatteryLevel::Full;
    if (percentage >= 50) return BatteryLevel::Medium;
    if (percentage >= 20) return BatteryLevel::Low;
    return BatteryLevel::Empty;
}

unsigned char functionByte(unsigned char function)
{
    return static_cast<unsigned char>((function << 4) | kSoftwareId);
}

Request buildRequest(unsigned char deviceIndex,
                     unsigned char featureIndex,
                     unsigned char function,
                     std::array<unsigned char, 3> parameters = {0, 0, 0})
{
    return {
        kShortReportId,
        deviceIndex,
        featureIndex,
        functionByte(function),
        parameters[0],
        parameters[1],
        parameters[2],
    };
}

bool responseMatches(const Report &report, int count, const Request &request)
{
    return count >= 4 &&
           (report[0] == kShortReportId || report[0] == kLongReportId) &&
           report[1] == request[1] && report[2] == request[2] &&
           report[3] == request[3];
}

bool errorMatches(const Report &report, int count, const Request &request)
{
    return count >= 5 && report[0] == kErrorReportId &&
           report[1] == request[1] && report[2] == request[2] &&
           report[3] == request[3];
}

void drainInput(hid_device *reader)
{
    Report report{};
    for (int i = 0; i < 256; ++i) {
        if (hid_read_timeout(reader, report.data(), report.size(), 1) <= 0) {
            return;
        }
    }
}

std::optional<Report> sendCommand(hid_device *writer,
                                  hid_device *reader,
                                  const Request &request)
{
    drainInput(reader);

    const int written = hid_write(writer, request.data(), request.size());
    if (written != static_cast<int>(request.size())) {
        LOG_VERBOSE_W(L"[Logitech HID++] hid_write failed: " +
                      hidErrorString(writer));
        return std::nullopt;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kCommandTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int timeout = std::max(1, std::min(100, static_cast<int>(remaining.count())));

        Report report{};
        const int count = hid_read_timeout(reader, report.data(), report.size(), timeout);
        if (count < 0) {
            LOG_VERBOSE_W(L"[Logitech HID++] hid_read_timeout failed: " +
                          hidErrorString(reader));
            return std::nullopt;
        }
        if (count == 0) {
            continue;
        }
        if (responseMatches(report, count, request)) {
            return report;
        }
        if (errorMatches(report, count, request)) {
            LOG_VERBOSE_W(L"[Logitech HID++] device returned error 0x" +
                          std::to_wstring(report[4]));
            return std::nullopt;
        }
        // Logitech Options 和接收器异步事件共享输入集合；只接收 Software ID、
        // 槽位、Feature、函数均与本次请求相符的响应。
    }

    return std::nullopt;
}

std::optional<unsigned char> findFeature(hid_device *writer,
                                         hid_device *reader,
                                         unsigned char deviceIndex,
                                         unsigned short featureId)
{
    const Request request = buildRequest(
        deviceIndex,
        kRootFeatureIndex,
        kGetFeatureFunction,
        {
            static_cast<unsigned char>(featureId >> 8),
            static_cast<unsigned char>(featureId & 0xFF),
            0,
        });
    const auto response = sendCommand(writer, reader, request);
    if (!response || (*response)[4] == 0) {
        return std::nullopt;
    }
    return (*response)[4];
}

int approximatePercentageFromVoltage(int millivolts)
{
    // HID++ 0x1001 只提供电压。用常见单节锂电池放电曲线做分段线性估算；
    // 这是近似值，不冒充设备直接上报的精确百分比。
    struct Point { int millivolts; int percentage; };
    constexpr Point curve[] = {
        {4180, 100}, {4000, 80}, {3800, 50},
        {3700, 20}, {3500, 0},
    };

    if (millivolts >= curve[0].millivolts) return curve[0].percentage;
    if (millivolts <= curve[4].millivolts) return curve[4].percentage;

    for (std::size_t i = 0; i + 1 < std::size(curve); ++i) {
        const Point high = curve[i];
        const Point low = curve[i + 1];
        if (millivolts <= high.millivolts && millivolts >= low.millivolts) {
            const int voltageRange = high.millivolts - low.millivolts;
            const int percentageRange = high.percentage - low.percentage;
            return low.percentage +
                   (millivolts - low.millivolts) * percentageRange / voltageRange;
        }
    }
    return 0;
}

bool isChargingStatus(unsigned char status)
{
    // HID++ BatteryStatus：1=充电，2=接近充满，3=已充满，4=慢速充电。
    return status >= 0x01 && status <= 0x04;
}

std::wstring queryDeviceName(hid_device *writer,
                             hid_device *reader,
                             unsigned char deviceIndex)
{
    const auto featureIndex = findFeature(
        writer, reader, deviceIndex, kDeviceNameFeatureId);
    if (!featureIndex) {
        return {};
    }

    const auto lengthResponse = sendCommand(
        writer, reader,
        buildRequest(deviceIndex, *featureIndex, 0x00));
    if (!lengthResponse || (*lengthResponse)[4] == 0) {
        return {};
    }

    const std::size_t nameLength = std::min<std::size_t>((*lengthResponse)[4], 64);
    std::string name;
    name.reserve(nameLength);
    while (name.size() < nameLength) {
        const std::size_t previousSize = name.size();
        const auto fragment = sendCommand(
            writer, reader,
            buildRequest(
                deviceIndex, *featureIndex, 0x01,
                {static_cast<unsigned char>(name.size()), 0, 0}));
        if (!fragment) {
            return {};
        }

        const std::size_t remaining = nameLength - name.size();
        const std::size_t count = std::min<std::size_t>(remaining, 16);
        for (std::size_t i = 0; i < count && (*fragment)[4 + i] != 0; ++i) {
            name.push_back(static_cast<char>((*fragment)[4 + i]));
        }
        if (count == 0 || name.size() == previousSize) {
            return {};
        }
    }

    // Logitech 的 DEVICE_NAME 实际使用 ASCII/UTF-8；常见型号名均为 ASCII。
    return std::wstring(name.begin(), name.end());
}

std::optional<BatteryDevice> queryBattery(hid_device *writer,
                                          hid_device *reader,
                                          unsigned char deviceIndex,
                                          unsigned char featureIndex,
                                          unsigned short featureId,
                                          unsigned short receiverProductId,
                                          const std::wstring &name)
{
    const unsigned char function =
        featureId == kUnifiedBatteryFeatureId ? 0x01 : 0x00;
    const Request request = buildRequest(
        deviceIndex, featureIndex, function);
    const auto response = sendCommand(writer, reader, request);
    if (!response) {
        return std::nullopt;
    }

    int percentage = -1;
    unsigned char batteryStatus = 0;
    unsigned char externalPower = 0;

    if (featureId == kUnifiedBatteryFeatureId) {
        // 0x1004 GetStatus：[4] 精确百分比（0 表示仅支持档位），[5] 档位，
        // [6] BatteryStatus，[7] ExternalPowerStatus。
        percentage = (*response)[4];
        if (percentage == 0) {
            switch ((*response)[5]) {
            case 8: percentage = 90; break;
            case 4: percentage = 50; break;
            case 2: percentage = 20; break;
            case 1: percentage = 5; break;
            default: percentage = 0; break;
            }
        }
        batteryStatus = (*response)[6];
        externalPower = (*response)[7];
    } else if (featureId == kBatteryStatusFeatureId) {
        // 0x1000 GetStatus：[4] 当前百分比，[5] 下一阈值，[6] BatteryStatus。
        percentage = (*response)[4];
        batteryStatus = (*response)[6];
    } else if (featureId == kBatteryVoltageFeatureId) {
        // 0x1001 GetStatus：[4..5] 电压（mV，大端），[6] 充电标志。
        const int millivolts = ((*response)[4] << 8) | (*response)[5];
        const unsigned char flags = (*response)[6];
        percentage = approximatePercentageFromVoltage(millivolts);
        batteryStatus = (flags & 0x80) ? 0x01 : 0x00;
    }

    if (percentage < 0 || percentage > 100) {
        LOG_WARN_W(L"[Logitech HID++] invalid battery percentage: " +
                   std::to_wstring(percentage));
        return std::nullopt;
    }

    BatteryDevice device;
    device.id = L"logitech-hidpp:" + std::to_wstring(receiverProductId) + L":slot" +
                std::to_wstring(deviceIndex);
    device.name = name.empty() ? L"Logitech HID++ Device" : name;
    device.type = BatteryDevice::Type::Hid;
    device.percentage = percentage;
    device.level = levelFromPercentage(percentage);
    device.charging = isChargingStatus(batteryStatus);
    // 充电线接入时鼠标仍通过 Bolt 上报电量。BatteryDevice::wired 在 UI 中
    // 代表“只显示有线、隐藏百分比”，这里不能把 externalPower 直接映射为 wired，
    // 否则抓包确认的 70% + 充电状态会被错误显示成单纯的“有线”。
    device.wired = false;
    device.connected = true;

    LOG_VERBOSE_W(L"[Logitech HID++] " + device.name + L" slot=" +
                  std::to_wstring(deviceIndex) + L" battery=" +
                  std::to_wstring(percentage) + L"% status=" +
                  std::to_wstring(batteryStatus) + L" feature=" +
                  std::to_wstring(featureId) + L" externalPower=" +
                  std::to_wstring(externalPower));
    return device;
}

std::optional<HidCollectionPaths> findCollections(hid_device_info *head,
                                                  int preferredInterface)
{
    const hid_device_info *preferredOutput = nullptr;
    const hid_device_info *preferredInput = nullptr;
    const hid_device_info *fallbackOutput = nullptr;
    const hid_device_info *fallbackInput = nullptr;

    for (const hid_device_info *cur = head; cur; cur = cur->next) {
        if (!cur->path || cur->usage_page != kUsagePage) {
            continue;
        }
        if (cur->usage == kOutputUsage) {
            if (!fallbackOutput) fallbackOutput = cur;
            if (cur->interface_number == preferredInterface) preferredOutput = cur;
        } else if (cur->usage == kInputUsage) {
            if (!fallbackInput) fallbackInput = cur;
            if (cur->interface_number == preferredInterface) preferredInput = cur;
        }
    }

    const hid_device_info *output = preferredOutput ? preferredOutput : fallbackOutput;
    const hid_device_info *input = preferredInput ? preferredInput : fallbackInput;
    if (!output || !input || output->interface_number != input->interface_number) {
        return std::nullopt;
    }

    return HidCollectionPaths{output->path, input->path};
}
} // namespace

std::wstring LogitechHidProvider::displayName() const
{
    return L"HID";
}

std::vector<BatteryDevice> LogitechHidProvider::readDevices()
{
    std::lock_guard<std::recursive_mutex> hidLock(hidApiMutex());
    std::vector<BatteryDevice> devices;

    if (hid_init() != 0) {
        LOG_ERR("[Logitech HID++] hid_init failed");
        return devices;
    }

    const ReceiverEntry *activeReceiver = nullptr;
    std::optional<HidCollectionPaths> paths;
    for (const ReceiverEntry &entry : kReceiverEntries) {
        hid_device_info *head = hid_enumerate(entry.vendorId, entry.productId);
        const auto candidatePaths = findCollections(head, entry.interfaceNumber);
        hid_free_enumeration(head);
        if (candidatePaths) {
            activeReceiver = &entry;
            paths = candidatePaths;
            break;
        }
    }

    if (!paths) {
        m_outputPath.clear();
        m_inputPath.clear();
        m_receiverProductId = 0;
        m_deviceCaches.clear();
        return devices;
    }
    if (!activeReceiver) {
        return devices;
    }
    LOG_VERBOSE_W(L"[Logitech HID++] selected " +
                  std::wstring(activeReceiver->name));

    // 拔插后 HID path 会改变，此时旧槽位/Feature 缓存不再可信。
    if (m_outputPath != paths->output || m_inputPath != paths->input ||
        m_receiverProductId != activeReceiver->productId) {
        m_outputPath = paths->output;
        m_inputPath = paths->input;
        m_receiverProductId = activeReceiver->productId;
        m_deviceCaches.clear();
    }

    hid_device *writer = hid_open_path(m_outputPath.c_str());
    hid_device *reader = hid_open_path(m_inputPath.c_str());
    HidHandles handles(writer, reader);
    if (!writer || !reader) {
        LOG_WARN_W(L"[Logitech HID++] failed to open HID++ collections");
        return devices;
    }

    if (!m_deviceCaches.empty()) {
        for (const DeviceCache &cache : m_deviceCaches) {
            if (auto result = queryBattery(
                    writer, reader,
                    static_cast<unsigned char>(cache.deviceIndex),
                    cache.featureIndex,
                    cache.featureId,
                    m_receiverProductId,
                    cache.name)) {
                devices.push_back(std::move(*result));
            }
        }
        return devices;
    }

    // 接收器支持一个或多个配对槽位。优先探测新式 Unified Battery，再回退到
    // Battery Status 和 Battery Voltage。每个能成功返回电量的在线槽位分别缓存。
    constexpr unsigned short batteryFeatures[] = {
        kUnifiedBatteryFeatureId,
        kBatteryStatusFeatureId,
        kBatteryVoltageFeatureId,
    };
    for (int index = 1; index <= activeReceiver->maxDevices; ++index) {
        const auto deviceIndex = static_cast<unsigned char>(index);
        for (const unsigned short featureId : batteryFeatures) {
            const auto featureIndex = findFeature(
                writer, reader, deviceIndex, featureId);
            if (!featureIndex) {
                continue;
            }

            std::wstring name = queryDeviceName(writer, reader, deviceIndex);
            if (name.empty()) {
                name = L"Logitech HID++ Device";
            }
            if (auto result = queryBattery(
                    writer, reader, deviceIndex, *featureIndex, featureId,
                    activeReceiver->productId, name)) {
                m_deviceCaches.push_back(DeviceCache{
                    deviceIndex,
                    *featureIndex,
                    featureId,
                    name,
                });
                devices.push_back(std::move(*result));
                break;
            }
        }
    }

    LOG_VERBOSE_W(L"[Logitech HID++] readDevices total = " +
                  std::to_wstring(devices.size()));
    return devices;
}
