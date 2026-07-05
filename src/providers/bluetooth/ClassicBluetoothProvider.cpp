#include "ClassicBluetoothProvider.h"
#include "util/Logger.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <windows.h>
#include <setupapi.h>

#pragma comment(lib, "setupapi.lib")

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Enumeration;

namespace
{
// 电量属性键 {104EA319-6EE2-4701-BD47-8DDBF425BBE5}, pid=2
// （DEVPKEY_Device_Battery 百分比）。
const DEVPROPKEY kBthenumBatteryKey = {
    {0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}},
    2};
}

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

std::wstring toLower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return s;
}

// 设备名是否看起来像蓝牙音频设备（用于 SetupDi 侧的初步过滤）。
bool looksLikeBtAudio(const std::wstring &name)
{
    const auto ln = toLower(name);
    return ln.find(L"bluetooth") != std::wstring::npos ||
           ln.find(L"headset") != std::wstring::npos ||
           ln.find(L"hands-free") != std::wstring::npos;
}

// 第 1 步：枚举已连接的经典蓝牙设备，返回 {设备名 -> 规范化ID} 映射。
// 用 BluetoothDevice.GetDeviceSelector() + Aep.IsConnected，
// 属性缺失/非 bool 时回退 BluetoothDevice.FromIdAsync().ConnectionStatus。
std::map<std::wstring, std::wstring, std::less<>> collectConnectedClassicDevices()
{
    std::map<std::wstring, std::wstring, std::less<>> result;
    const winrt::hstring connectedProp = L"System.Devices.Aep.IsConnected";
    const winrt::hstring containerProp = L"System.Devices.ContainerId";
    std::vector<winrt::hstring> extra = {connectedProp, containerProp};
    const auto iterable =
        winrt::multi_threaded_vector<winrt::hstring>(std::move(extra))
            .as<winrt::Windows::Foundation::Collections::IIterable<winrt::hstring>>();

    DeviceInformationCollection found = nullptr;
    try {
        found = DeviceInformation::FindAllAsync(
            BluetoothDevice::GetDeviceSelector(), iterable).get();
    } catch (const winrt::hresult_error &e) {
        LOG_ERR_W(L"[ClassicBT] enum failed: " + std::wstring(e.message()));
        return result;
    }
    if (!found) {
        return result;
    }

    for (const auto &info : found) {
        const std::wstring name = std::wstring(info.Name());
        if (name.empty()) {
            continue;
        }
        bool connected = false;
        const auto props = info.Properties();
        if (props && props.HasKey(connectedProp)) {
            const auto v = props.Lookup(connectedProp);
            if (v) {
                try {
                    connected = v.as<winrt::Windows::Foundation::IReference<bool>>().Value();
                } catch (...) {
                    connected = false;
                }
            }
        }
        // 属性缺失/非 bool 时回退到 BluetoothDevice.ConnectionStatus。
        if (!connected) {
            try {
                const auto dev = BluetoothDevice::FromIdAsync(info.Id()).get();
                if (dev && dev.ConnectionStatus() == BluetoothConnectionStatus::Connected) {
                    connected = true;
                }
            } catch (...) {
            }
        }
        if (!connected) {
            continue;
        }
        // 规范化 ID：优先 ContainerId。
        std::wstring id = std::wstring(info.Id());
        if (props && props.HasKey(containerProp)) {
            const auto cv = props.Lookup(containerProp);
            if (cv) {
                try {
                    const auto g = cv.as<
                        winrt::Windows::Foundation::IReference<winrt::guid>>().Value();
                    if (g != winrt::guid{}) {
                        id = L"container:" + std::wstring(winrt::to_hstring(g));
                    }
                } catch (...) {
                }
            }
        }
        // 用设备名做键（SetupDi 侧按名匹配）；保留首个映射，去重。
        if (!result.count(name)) {
            result.emplace(name, id);
        }
    }
    return result;
}

// 第 2 步：SetupDi 枚举 BTHENUM 类，读电量。
// 返回 {设备名 -> 百分比}。
std::map<std::wstring, int, std::less<>> readBthenumBattery(
    const std::map<std::wstring, std::wstring, std::less<>> &connected)
{
    std::map<std::wstring, int, std::less<>> batteryByName;
    if (connected.empty()) {
        return batteryByName;
    }

    // 打开 BTHENUM 枚举器下的设备信息集（仅当前存在的设备）。
    // flags = DIGCF_PRESENT(2) | DIGCF_ALLCLASSES(4)：ALLCLASSES 让第二个参数
    // 被当作 enumerator 名（"BTHENUM"），而非 class GUID。
    HDEVINFO hDev = SetupDiGetClassDevsW(nullptr, L"BTHENUM", nullptr,
                                         DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (hDev == nullptr || hDev == INVALID_HANDLE_VALUE) {
        LOG_ERR(L"[ClassicBT] SetupDiGetClassDevs(BTHENUM) failed");
        return batteryByName;
    }

    // 电量属性键。
    const DEVPROPKEY &batteryKey = kBthenumBatteryKey;

    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(devData);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDev, i, &devData); ++i) {
        // 读友好名（SPDRP_FRIENDLYNAME），失败再读 DeviceDesc。
        wchar_t nameBuf[256] = {};
        std::wstring devName;
        if (SetupDiGetDeviceRegistryPropertyW(hDev, &devData, SPDRP_FRIENDLYNAME,
                                              nullptr, reinterpret_cast<PBYTE>(nameBuf),
                                              sizeof(nameBuf), nullptr) && nameBuf[0]) {
            devName = nameBuf;
        } else if (SetupDiGetDeviceRegistryPropertyW(hDev, &devData, SPDRP_DEVICEDESC,
                                                     nullptr, reinterpret_cast<PBYTE>(nameBuf),
                                                     sizeof(nameBuf), nullptr) && nameBuf[0]) {
            devName = nameBuf;
        }
        if (devName.empty()) {
            continue;
        }
        // 名字需看起来像蓝牙音频，且与某个已连接设备匹配。
        if (!looksLikeBtAudio(devName)) {
            continue;
        }
        LOG_W(L"[ClassicBT] BTHENUM node: \"" + devName + L"\"");
        // 在已连接集合里找匹配。
        std::wstring matchedName;
        for (const auto &[connName, id] : connected) {
            if (devName.find(connName) != std::wstring::npos ||
                connName.find(devName) != std::wstring::npos) {
                matchedName = connName;
                break;
            }
        }
        if (matchedName.empty()) {
            LOG_W(L"[ClassicBT]   no connected match, skip");
            continue;
        }
        // 读电量。电量百分比 0-100 可能以 DEVPROP_TYPE_BYTE / INT16 / UINT16 /
        // INT32 / UINT32 任一形式返回（实测多数耳机是 BYTE，即 type=3）。
        // 缓冲区 8 字节、零填充，按 int32 取值（高字节为 0，结果与窄类型一致）。
        BYTE buf[8] = {};
        DEVPROPTYPE propType = DEVPROP_TYPE_EMPTY;
        const BOOL ok = SetupDiGetDevicePropertyW(hDev, &devData, &batteryKey, &propType,
                                                  buf, sizeof(buf), nullptr, 0);
        const bool numericType =
            propType == DEVPROP_TYPE_BYTE ||
            propType == DEVPROP_TYPE_INT16 ||
            propType == DEVPROP_TYPE_UINT16 ||
            propType == DEVPROP_TYPE_INT32 ||
            propType == DEVPROP_TYPE_UINT32;
        if (!ok) {
            // API 调用失败（属性不存在返回 CR_NO_SUCH_VALUE -> err=1168 等）。
            LOG_W(L"[ClassicBT]   battery query failed: win32=" +
                  std::to_wstring(GetLastError()));
        } else if (!numericType) {
            // 调用成功但类型不是数值（如 STRING / EMPTY），此节点无电量属性。
            LOG_W(L"[ClassicBT]   battery property unavailable: type=" +
                  std::to_wstring(propType));
        } else {
            const int pct = *reinterpret_cast<const int *>(buf);
            if (pct >= 0 && pct <= 100) {
                LOG_W(L"[ClassicBT]   battery read OK: " + std::to_wstring(pct) + L"%");
                // 同名取一份（按 canonical key 合并）。
                if (!batteryByName.count(matchedName) || pct > batteryByName[matchedName]) {
                    batteryByName[matchedName] = pct;
                }
            } else {
                LOG_W(L"[ClassicBT]   battery out of range: " + std::to_wstring(pct));
            }
        }
    }
    SetupDiDestroyDeviceInfoList(hDev);
    return batteryByName;
}
} // namespace

std::wstring ClassicBluetoothProvider::displayName() const
{
    return L"ClassicBluetooth";
}

std::vector<BatteryDevice> ClassicBluetoothProvider::readDevices()
{
    std::vector<BatteryDevice> devices;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }

    // 第 1 步：哪些经典蓝牙设备当前在线。
    const auto connected = collectConnectedClassicDevices();
    LOG_W(L"[ClassicBT] connected classic devices = " +
          std::to_wstring(connected.size()));
    for (const auto &[name, id] : connected) {
        LOG_W(L"[ClassicBT]   connected: " + name + L" -> " + id);
    }

    // 第 2 步：从 BTHENUM 设备节点读电量。
    const auto batteryByName = readBthenumBattery(connected);

    // 合并：已连接设备 + 读到的电量（读不到的标 -1）。
    std::set<std::wstring> seen;
    for (const auto &[name, id] : connected) {
        if (seen.count(id)) {
            continue;
        }
        seen.insert(id);
        int pct = -1;
        const auto it = batteryByName.find(name);
        if (it != batteryByName.end()) {
            pct = it->second;
        }
        BatteryDevice device;
        device.id = id;
        device.name = name;
        device.type = BatteryDevice::Type::Bluetooth;
        device.percentage = pct;
        device.level = levelFromPercentage(pct);
        device.connected = true;
        devices.push_back(std::move(device));
        LOG_W(L"[ClassicBT] device: " + name + L" = " + std::to_wstring(pct) +
              L"% (" + id + L")");
    }
    return devices;
}
