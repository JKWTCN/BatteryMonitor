#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// JS 插件设备的通用 HID 探测层(hidapi 之上,纯 C++,不依赖 Qt)。
//
// 封装从各原生 HID provider 抽出的通用逻辑:按 VID + PID 表枚举、
// vendor-defined 接口过滤(排除标准键盘/鼠标输入接口)、同 VID/PID 去重、
// 报告长度查询、last-good-path 接口缓存(同一物理设备常暴露多个接口,
// 只有一个能响应电量协议;记住上次成功的接口,下轮优先且只先查它)。
//
// 线程模型:实例存活于插件专属的 worker 线程;所有 hidapi 调用要求
// 调用方已持有 hidApiMutex()(与原生 provider 一致)。
class HidProbe
{
public:
    // 一个候选接口(已按 PID 过滤,未打开)。
    struct Interface
    {
        std::string path;
        uint16_t vendorId = 0;
        uint16_t productId = 0;
        uint16_t usagePage = 0;
        uint16_t usage = 0;
        std::wstring productString;

        // 输出 / 输入 / feature 报告数据长度(不含 Report ID),由 fillReportLengths
        // 填充;查询失败时回退默认 32。注意部分协议(如 Razer)只用 feature report,
        // 其接口可能没有 output report——usable = 输出或 feature 任一存在。
        size_t outDataLen = 0;
        size_t inDataLen = 0;
        size_t featureDataLen = 0;
        bool hasOutputReport = false;
        bool hasFeatureReport = false;
    };

    // vendorIds:该插件可见的总线范围(一个或多个 VID,如 VGN 的关联品牌)。
    HidProbe(std::vector<uint16_t> vendorIds, std::vector<uint16_t> productIds);

    // 枚举匹配接口:hid_enumerate(vendorId) 后按 PID 表过滤(不做 usage 过滤,
    // 与原生 Razer provider 一致——各厂商控制接口的 usage_page 并不统一)。
    // 返回顺序:按设备(VID/PID)分组,每组内上次成功缓存的接口排最前,
    // 其余保持枚举顺序。缓存 path 已不在枚举结果里(拔插/换口)时清除。
    std::vector<Interface> candidates();

    // 记录某接口本轮查询成功,下轮该设备优先且只先查它。
    void rememberGoodPath(const std::string &path);

    // 查询接口路径对应的输出 / 输入 / feature 报告数据长度(HidP_GetCaps)。
    // 返回 false 仅表示「确认是纯输入接口」(caps 读取成功且无输出也无
    // feature 报告),调用方应跳过。caps 查询本身失败(如 CreateFileW 被
    // 拒——键盘类集合会这样)时保留默认长度并返回 true:接口可能仍可被
    // hidapi 打开并响应查询,长度由插件显式传参兜底。
    static bool fillReportLengths(Interface *itf);

private:
    std::vector<uint16_t> m_vendorIds;
    std::vector<uint16_t> m_productIds;
    // key = (vendorId << 16) | productId → 上次查询成功的接口 path。
    std::unordered_map<uint32_t, std::string> m_lastGoodPath;
    // 最近一次 candidates() 输出的 path → 设备 key,供 rememberGoodPath 反查。
    std::unordered_map<std::string, uint32_t> m_pathToKey;
};
