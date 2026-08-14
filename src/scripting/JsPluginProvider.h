#pragma once

#include "HidProbe.h"
#include "QuickjsEngine.h"
#include "src/core/IBatteryProvider.h"

#include <hidapi.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// JS 插件的电量提供者:一个 .js 文件 = 一个 provider(使用说明见 docs/js-plugin.md)。
//
// 插件通过具名导出函数声明自己:
//   Name() / VendorId() / ProductId()  —— 插件名与设备匹配表(必填)。
//   GetBattery()                       —— 协议本体(必填),宿主对每台匹配且成功
//                                          打开的设备每轮调用一次,返回电量对象
//                                          {percentage | level, charging?, wired?}
//                                          或 null。
// 协议函数内通过全局 device 对象收发包:write / read / pause / log 等。
//
// 线程模型:实例存活于 BatteryManager 分配的专属 worker 线程;JS 引擎的创建、
// 调用、销毁均在该线程内,无跨线程访问。
class JsPluginProvider : public IBatteryProvider
{
public:
    // pluginId:相对 plugins 根目录的无扩展路径(日志前缀与设置项标识);
    // filePath:.js 绝对路径。
    JsPluginProvider(std::string pluginId, std::wstring filePath);
    ~JsPluginProvider() override;

    std::wstring displayName() const override;
    std::vector<BatteryDevice> readDevices() override;

private:
    // —— device 全局对象绑定的宿主上下文(仅 worker 线程访问)——
    // public:原生函数实现(.cpp 文件级静态函数)需要能命名此类型。
public:
    struct HostContext
    {
        std::string pluginId;
        hid_device *handle = nullptr; // 当前绑定的接口句柄;null = 未绑定
        size_t outDataLen = 0;        // 输出报告数据长度(不含 Report ID)
        size_t inDataLen = 0;         // 输入报告数据长度
        size_t featureDataLen = 0;    // feature 报告数据长度(0 = 接口未定义)
        uint16_t vendorId = 0;
        uint16_t productId = 0;
        uint16_t usagePage = 0;       // 当前接口的顶层 usage(插件按需匹配)
        uint16_t usage = 0;
        std::wstring name;
        std::string path;
    };

private:

    // 首次 readDevices 时加载并校验插件;失败(文件读不出 / 语法错误 /
    // 必填导出缺失 / 非法值)返回 false 并停用。仅在 worker 线程调用。
    bool ensureLoaded();
    // 注册全局 device 对象与原生函数。
    void installBindings();
    // 绑定 / 解绑当前接口(readDevices 内对每个候选接口成对调用)。
    void bindInterface(const HidProbe::Interface &itf, hid_device *handle);
    // 调 GetBattery 并把返回值解码为 BatteryDevice 列表:
    //   - 返回电量对象 → 单设备;
    //   - 返回电量对象数组 → 多设备(一拖多接收器,条目可带 id 后缀/name 覆盖);
    //   - null / 空数组 / 各条目均缺少 percentage 与 level → 空列表。
    std::vector<BatteryDevice> queryInterface(const HidProbe::Interface &itf);

    std::string m_id;
    std::wstring m_filePath;
    std::string m_source; // 插件源码(UTF-8)

    std::unique_ptr<QuickjsEngine> m_engine;
    JSValue m_getBattery = JS_UNDEFINED; // 引用持有,析构时释放
    HostContext m_host;
    std::unique_ptr<HidProbe> m_probe;
    std::vector<uint16_t> m_vendorIds;

    std::wstring m_displayName; // 取自 Name();加载前为 L"js:<id>"
    int m_consecutiveFailures = 0;
    bool m_roundHadException = false; // 本轮是否出现 JS 异常(失败计数依据)
    bool m_loadAttempted = false;
    bool m_disabled = false;
};
