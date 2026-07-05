#include "BluetoothProvider.h"
#include "util/Logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cwctype>
#include <map>
#include <optional>
#include <set>
#include <string>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Power.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

namespace
{
// 引入根命名空间最省事：hstring 在 winrt 根命名空间，
// IInspectable / IPropertyValue / PropertyType 在 winrt::Windows::Foundation。
using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Devices::Power;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;

// winrt::hstring -> std::wstring。
std::wstring toWString(const hstring &s)
{
    return std::wstring(s.c_str());
}

// 整数 -> std::wstring（日志用）。
std::wstring toWString(int v)
{
    return std::to_wstring(v);
}

// 计时辅助：测量某段 WinRT 异步操作耗时（毫秒），用于定位卡顿。
using SteadyClock = std::chrono::steady_clock;
long long elapsedMs(SteadyClock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        SteadyClock::now() - start).count();
}

// 由精确百分比派生到统一档位，仅用于着色 / 排序 / 低电量提醒。
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

// ===== Power::Battery 路径（主力：能读到真实电量的路径）=====
//
// Windows 通过 Power::Battery API 暴露所有"电池设备"，包括蓝牙耳机/手柄等。
// 该 API 返回 RemainingCapacity / FullChargeCapacity，可精确算出百分比。
// 副作用：会混入笔记本电池等系统电池（ACPI），需用下面的过滤排除。

std::wstring toLower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return s;
}

// 是否是系统电池（笔记本电源等），需排除。
bool looksLikeSystemBattery(const std::wstring &name, const std::wstring &id)
{
    const std::wstring ln = toLower(name);
    const std::wstring li = toLower(id);
    return ln.find(L"acpi") != std::wstring::npos ||
           ln.find(L"control method battery") != std::wstring::npos ||
           ln.find(L"system battery") != std::wstring::npos ||
           li.find(L"acpi") != std::wstring::npos;
}

// 是否看起来像蓝牙设备（用于过滤掉非蓝牙的电池设备）。
bool looksLikeBluetoothDevice(const std::wstring &name, const std::wstring &id)
{
    const std::wstring ln = toLower(name);
    const std::wstring li = toLower(id);
    return ln.find(L"bluetooth") != std::wstring::npos ||
           ln.find(L"ble") != std::wstring::npos ||
           li.find(L"bluetooth") != std::wstring::npos ||
           li.find(L"bth") != std::wstring::npos ||
           li.find(L"ble") != std::wstring::npos;
}

// 从 Power::Battery 读百分比：RemainingCapacity / FullChargeCapacity。
std::optional<int> readPercentage(const Battery &battery)
{
    const auto report = battery.GetReport();
    const auto remaining = report.RemainingCapacityInMilliwattHours();
    const auto full = report.FullChargeCapacityInMilliwattHours();
    if (!remaining || !full || full.Value() <= 0) {
        return std::nullopt;
    }
    const int percentage = static_cast<int>(std::round(
        (remaining.Value() * 100.0) / full.Value()));
    return (std::max)(0, (std::min)(100, percentage));
}

// 枚举 Power::Battery 设备，读取电量。
// 收集所有非系统电池的设备；调用方根据是否有"蓝牙样"设备决定是否再过滤。
void readPowerBatteryDevices(std::vector<BatteryDevice> &devices,
                             std::set<std::wstring> &seenIds,
                             bool &hasBluetoothLikeDevice)
{
    const auto selector = Battery::GetDeviceSelector();
    const auto pbStart = SteadyClock::now();
    const auto found = DeviceInformation::FindAllAsync(selector).get();
    LOG_W(L"[Bluetooth] Power::Battery FindAllAsync took " +
          toWString(static_cast<int>(elapsedMs(pbStart))) + L"ms, found " +
          toWString(static_cast<int>(found.Size())) + L" battery devices");

    for (const auto &device : found) {
        const std::wstring id = toWString(device.Id());
        std::wstring name = toWString(device.Name());

        LOG_W(L"[Bluetooth] Power::Battery device raw: name=\"" + name +
              L"\" id=\"" + id + L"\"");

        if (id.empty()) {
            LOG_W(L"[Bluetooth] Power::Battery skipped: empty id");
            continue;
        }
        if (looksLikeSystemBattery(name, id)) {
            LOG_W(L"[Bluetooth] Power::Battery skipped: looks like system battery");
            continue;
        }
        if (name.empty()) {
            name = L"Bluetooth";
        }

        if (looksLikeBluetoothDevice(name, id)) {
            hasBluetoothLikeDevice = true;
        }

        const auto battery = Battery::FromIdAsync(device.Id()).get();
        if (!battery) {
            LOG_W(L"[Bluetooth] Power::Battery skipped: FromIdAsync returned null");
            continue;
        }
        const auto percentage = readPercentage(battery);
        if (!percentage) {
            LOG_W(L"[Bluetooth] Power::Battery skipped: readPercentage returned empty");
            continue;
        }

        if (seenIds.count(id)) {
            continue;
        }

        BatteryDevice entry;
        entry.id = id;
        entry.name = std::move(name);
        entry.type = BatteryDevice::Type::Bluetooth;
        entry.percentage = *percentage;
        entry.level = levelFromPercentage(*percentage);
        entry.connected = true;
        devices.push_back(std::move(entry));
        seenIds.insert(id);

        LOG_W(L"[Bluetooth] Power::Battery device: " + entry.name +
              L" = " + toWString(*percentage) + L"%");
    }
}

// 从单次 GATT 读结果解析电量百分比（0-100）。无效返回 nullopt。
std::optional<int> parseGattBatteryValue(const GattReadResult &readResult)
{
    if (readResult.Status() != GattCommunicationStatus::Success ||
        readResult.Value().Length() < 1) {
        return std::nullopt;
    }
    const auto reader = DataReader::FromBuffer(readResult.Value());
    const int percentage = reader.ReadByte();
    if (percentage < 0 || percentage > 100) {
        return std::nullopt;
    }
    return percentage;
}

// 从 GATT 特征值读出电量百分比（0-100），失败返回 nullopt。
//
// 先 Uncached（拿真值，多数耳机的 Cached 缓存为空/过期），
// 失败再回退到 Cached。这是当前“读不到”问题的最大修复点。
std::optional<int> readGattBatteryLevel(const GattCharacteristic &characteristic)
{
    // 1) Uncached 优先。
    try {
        const auto uncached = characteristic.ReadValueAsync(
            BluetoothCacheMode::Uncached).get();
        if (const auto v = parseGattBatteryValue(uncached)) {
            return v;
        }
    } catch (const winrt::hresult_error &) {
        // Uncached 抛错时静默回退到 Cached。
    }
    // 2) Cached 兜底。
    try {
        const auto cached = characteristic.ReadValueAsync(
            BluetoothCacheMode::Cached).get();
        return parseGattBatteryValue(cached);
    } catch (const winrt::hresult_error &) {
        return std::nullopt;
    }
}

// 取 Battery 服务：先 Cached，失败再 Uncached（同步阻塞等待）。
GattDeviceServicesResult getBatteryServices(const BluetoothLEDevice &device)
{
    auto cached = device.GetGattServicesForUuidAsync(
        GattServiceUuids::Battery(), BluetoothCacheMode::Cached).get();
    if (cached.Status() == GattCommunicationStatus::Success &&
        cached.Services().Size() > 0) {
        return cached;
    }
    try {
        return device.GetGattServicesForUuidAsync(
            GattServiceUuids::Battery(), BluetoothCacheMode::Uncached).get();
    } catch (const winrt::hresult_error &) {
        return cached; // Uncached 抛错时回退到 Cached 结果（可能为空）。
    }
}

// 取 BatteryLevel 特征值：先 Cached，失败再 Uncached（同步阻塞等待）。
GattCharacteristicsResult getBatteryLevelCharacteristic(const GattDeviceService &service)
{
    auto cached = service.GetCharacteristicsForUuidAsync(
        GattCharacteristicUuids::BatteryLevel(), BluetoothCacheMode::Cached).get();
    if (cached.Status() == GattCommunicationStatus::Success &&
        cached.Characteristics().Size() > 0) {
        return cached;
    }
    try {
        return service.GetCharacteristicsForUuidAsync(
            GattCharacteristicUuids::BatteryLevel(), BluetoothCacheMode::Uncached).get();
    } catch (const winrt::hresult_error &) {
        return cached;
    }
}

// 规范化设备 ID：优先用 System.Devices.ContainerId（GUID），拼成 container:<guid>，
// 便于跨 provider 去重；读不到则回退到 DeviceInformation.Id。
//
// ContainerId 在 WinRT 中以装箱的 winrt::guid（IReference<guid>）形式返回。
std::wstring canonicalId(const DeviceInformation &deviceInfo)
{
    const auto props = deviceInfo.Properties();
    if (props && props.Size() > 0) {
        const hstring key = L"System.Devices.ContainerId";
        if (props.HasKey(key)) {
            const auto v = props.Lookup(key);
            if (v) {
                try {
                    const auto ref =
                        v.as<winrt::Windows::Foundation::IReference<winrt::guid>>();
                    const auto g = ref.Value();
                    // 全零 GUID 视为无效。
                    if (g != winrt::guid{}) {
                        return L"container:" + toWString(winrt::to_hstring(g));
                    }
                } catch (...) {
                    // 某些设备该属性可能以字符串返回，回退到 Id。
                }
            }
        }
    }
    return toWString(deviceInfo.Id());
}

// 读取 DeviceInformation.Properties 中的布尔属性（如 System.Devices.Aep.IsConnected）。
// 属性缺失 / 非 bool 返回 fallback。
bool boolProperty(const DeviceInformation &deviceInfo,
                  const hstring &key, bool fallback = false)
{
    const auto props = deviceInfo.Properties();
    if (!props || props.Size() == 0 || !props.HasKey(key)) {
        return fallback;
    }
    const auto v = props.Lookup(key);
    if (!v) {
        return fallback;
    }
    // 属性值以装箱 bool（IReference<bool>）返回；Value() 取裸 bool。
    try {
        return v.as<winrt::Windows::Foundation::IReference<bool>>().Value();
    } catch (...) {
        try {
            return winrt::unbox_value<bool>(v);
        } catch (...) {
            return fallback;
        }
    }
}
} // namespace

std::wstring BluetoothProvider::displayName() const
{
    return L"Bluetooth";
}

std::vector<BatteryDevice> BluetoothProvider::readDevices()
{
    std::vector<BatteryDevice> devices;
    // 两条路径合并去重：
    //   1) Power::Battery（主力：能精确读到 RemainingCapacity/FullChargeCapacity）。
    //   2) GATT（BLE 标准 Battery Service 补充）。
    // 各路径设备 Id 形态不同，按各自 Id 去重。
    std::set<std::wstring> seenIds;
    bool hasBluetoothLikeDevice = false;

    try {
        // 在 worker 线程惰性初始化 WinRT apartment（multi_threaded，
        // 与 .get() 的同步等待配合，避免 STA 阻塞）。
        // 关键：Qt 的 QApplication 可能在主线程初始化了 STA（OleInitialize），
        // 但 worker 是独立线程，应有自己的 apartment。
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            LOG("[Bluetooth] apartment init OK (multi_threaded)");
        } catch (const winrt::hresult_error &e) {
            LOG_ERR_W(L"[Bluetooth] apartment init FAILED: " + toWString(e.message()));
        } catch (...) {
            LOG_ERR("[Bluetooth] apartment init FAILED (unknown)");
        }

        // 主力路径：Power::Battery（先收集到临时 vector，单独过滤后再合并，
        // 避免 GATT 读到的设备被蓝牙过滤误删——它们的 name/id 可能不含
        // “bluetooth/bth” 关键字）。
        std::vector<BatteryDevice> powerDevices;
        readPowerBatteryDevices(powerDevices, seenIds, hasBluetoothLikeDevice);

        // 关键过滤：如果枚举到了“蓝牙样”设备，则只保留蓝牙样的，
        // 排除 Power::Battery 里混入的其它非蓝牙电池设备（如 USB 电池等）。
        if (hasBluetoothLikeDevice) {
            powerDevices.erase(std::remove_if(powerDevices.begin(), powerDevices.end(),
                [](const BatteryDevice &d) {
                    return !looksLikeBluetoothDevice(d.name, d.id);
                }), powerDevices.end());
        }
        // 合并进主列表。
        for (auto &d : powerDevices) {
            devices.push_back(std::move(d));
        }

        // 辅助路径：GATT（BLE Battery Service）。
        //
        //   - 用全量选择器 BluetoothLEDevice.GetDeviceSelector() + 附加属性，
        //     再按 System.Devices.Aep.IsConnected 属性过滤连接状态
        //     （覆盖面比 GetDeviceSelectorFromConnectionStatus(Connected) 更广）。
        //   - 附加请求 System.Devices.ContainerId 用于规范化设备 ID（container:<guid>），
        //     便于与 Power::Battery / 其它 provider 去重。
        const auto gattStart = SteadyClock::now();
        const auto selector = BluetoothLEDevice::GetDeviceSelector();
        // FindAllAsync 需要 IIterable<hstring>，用 winrt::multi_threaded_vector 包装。
        // （这两个属性分别用于判断连接状态和规范化设备 ID。）
        std::vector<hstring> additionalPropsH = {
            hstring(L"System.Devices.Aep.IsConnected"),
            hstring(L"System.Devices.ContainerId"),
        };
        const auto iterableProps =
            winrt::multi_threaded_vector<hstring>(std::move(additionalPropsH))
                .as<winrt::Windows::Foundation::Collections::IIterable<hstring>>();
        const auto deviceInfos = DeviceInformation::FindAllAsync(
            selector, iterableProps).get();

        LOG_W(L"[Bluetooth] GATT FindAllAsync took " +
              toWString(static_cast<int>(elapsedMs(gattStart))) + L"ms, found " +
              toWString(static_cast<int>(deviceInfos.Size())) + L" LE devices");

        for (const auto &deviceInfo : deviceInfos) {
            const std::wstring devName = toWString(deviceInfo.Name());
            const std::wstring deviceId = canonicalId(deviceInfo);
            const bool isConnected = boolProperty(
                deviceInfo, hstring(L"System.Devices.Aep.IsConnected"), false);
            // 每设备诊断日志：看清 6 台设备各自的状态，定位为何读不到电量。
            LOG_W(L"[Bluetooth] GATT device: name=\"" + devName + L"\" connected=" +
                  (isConnected ? L"true" : L"false") + L" id=\"" + deviceId + L"\"");
            if (seenIds.count(deviceId)) {
                LOG_W(L"[Bluetooth] GATT skip (seen in Power::Battery): " + devName);
                continue;
            }
            // 关键：用属性判断连接状态，而不是依赖 selector。
            if (!isConnected) {
                LOG_W(L"[Bluetooth] GATT skip (not connected): " + devName);
                continue;
            }

            const auto fromIdStart = SteadyClock::now();
            BluetoothLEDevice bluetoothDevice = nullptr;
            try {
                bluetoothDevice = BluetoothLEDevice::FromIdAsync(deviceInfo.Id()).get();
            } catch (const winrt::hresult_error &) {
                bluetoothDevice = nullptr;
            }
            LOG_W(L"[Bluetooth] GATT FromIdAsync for " + devName + L" took " +
                  toWString(static_cast<int>(elapsedMs(fromIdStart))) + L"ms");
            if (!bluetoothDevice) {
                LOG_WARN_W(L"[Bluetooth] GATT FromIdAsync returned null: " + devName);
                continue;
            }
            // 二次确认连接（属性可能滞后）。
            if (bluetoothDevice.ConnectionStatus() != BluetoothConnectionStatus::Connected) {
                continue;
            }

            // 查询标准 Battery 服务（先 Cached 再 Uncached）。
            GattDeviceServicesResult servicesResult = nullptr;
            try {
                servicesResult = getBatteryServices(bluetoothDevice);
            } catch (const winrt::hresult_error &) {
                servicesResult = nullptr;
            }
            if (!servicesResult ||
                servicesResult.Status() != GattCommunicationStatus::Success ||
                servicesResult.Services().Size() == 0) {
                LOG_W(L"[Bluetooth] GATT no Battery svc: " + devName);
                continue;
            }

            for (const auto &service : servicesResult.Services()) {
                GattCharacteristicsResult characteristicsResult = nullptr;
                try {
                    characteristicsResult = getBatteryLevelCharacteristic(service);
                } catch (const winrt::hresult_error &) {
                    characteristicsResult = nullptr;
                }
                if (!characteristicsResult ||
                    characteristicsResult.Status() != GattCommunicationStatus::Success ||
                    characteristicsResult.Characteristics().Size() == 0) {
                    continue;
                }

                for (const auto &characteristic : characteristicsResult.Characteristics()) {
                    const auto percentage = readGattBatteryLevel(characteristic);
                    if (!percentage) {
                        LOG_W(L"[Bluetooth] GATT read battery level failed: " + devName);
                        continue;
                    }

                    LOG_W(L"[Bluetooth] GATT battery found: " + devName + L" = " +
                          toWString(*percentage) + L"%");
                    BatteryDevice device;
                    device.id = deviceId;
                    std::wstring name = toWString(bluetoothDevice.Name());
                    if (name.empty()) {
                        name = devName;
                    }
                    if (name.empty()) {
                        name = L"Bluetooth";
                    }
                    device.name = std::move(name);
                    device.type = BatteryDevice::Type::Bluetooth;
                    device.percentage = *percentage;
                    device.level = levelFromPercentage(*percentage);
                    device.connected = true;
                    devices.push_back(std::move(device));
                    seenIds.insert(deviceId);
                    break; // 每台设备取到一个有效电量即可。
                }
            }
        }

        // 经典蓝牙（A2DP/HFP）耳机的电量读取由 ClassicBluetoothProvider 负责：
        // 它通过 BluetoothDevice.GetDeviceSelector() 判断连接状态，再通过
        // SetupAPI 枚举 BTHENUM 类读 {104EA319-...},2 电量。本 provider 仅处理
        // BLE GATT 路径，避免与 ClassicBluetoothProvider 重复或误收外设。
    } catch (const winrt::hresult_error &e) {
        LOG_ERR_W(L"[Bluetooth] refresh failed (hresult): " + toWString(e.message()));
    } catch (const std::exception &e) {
        LOG_ERR(std::string("[Bluetooth] refresh failed (std): ") + e.what());
    } catch (...) {
        LOG_ERR("[Bluetooth] refresh failed (unknown exception)");
    }

    LOG_W(L"[Bluetooth] readDevices total = " + toWString(static_cast<int>(devices.size())));
    return devices;
}
