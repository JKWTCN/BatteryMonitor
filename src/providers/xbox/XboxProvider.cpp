#include "XboxProvider.h"
#include "util/Logger.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cwctype>
#include <set>

#include <windows.h>
#include <XInput.h>

#pragma comment(lib, "XInput.lib")

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Power.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.System.Power.h>
#include <winrt/base.h>

using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Devices::Power;
using namespace winrt::Windows::Gaming::Input;
using BatteryStatus = winrt::Windows::System::Power::BatteryStatus;

namespace
{
// 精确百分比 -> 离散档位（与其它 provider 一致）。
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

// XInput 离散电量状态直接映射到统一档位，不做百分比换算。
BatteryLevel levelFromXInput(BYTE batteryLevel)
{
    switch (batteryLevel) {
    case BATTERY_LEVEL_FULL:
        return BatteryLevel::Full;
    case BATTERY_LEVEL_MEDIUM:
        return BatteryLevel::Medium;
    case BATTERY_LEVEL_LOW:
        return BatteryLevel::Low;
    case BATTERY_LEVEL_EMPTY:
        return BatteryLevel::Empty;
    default:
        return BatteryLevel::Unknown;
    }
}

// 微软手柄 VendorId = 0x045E = 1118。
constexpr uint16_t kMicrosoftVendorId = 1118;

// DEVPKEY_Device_Battery 系列属性键（简写形式）。
//   "{104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2"  -> 电量百分比 (0-100)
//   "{104EA319-6EE2-4701-BD47-8DDBF425BBE5} 13" -> 电池状态字符串
constexpr const wchar_t *kBatteryPercentProp = L"{104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2";
constexpr const wchar_t *kBatteryStatusProp = L"{104EA319-6EE2-4701-BD47-8DDBF425BBE5} 13";

// 把属性中的数值（可能为 byte/short/int/long/uint/ulong/string）转成 int 百分比。
int propertyToInt(const winrt::Windows::Foundation::IInspectable &v)
{
    if (!v) {
        return -1;
    }
    try {
        // 装箱数值类型优先走 IReference<T>。
        try { return v.as<winrt::Windows::Foundation::IReference<int8_t>>().Value(); } catch (...) {}
        try { return v.as<winrt::Windows::Foundation::IReference<int16_t>>().Value(); } catch (...) {}
        try { return v.as<winrt::Windows::Foundation::IReference<int32_t>>().Value(); } catch (...) {}
        try { return v.as<winrt::Windows::Foundation::IReference<uint8_t>>().Value(); } catch (...) {}
        try { return v.as<winrt::Windows::Foundation::IReference<uint16_t>>().Value(); } catch (...) {}
        try { return v.as<winrt::Windows::Foundation::IReference<uint32_t>>().Value(); } catch (...) {}
        // 字符串：尝试解析。
        const auto s = v.as<winrt::Windows::Foundation::IReference<winrt::hstring>>().Value();
        try { return std::stoi(std::wstring(s)); } catch (...) { return -1; }
    } catch (...) {
        return -1;
    }
}

// 读取属性表中的字符串属性（如电池状态）。
std::wstring propertyToString(const winrt::Windows::Foundation::IInspectable &v)
{
    if (!v) {
        return {};
    }
    try {
        return std::wstring(v.as<winrt::Windows::Foundation::IReference<winrt::hstring>>().Value());
    } catch (...) {
        return {};
    }
}

// 取 DeviceInformation.Properties 中的属性值。
winrt::Windows::Foundation::IInspectable lookupProp(
    const DeviceInformation &info, const winrt::hstring &key)
{
    const auto props = info.Properties();
    if (!props || props.Size() == 0 || !props.HasKey(key)) {
        return nullptr;
    }
    return props.Lookup(key);
}

// 通过 RawGameController.TryGetBatteryReport() 读微软手柄精确百分比。
// 命中的设备 id 写入 filled，避免与 XInput 路径重复。
void readRawGameControllers(std::vector<BatteryDevice> &devices,
                            std::set<std::wstring> &filled)
{
    for (const auto &rc : RawGameController::RawGameControllers()) {
        if (rc.HardwareVendorId() != kMicrosoftVendorId) {
            continue;
        }
        BatteryReport report = nullptr;
        try {
            report = rc.TryGetBatteryReport();
        } catch (...) {
            continue;
        }
        if (!report) {
            continue;
        }
        int pct = -1;
        const auto remaining = report.RemainingCapacityInMilliwattHours();
        const auto full = report.FullChargeCapacityInMilliwattHours();
        if (remaining && full && full.Value() > 0) {
            pct = static_cast<int>(std::round(
                remaining.Value() * 100.0 / full.Value()));
            pct = (std::max)(0, (std::min)(100, pct));
        }
        const auto status = report.Status();
        const bool charging = (status == BatteryStatus::Charging);
        const std::wstring id = L"xbox:raw:" + std::wstring(rc.NonRoamableId());
        if (filled.count(id)) {
            continue;
        }
        std::wstring name = std::wstring(rc.DisplayName());
        if (name.empty()) {
            name = L"Xbox Controller";
        }

        BatteryDevice device;
        device.id = id;
        device.name = name;
        device.type = BatteryDevice::Type::Xbox;
        device.percentage = pct;
        device.level = levelFromPercentage(pct);
        device.connected = true;
        if (status == BatteryStatus::NotPresent || charging) {
            device.wired = charging;
        }
        devices.push_back(std::move(device));
        filled.insert(id);
        LOG_VERBOSE_W(L"[Xbox] RawGameController " + name + L" = " +
              std::to_wstring(pct) + L"% charging=" + (charging ? L"1" : L"0"));
    }
}

// 通过设备属性键读经典蓝牙手柄(BTH)/XUSB 等的电量百分比。
//
// 注意：{104EA319-...} 属性键不只手柄有，普通蓝牙耳机（HFP/A2DP 端点）也有。
// 为避免把耳机误归到 Xbox，本函数只接受名称看起来像手柄的设备。
void readDevicePropertyBatteries(std::vector<BatteryDevice> &devices,
                                 std::set<std::wstring> &filled)
{
    const winrt::hstring percentProp = kBatteryPercentProp;
    const winrt::hstring statusProp = kBatteryStatusProp;
    const winrt::hstring connectedProp = L"System.Devices.Connected";
    std::vector<winrt::hstring> extra = {percentProp, statusProp, connectedProp};
    const auto iterable =
        winrt::multi_threaded_vector<winrt::hstring>(std::move(extra))
            .as<winrt::Windows::Foundation::Collections::IIterable<winrt::hstring>>();
    DeviceInformationCollection found = nullptr;
    try {
        found = DeviceInformation::FindAllAsync(L"", iterable,
            DeviceInformationKind::Device).get();
    } catch (const winrt::hresult_error &e) {
        LOG_ERR_W(L"[Xbox] FindAllAsync(device props) failed: " + std::wstring(e.message()));
        return;
    }
    if (!found) {
        return;
    }
    for (const auto &info : found) {
        std::wstring name = std::wstring(info.Name());
        if (name.empty()) {
            continue;
        }
        // 只接受看起来像手柄的设备名（xinput / XUSB / xbox），避免误收耳机/鼠标/键盘。
        const auto lower = [&name]() {
            std::wstring n = name;
            std::transform(n.begin(), n.end(), n.begin(), ::towlower);
            return n;
        }();
        const bool looksLikeController =
            lower.find(L"xinput") != std::wstring::npos ||
            lower.find(L"xusb") != std::wstring::npos ||
            lower.find(L"xbox") != std::wstring::npos;
        if (!looksLikeController) {
            continue;
        }
        const int pct = propertyToInt(lookupProp(info, percentProp));
        if (pct < 0 || pct > 100) {
            continue;
        }
        const auto statusStr = propertyToString(lookupProp(info, statusProp));
        const bool charging = (statusStr == L"Charging");
        const std::wstring id = std::wstring(info.Id());
        if (filled.count(id)) {
            continue;
        }
        BatteryDevice device;
        device.id = id;
        device.name = name;
        device.type = BatteryDevice::Type::Xbox;
        device.percentage = pct;
        device.level = levelFromPercentage(pct);
        device.connected = true;
        device.wired = charging;
        devices.push_back(std::move(device));
        filled.insert(id);
        LOG_VERBOSE_W(L"[Xbox] DeviceProp " + name + L" = " + std::to_wstring(pct) +
              L"% status=\"" + statusStr + L"\"");
    }
}
} // namespace

std::wstring XboxProvider::displayName() const
{
    return L"Xbox";
}

std::vector<BatteryDevice> XboxProvider::readDevices()
{
    std::vector<BatteryDevice> devices;
    // filled 记录已精确命中的设备 ID（RawGameController / 设备属性键路径），
    // 用于在 XInput 兜底时去重——精确百分比优先于离散档位。
    std::set<std::wstring> filled;

    // —— 精确百分比路径（优先）——
    // 这两条路径需要 WinRT，惰性初始化 apartment（在 worker 线程）。
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        readRawGameControllers(devices, filled);
        readDevicePropertyBatteries(devices, filled);
    } catch (const winrt::hresult_error &e) {
        LOG_ERR_W(L"[Xbox] WinRT path failed: " + std::wstring(e.message()));
    } catch (...) {
        LOG_ERR("[Xbox] WinRT path failed (unknown)");
    }

    // —— XInput 兜底路径（离散档位）——
    constexpr DWORD kMaxUsers = 4;
    for (DWORD userIndex = 0; userIndex < kMaxUsers; ++userIndex) {
        XINPUT_STATE state{};
        // 仅当手柄在线时才有读电量的意义。
        if (XInputGetState(userIndex, &state) != ERROR_SUCCESS) {
            continue;
        }

        XINPUT_BATTERY_INFORMATION batteryInfo{};
        if (XInputGetBatteryInformation(userIndex, BATTERY_DEVTYPE_GAMEPAD, &batteryInfo)
            != ERROR_SUCCESS) {
            continue;
        }

        BatteryDevice device;
        device.id = L"XInput_" + std::to_wstring(userIndex);
        device.name = L"Xbox Controller " + std::to_wstring(userIndex + 1);
        device.type = BatteryDevice::Type::Xbox;
        device.percentage = -1; // XInput 无精确百分比。
        device.connected = true;

        switch (batteryInfo.BatteryType) {
        case BATTERY_TYPE_WIRED:
            // XInput 的“有线”描述的是 Windows 看到的连接。部分 2.4 GHz
            // dongle 也会伪装成 USB 有线手柄，但仍通过 BatteryLevel 上报
            // 无线手柄电量，因此不能在这里丢弃档位。
            device.wired = true;
            device.level = levelFromXInput(batteryInfo.BatteryLevel);
            break;
        case BATTERY_TYPE_DISCONNECTED:
            // 槽位在线但电池断开：视为未连接电量。
            device.wired = false;
            device.level = BatteryLevel::Unknown;
            break;
        case BATTERY_TYPE_ALKALINE:
        case BATTERY_TYPE_NIMH:
        case BATTERY_TYPE_UNKNOWN:
        default:
            device.wired = false;
            device.level = levelFromXInput(batteryInfo.BatteryLevel);
            break;
        }

        devices.push_back(std::move(device));
    }

    return devices;
}
