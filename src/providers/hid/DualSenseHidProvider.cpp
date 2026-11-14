#include "DualSenseHidProvider.h"
#include "HidApiLock.h"
#include "util/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <hidapi.h>

namespace
{
// Sony DualSense / DualSense Edge。其它 Sony 手柄（如 DualShock 4）协议不同，
// 不在这个 Provider 的范围内。
constexpr uint16_t kSonyVendorId = 0x054C;
constexpr uint16_t kDualSenseProductId = 0x0CE6;
constexpr uint16_t kDualSenseEdgeProductId = 0x0DF2;

// 顶级 collection：Generic Desktop / Game Pad。
// hidapi 的 Windows 后端用 HidP_GetCaps 填 usage_page / usage，
// 对应顶层 collection 的 UsagePage / Usage。
constexpr uint16_t kGenericDesktopUsagePage = 0x0001;
constexpr uint16_t kGamePadUsage = 0x0005;

// 输入报告。长度均包含首字节 Report ID。
constexpr uint8_t kUsbReportId = 0x01;
constexpr uint8_t kBluetoothReportId = 0x31;
constexpr size_t kUsbReportSize = 64;
constexpr size_t kBluetoothReportSize = 78;
constexpr size_t kUsbStatusOffset = 53;
constexpr size_t kBluetoothStatusOffset = 54;
constexpr size_t kCrcSize = 4;
constexpr uint32_t kCrc32Polynomial = 0xEDB88320u;
constexpr uint8_t kBluetoothCrcSeed = 0xA1;

// 多留 1 字节，兼容部分 hidapi 后端在编号报告前补 0 的行为。
constexpr size_t kReadBufferSize = kBluetoothReportSize + 1;
constexpr int kMaxReadAttempts = 8;
constexpr int kReadTimeoutMs = 1200;

bool isDualSenseProduct(uint16_t productId)
{
    return productId == kDualSenseProductId ||
           productId == kDualSenseEdgeProductId;
}

std::wstring modelName(uint16_t productId)
{
    return productId == kDualSenseEdgeProductId
               ? L"DualSense Edge (PS5)"
               : L"DualSense (PS5)";
}

std::wstring pathToWString(const char *path)
{
    if (!path) {
        return {};
    }
    return std::wstring(path, path + std::strlen(path));
}

// hid_error 返回的指针归 hidapi 所有，只取一次并立即拷贝。
std::wstring hidErrorString(hid_device *dev)
{
    if (!dev) {
        return L"unavailable";
    }
    const wchar_t *message = hid_error(dev);
    return std::wstring(message ? message : L"unknown");
}

std::wstring hexByte(uint8_t value)
{
    std::wostringstream ss;
    ss << L"0x" << std::uppercase << std::hex << std::setw(2)
       << std::setfill(L'0') << static_cast<unsigned>(value);
    return ss.str();
}

// 与项目内其它 HID Provider 保持一致的档位阈值。
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

uint32_t updateCrc32(uint32_t crc, uint8_t value)
{
    crc ^= value;
    for (int bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1) ^ ((crc & 1u) != 0 ? kCrc32Polynomial : 0u);
    }
    return crc;
}

uint32_t readLittleEndian32(const unsigned char *data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

struct Ds5Reading
{
    // percentage 是区间下界；percentageUpper 是开区间上界，
    // -1 表示精确读数（100% 满电档）。
    int percentage = -1;
    int percentageUpper = -1;
    bool charging = false;
    bool bluetooth = false;
};

// 解析一帧输入报告。reason 用于诊断（写入 Verbose 日志），不解释任何可疑数据。
std::optional<Ds5Reading> decodeReport(const unsigned char *data,
                                       size_t length,
                                       std::wstring &reason)
{
    if (length == 0) {
        reason = L"empty input report";
        return std::nullopt;
    }

    bool bluetooth = false;
    size_t expectedSize = 0;
    if (data[0] == kUsbReportId) {
        expectedSize = kUsbReportSize;
    } else if (data[0] == kBluetoothReportId) {
        bluetooth = true;
        expectedSize = kBluetoothReportSize;
    } else {
        reason = L"not a DS5 full battery report (id=" +
                 hexByte(data[0]) + L")";
        return std::nullopt;
    }

    if (length < expectedSize) {
        reason = L"short " + std::wstring(bluetooth ? L"Bluetooth" : L"USB") +
                 L" report (got " + std::to_wstring(length) + L", need " +
                 std::to_wstring(expectedSize) + L")";
        return std::nullopt;
    }

    // 蓝牙完整报告的末尾 4 字节是 CRC32(0xA1 || report[0 .. size-5]) 的取反。
    // 校验不通过就丢弃整帧，绝不把噪声字节当成电量。
    if (bluetooth) {
        uint32_t crc = updateCrc32(0xFFFFFFFFu, kBluetoothCrcSeed);
        for (size_t i = 0; i < expectedSize - kCrcSize; ++i) {
            crc = updateCrc32(crc, data[i]);
        }
        const uint32_t actual = readLittleEndian32(data + expectedSize - kCrcSize);
        if (~crc != actual) {
            reason = L"Bluetooth CRC32 mismatch";
            return std::nullopt;
        }
    }

    const uint8_t status = data[bluetooth ? kBluetoothStatusOffset
                                          : kUsbStatusOffset];
    const int level = status & 0x0F;
    const int chargeState = status >> 4;
    if (chargeState > 2) {
        // 充电状态异常时不能把它解释成「低电量」。
        reason = L"unknown charging state " + std::to_wstring(chargeState);
        return std::nullopt;
    }
    if (level > 10) {
        reason = L"invalid battery level " + std::to_wstring(level);
        return std::nullopt;
    }

    // DS5 以 10% 分档上报，[level*10, level*10+10) 才是真实电量区间。
    // 只写下界会让界面把「80%~90%」显示成看似精确的 80%，因此同时给出
    // 开区间上界；「已充满」(chargeState==2) 与最高档 (level==10) 是
    // 确定的 100%，不设区间。
    Ds5Reading reading;
    if (chargeState == 2 || level == 10) {
        reading.percentage = 100;
    } else {
        reading.percentage = level * 10;
        reading.percentageUpper = reading.percentage + 10;
    }
    reading.charging = chargeState == 1;
    reading.bluetooth = bluetooth;
    return reading;
}

struct ReadOutcome
{
    std::optional<Ds5Reading> reading;
    std::wstring reason;
    int attempts = 0;
};

// DS5 持续上报输入报告，但队列里可能先夹着其它 Report ID / 简化的蓝牙报告，
// 因此在总超时内最多读 8 帧，直到拿到一帧完整且校验通过的报告。
// 不发送任何模式切换命令：蓝牙简化报告模式由设备 / Windows 决定，这里不干预。
ReadOutcome readBatteryReport(hid_device *dev)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kReadTimeoutMs);
    ReadOutcome outcome;
    for (int attempt = 0; attempt < kMaxReadAttempts; ++attempt) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            outcome.reason = L"read timed out before a full report arrived";
            break;
        }
        const int timeoutMs = std::max(1, static_cast<int>(remaining.count()));

        std::vector<unsigned char> buffer(kReadBufferSize, 0);
        const int count = hid_read_timeout(dev, buffer.data(), buffer.size(),
                                           timeoutMs);
        if (count < 0) {
            outcome.reason = L"hid_read_timeout failed: " + hidErrorString(dev);
            break;
        }
        if (count == 0) {
            outcome.reason = L"no input report within " +
                             std::to_wstring(timeoutMs) + L" ms";
            break;
        }
        ++outcome.attempts;

        // 兼容在编号报告前补 0 的 hidapi 后端：去掉这个填充字节。
        const unsigned char *report = buffer.data();
        size_t reportSize = static_cast<size_t>(count);
        if (reportSize > 1 && report[0] == 0x00 &&
            (report[1] == kUsbReportId || report[1] == kBluetoothReportId)) {
            ++report;
            --reportSize;
        }

        std::wstring reason;
        auto reading = decodeReport(report, reportSize, reason);
        if (reading) {
            outcome.reading = std::move(reading);
            return outcome;
        }
        outcome.reason = std::move(reason);
        LOG_VERBOSE_W(L"[DualSense] ignored report " +
                      std::to_wstring(attempt + 1) + L": " + outcome.reason);
    }
    return outcome;
}

struct Candidate
{
    const hid_device_info *info = nullptr;
    std::wstring path;
    std::wstring id;
    // true  = 标准 Game Pad collection（优先尝试）
    // false = 驱动未填 usage 的接口，仅在标准 collection 全部失败时兜底
    bool gamePad = false;
};

// 设备 id 优先用序列号，使同一手柄在 USB / 蓝牙切换、或出现多个 HID
// collection 时仍合并为一行，并让别名 / 提醒 / 历史 / 粘性缓存保持稳定。
std::wstring deviceId(const hid_device_info *info, const std::wstring &path)
{
    std::wstring serial = info->serial_number
                              ? std::wstring(info->serial_number)
                              : std::wstring();
    // 部分固件返回全空白序列号，视为无序列号。
    if (serial.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
        serial.clear();
    }
    const std::wstring identity = serial.empty() ? path : L"sn:" + serial;
    return L"dualsense:" + std::to_wstring(info->product_id) + L":" + identity;
}

BatteryDevice makeDevice(const Candidate &candidate, const Ds5Reading &reading)
{
    BatteryDevice device;
    device.id = candidate.id;
    device.name = modelName(candidate.info->product_id) +
                  (reading.bluetooth ? L" (Bluetooth)" : L" (USB)");
    device.type = BatteryDevice::Type::Hid;
    device.percentage = reading.percentage;
    device.percentageUpper = reading.percentageUpper;
    device.level = levelFromPercentage(reading.percentage);
    device.charging = reading.charging;
    device.connected = true;
    // 刻意不设 wired：USB 连接的 DS5 依然有电池，且常常正在充电。
    // UI 会把 wired=true 的非 Xbox 设备整行显示为「Wired」并隐藏电量，
    // 那样反而看不到手柄电量。
    device.wired = false;
    return device;
}
} // namespace

std::wstring DualSenseHidProvider::displayName() const
{
    return L"DualSense HID";
}

std::vector<BatteryDevice> DualSenseHidProvider::readDevices()
{
    // hidapi 全局状态由所有 HID Provider 共享，必须串行访问。
    std::lock_guard<std::recursive_mutex> hidLock(hidApiMutex());
    std::vector<BatteryDevice> devices;

    if (hid_init() != 0) {
        LOG_ERR("[DualSense] hid_init failed");
        return devices;
    }

    hid_device_info *head = hid_enumerate(kSonyVendorId, 0);
    std::vector<Candidate> candidates;
    for (hid_device_info *cur = head; cur; cur = cur->next) {
        if (!isDualSenseProduct(cur->product_id) || !cur->path) {
            continue;
        }
        Candidate candidate;
        candidate.info = cur;
        candidate.path = pathToWString(cur->path);
        candidate.id = deviceId(cur, candidate.path);
        if (cur->usage_page == kGenericDesktopUsagePage &&
            cur->usage == kGamePadUsage) {
            candidate.gamePad = true;
        } else if (cur->usage_page == 0 && cur->usage == 0) {
            // 少数驱动不上报 usage。仍严格校验 Report ID / 长度 / CRC，
            // 不会把其它设备误认成 DS5。
            candidate.gamePad = false;
        } else {
            // 键盘、鼠标、音频、厂商控制等 collection 一律不打开。
            continue;
        }
        candidates.push_back(std::move(candidate));
    }

    std::set<std::wstring> emittedIds;
    std::wstring lastFailure;
    int tried = 0;

    // 两轮：先只试标准 Game Pad collection；一个都没读到时，才试
    // usage 未知的接口兜底，避免每轮无谓打开多个无关 HID 接口。
    for (int pass = 0; pass < 2 && devices.empty(); ++pass) {
        for (const auto &candidate : candidates) {
            if (candidate.gamePad != (pass == 0)) {
                continue;
            }
            if (emittedIds.count(candidate.id) != 0) {
                continue;
            }
            ++tried;

            hid_device *dev = hid_open_path(candidate.info->path);
            if (!dev) {
                lastFailure = L"cannot open HID interface (may be claimed by "
                              L"another program): " + candidate.path;
                LOG_VERBOSE_W(L"[DualSense] " + lastFailure);
                continue;
            }
            const ReadOutcome outcome = readBatteryReport(dev);
            hid_close(dev);

            if (!outcome.reading) {
                lastFailure = outcome.reason;
                LOG_VERBOSE_W(L"[DualSense] " + candidate.path + L": " +
                              lastFailure);
                continue;
            }

            const Ds5Reading &reading = *outcome.reading;
            emittedIds.insert(candidate.id);
            devices.push_back(makeDevice(candidate, reading));
            const std::wstring batteryLog =
                reading.percentageUpper > reading.percentage
                    ? std::to_wstring(reading.percentage) + L"~" +
                          std::to_wstring(reading.percentageUpper) + L"%"
                    : std::to_wstring(reading.percentage) + L"%";
            LOG_VERBOSE_W(L"[DualSense] " + modelName(candidate.info->product_id) +
                          L" battery=" + batteryLog +
                          L" charging=" +
                          (reading.charging ? L"1" : L"0") +
                          L" transport=" +
                          (reading.bluetooth ? L"Bluetooth 0x31" : L"USB 0x01") +
                          L" reports=" + std::to_wstring(outcome.attempts));
        }
    }

    hid_free_enumeration(head);

    if (!candidates.empty()) {
        if (devices.empty() && m_lastReadSucceeded) {
            // 默认每 10s 轮询一次，失败原因只记一次，避免日志刷屏。
            LOG_WARN_W(L"[DualSense] controller found but no full battery "
                       L"report could be read: " + lastFailure +
                       L". Bluetooth simplified reports carry no battery; the "
                       L"device may also be asleep or opened exclusively by "
                       L"another program. Keeping the last cached reading.");
        }
        m_lastReadSucceeded = !devices.empty();
    } else {
        m_lastReadSucceeded = true;
    }

    return devices;
}
