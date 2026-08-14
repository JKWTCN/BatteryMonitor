#include "HidProbe.h"

#include "util/Logger.h"

#include <algorithm>
#include <cstring>

#include <hidapi.h>

#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>

namespace
{
    // 报告长度查询失败时的回退值(与原生 provider 一致:AULA 型号存在 32/64 两种)。
    constexpr size_t kDefaultReportDataSize = 32;

    std::uint32_t deviceKey(std::uint16_t vendorId, std::uint16_t productId)
    {
        return (static_cast<std::uint32_t>(vendorId) << 16) | productId;
    }
} // namespace

HidProbe::HidProbe(std::vector<uint16_t> vendorIds, std::vector<uint16_t> productIds)
    : m_vendorIds(std::move(vendorIds)), m_productIds(std::move(productIds))
{
}

std::vector<HidProbe::Interface> HidProbe::candidates()
{
    std::vector<Interface> out;

    // 逐 VID 枚举,汇总到同一条候选链;每个 VID 的枚举头独立释放。
    std::vector<hid_device_info *> enumHeads;
    for (uint16_t vendorId : m_vendorIds)
    {
        hid_device_info *head = hid_enumerate(vendorId, 0);
        if (head)
        {
            enumHeads.push_back(head);
        }
    }
    if (enumHeads.empty())
    {
        // 所有 VID 下均无设备属常态(接收器未插入),顺带清掉全部接口缓存。
        m_lastGoodPath.clear();
        return out;
    }

    // 第一遍:收集 PID 命中的接口(不做 usage 过滤——各厂商控制接口的
    // usage_page 并不统一,由 GetBattery 的成败与接口缓存来收敛)。
    std::vector<const hid_device_info *> matched;
    std::vector<std::uint32_t> presentKeys;
    for (const hid_device_info *enumHead : enumHeads)
    {
        for (const hid_device_info *cur = enumHead; cur; cur = cur->next)
        {
            if (std::find(m_productIds.begin(), m_productIds.end(), cur->product_id) ==
                m_productIds.end())
            {
                continue;
            }
            matched.push_back(cur);
            const std::uint32_t key = deviceKey(cur->vendor_id, cur->product_id);
            if (std::find(presentKeys.begin(), presentKeys.end(), key) ==
                presentKeys.end())
            {
                presentKeys.push_back(key);
            }
        }
    }

    // 清死路径:缓存里已不在本轮枚举结果中的设备,直接清除缓存。
    for (auto it = m_lastGoodPath.begin(); it != m_lastGoodPath.end();)
    {
        if (std::find(presentKeys.begin(), presentKeys.end(), it->first) ==
            presentKeys.end())
        {
            it = m_lastGoodPath.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // 第二遍:按设备分组输出,组内缓存接口排最前。
    // 设备顺序取枚举顺序;同组其余接口保持枚举顺序,供缓存接口失败后回退。
    std::vector<std::uint32_t> emittedKeys;
    auto emitGroup = [&](std::uint32_t key)
    {
        const auto cacheIt = m_lastGoodPath.find(key);
        // 先发缓存接口(若仍在)。
        if (cacheIt != m_lastGoodPath.end())
        {
            for (const hid_device_info *cur : matched)
            {
                if (deviceKey(cur->vendor_id, cur->product_id) == key &&
                    std::strcmp(cur->path, cacheIt->second.c_str()) == 0)
                {
                    Interface itf;
                    itf.path = cur->path;
                    itf.vendorId = cur->vendor_id;
                    itf.productId = cur->product_id;
                    itf.usagePage = cur->usage_page;
                    itf.usage = cur->usage;
                    itf.productString = cur->product_string ? cur->product_string : L"";
                    out.push_back(std::move(itf));
                    break;
                }
            }
        }
        // 再发其余接口。
        for (const hid_device_info *cur : matched)
        {
            if (deviceKey(cur->vendor_id, cur->product_id) != key)
            {
                continue;
            }
            if (cacheIt != m_lastGoodPath.end() &&
                std::strcmp(cur->path, cacheIt->second.c_str()) == 0)
            {
                continue; // 缓存接口已发过
            }
            Interface itf;
            itf.path = cur->path;
            itf.vendorId = cur->vendor_id;
            itf.productId = cur->product_id;
            itf.usagePage = cur->usage_page;
            itf.usage = cur->usage;
            itf.productString = cur->product_string ? cur->product_string : L"";
            out.push_back(std::move(itf));
        }
    };

    for (const hid_device_info *cur : matched)
    {
        const std::uint32_t key = deviceKey(cur->vendor_id, cur->product_id);
        if (std::find(emittedKeys.begin(), emittedKeys.end(), key) == emittedKeys.end())
        {
            emittedKeys.push_back(key);
            emitGroup(key);
        }
    }

    for (hid_device_info *enumHead : enumHeads)
    {
        hid_free_enumeration(enumHead);
    }

    // 维护 path → key 映射,供 rememberGoodPath 反查。
    m_pathToKey.clear();
    for (const Interface &itf : out)
    {
        m_pathToKey[itf.path] = deviceKey(itf.vendorId, itf.productId);
    }
    return out;
}

void HidProbe::rememberGoodPath(const std::string &path)
{
    // path → 设备 key 的对应关系在 candidates() 内建立,这里查表更新缓存。
    if (const auto it = m_pathToKey.find(path); it != m_pathToKey.end())
    {
        m_lastGoodPath[it->second] = path;
    }
}

bool HidProbe::fillReportLengths(Interface *itf)
{
    if (!itf || itf->path.empty())
    {
        return false;
    }
    itf->outDataLen = kDefaultReportDataSize;
    itf->inDataLen = kDefaultReportDataSize;

    const std::wstring wpath(itf->path.begin(), itf->path.end());
    // hidapi 已用共享读方式打开设备;这里仅需读取报告描述符,以只读 + 共享
    // 读写方式打开,避免与 hidapi 句柄产生独占冲突。
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        // 查不到 caps ≠ 接口不可用(键盘类集合会拒绝 GENERIC_READ 打开,
        // 但仍可能被 hidapi 打开并响应查询协议)。保留默认长度照常尝试。
        LOG_VERBOSE("[js] queryReportByteLengths: CreateFileW failed, err=" +
                    std::to_string(GetLastError()) + ", keeping defaults, path=" +
                    itf->path);
        return true;
    }

    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!HidD_GetPreparsedData(h, &preparsed) || !preparsed)
    {
        LOG_VERBOSE("[js] queryReportByteLengths: HidD_GetPreparsedData failed, "
                    "keeping defaults, path=" + itf->path);
        CloseHandle(h);
        return true;
    }

    bool usable = false;
    HIDP_CAPS caps{};
    if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS)
    {
        LOG_VERBOSE("[js] queryReportByteLengths: path=" + itf->path +
                    " out=" + std::to_string(caps.OutputReportByteLength) +
                    " in=" + std::to_string(caps.InputReportByteLength) +
                    " feature=" + std::to_string(caps.FeatureReportByteLength));
        // caps 的报告长度已含 Report ID 字节(设备使用编号报告时),故减 1;
        // 对不使用 Report ID 的设备减 1 会偏小,协议头仅 8 字节,偏小会在
        // 构造请求包时回退默认值,安全。
        if (caps.OutputReportByteLength > 1)
        {
            itf->outDataLen = caps.OutputReportByteLength - 1;
        }
        if (caps.InputReportByteLength > 1)
        {
            itf->inDataLen = caps.InputReportByteLength - 1;
        }
        if (caps.FeatureReportByteLength > 1)
        {
            itf->featureDataLen = caps.FeatureReportByteLength - 1;
        }
        itf->hasOutputReport = caps.OutputReportByteLength > 0;
        itf->hasFeatureReport = caps.FeatureReportByteLength > 0;
        // 既无输出报告也无 feature 报告的接口(纯输入)无法承载查询协议。
        // 注意部分协议(如 Razer)只用 feature report,不能只看输出报告。
        usable = itf->hasOutputReport || itf->hasFeatureReport;
    }

    HidD_FreePreparsedData(preparsed);
    CloseHandle(h);
    return usable;
}
