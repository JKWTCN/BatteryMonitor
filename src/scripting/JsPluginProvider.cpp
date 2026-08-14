#include "JsPluginProvider.h"

#include "src/providers/hid/HidApiLock.h"
#include "util/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>
#include <unordered_set>

#include <hidapi.h>

#include <windows.h>

namespace
{
    // —— 资源与轮询限额(见 docs/js-plugin.md)——
    // 单轮 readDevices 的总执行时限(含 hid 阻塞等待)。
    constexpr auto kRoundTimeout = std::chrono::seconds(10);
    // 单轮最多上报的设备数,防御性上限。
    constexpr size_t kMaxDevicesPerRound = 32;
    // device.read 的单次超时上限(毫秒),与原生 provider 的 hid_read_timeout 一致。
    constexpr int kMaxReadTimeoutMs = 500;
    // device.pause 的单次睡眠上限(毫秒)。
    constexpr int kMaxPauseMs = 1000;
    // 连续失败这么多轮后自动停用插件,避免每次轮询都刷错误日志。
    constexpr int kMaxConsecutiveFailures = 5;

    // —— UTF-8 ↔ 宽字符(核心层用 std::wstring,JS 字符串是 UTF-8)——
    std::wstring utf8ToWide(const std::string &s)
    {
        if (s.empty())
        {
            return {};
        }
        const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                            static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(),
                            len);
        return out;
    }

    std::string wideToUtf8(const std::wstring &s)
    {
        if (s.empty())
        {
            return {};
        }
        const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                            static_cast<int>(s.size()), nullptr, 0,
                                            nullptr, nullptr);
        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(),
                            len, nullptr, nullptr);
        return out;
    }

    std::wstring toHex4(unsigned short v)
    {
        wchar_t buf[8];
        swprintf(buf, 8, L"%04X", v);
        return std::wstring(buf);
    }

    std::string hex4Utf8(unsigned short v)
    {
        return wideToUtf8(toHex4(v));
    }

    // 精确百分比 → 离散档位(与原生 provider 一致的 80/50/20 阈值)。
    BatteryLevel levelFromPercentage(int percentage)
    {
        if (percentage < 0)
        {
            return BatteryLevel::Unknown;
        }
        if (percentage >= 80)
        {
            return BatteryLevel::Full;
        }
        if (percentage >= 50)
        {
            return BatteryLevel::Medium;
        }
        if (percentage >= 20)
        {
            return BatteryLevel::Low;
        }
        return BatteryLevel::Empty;
    }

    // JS 字符串档位 → BatteryLevel。非法值返回 Unknown 并由调用方按缺失处理。
    std::optional<BatteryLevel> levelFromString(const std::string &s)
    {
        if (s == "empty")
        {
            return BatteryLevel::Empty;
        }
        if (s == "low")
        {
            return BatteryLevel::Low;
        }
        if (s == "medium")
        {
            return BatteryLevel::Medium;
        }
        if (s == "full")
        {
            return BatteryLevel::Full;
        }
        return std::nullopt;
    }

    // —— device 全局对象的原生实现 ——
    // HostContext 挂在 JSContext opaque 上,一个 provider 一个 context,互不干扰。

    JsPluginProvider::HostContext *hostContext(JSContext *ctx)
    {
        return static_cast<JsPluginProvider::HostContext *>(JS_GetContextOpaque(ctx));
    }

    // 安全取一次 hidapi 错误信息并立即拷贝(脱离 hidapi 内部 buffer 的生命周期;
    // 对失败句柄反复调用 hid_error 有堆损坏风险,见 AulaHidProvider 注释)。
    std::string hidErrorUtf8(hid_device *dev)
    {
        const wchar_t *msg = hid_error(dev);
        return msg ? wideToUtf8(msg) : std::string("unknown hid error");
    }

    // 把数组 / Uint8Array 形式的 packet 拷出为字节序列;类型不符时抛 JS 异常。
    bool readPacketBytes(JSContext *ctx, JSValueConst val, std::vector<uint8_t> *out)
    {
        int64_t len = 0;
        if (JS_GetLength(ctx, val, &len) != 0 || len < 0 || len > 4096)
        {
            JS_ThrowTypeError(ctx, "packet must be an array of bytes");
            return false;
        }
        out->assign(static_cast<size_t>(len), 0);
        for (int64_t i = 0; i < len; ++i)
        {
            uint32_t byte = 0;
            JSValue item = JS_GetPropertyUint32(ctx, val, static_cast<uint32_t>(i));
            const bool ok = !JS_IsException(item) && JS_ToUint32(ctx, &byte, item) == 0;
            JS_FreeValue(ctx, item);
            if (!ok || byte > 0xFF)
            {
                JS_ThrowTypeError(ctx, "packet byte #%lld is not a byte value",
                                  static_cast<long long>(i));
                return false;
            }
            (*out)[static_cast<size_t>(i)] = static_cast<uint8_t>(byte);
        }
        return true;
    }

    JSValue jsDeviceWrite(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        auto *hc = hostContext(ctx);
        if (!hc || !hc->handle)
        {
            return JS_ThrowTypeError(ctx, "no device is being queried");
        }
        if (argc < 1)
        {
            return JS_ThrowTypeError(ctx, "device.write(packet[, length])");
        }

        std::vector<uint8_t> packet;
        if (!readPacketBytes(ctx, argv[0], &packet))
        {
            return JS_EXCEPTION;
        }

        // length:报告总长(含 Report ID);省略时用该接口的输出报告长度。
        uint32_t reportLen = 0;
        if (argc >= 2 && !JS_IsUndefined(argv[1]))
        {
            if (JS_ToUint32(ctx, &reportLen, argv[1]) != 0 || reportLen == 0 ||
                reportLen > 4096)
            {
                return JS_ThrowTypeError(ctx, "invalid report length");
            }
        }
        else
        {
            reportLen = static_cast<uint32_t>(hc->outDataLen) + 1;
        }
        if (packet.size() > reportLen)
        {
            return JS_ThrowTypeError(ctx, "packet (%zu bytes) exceeds report length %u",
                                     packet.size(), reportLen);
        }

        std::vector<uint8_t> buf(reportLen, 0);
        std::copy(packet.begin(), packet.end(), buf.begin());
        const int written = hid_write(hc->handle, buf.data(), buf.size());
        if (written < 0)
        {
            return JS_ThrowTypeError(ctx, "hid_write failed: %s",
                                     hidErrorUtf8(hc->handle).c_str());
        }
        return JS_NewInt32(ctx, written);
    }

    JSValue jsDeviceRead(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        auto *hc = hostContext(ctx);
        if (!hc || !hc->handle)
        {
            return JS_ThrowTypeError(ctx, "no device is being queried");
        }

        // length:读取缓冲区容量(含 Report ID 前缀);省略时用输入报告长度。
        uint32_t len = 0;
        if (argc >= 1 && !JS_IsUndefined(argv[0]))
        {
            if (JS_ToUint32(ctx, &len, argv[0]) != 0 || len == 0 || len > 4096)
            {
                return JS_ThrowTypeError(ctx, "invalid read length");
            }
        }
        else
        {
            len = static_cast<uint32_t>(hc->inDataLen) + 1;
        }

        // timeoutMs:上限与原生 provider 的 hid_read_timeout 一致,避免慢插件
        // 拖长全局 hidapi 互斥窗口。
        uint32_t timeoutMs = static_cast<uint32_t>(kMaxReadTimeoutMs);
        if (argc >= 2 && !JS_IsUndefined(argv[1]))
        {
            if (JS_ToUint32(ctx, &timeoutMs, argv[1]) != 0)
            {
                return JS_ThrowTypeError(ctx, "invalid read timeout");
            }
            timeoutMs = std::min(timeoutMs, static_cast<uint32_t>(kMaxReadTimeoutMs));
        }

        std::vector<uint8_t> buf(len, 0);
        const int n = hid_read_timeout(hc->handle, buf.data(), buf.size(),
                                       static_cast<int>(timeoutMs));
        if (n < 0)
        {
            return JS_ThrowTypeError(ctx, "hid_read failed: %s",
                                     hidErrorUtf8(hc->handle).c_str());
        }
        if (n == 0)
        {
            return JS_NULL; // 超时 / 无数据
        }
        return JS_NewUint8ArrayCopy(ctx, buf.data(), static_cast<size_t>(n));
    }

    // feature 报告收发:Razer 等协议不走 output/input report,而用
    // HID_SET_FEATURE / HID_GET_FEATURE(设备侧固件命令)。
    JSValue jsDeviceSendFeature(JSContext *ctx, JSValueConst, int argc,
                                JSValueConst *argv)
    {
        auto *hc = hostContext(ctx);
        if (!hc || !hc->handle)
        {
            return JS_ThrowTypeError(ctx, "no device is being queried");
        }
        if (argc < 1)
        {
            return JS_ThrowTypeError(ctx, "device.sendFeature(packet[, length])");
        }

        std::vector<uint8_t> packet;
        if (!readPacketBytes(ctx, argv[0], &packet))
        {
            return JS_EXCEPTION;
        }

        // length:feature 报告总长(含 Report ID);省略时用接口的 feature
        // 报告长度,接口未定义时回退输出报告长度。
        uint32_t reportLen = 0;
        if (argc >= 2 && !JS_IsUndefined(argv[1]))
        {
            if (JS_ToUint32(ctx, &reportLen, argv[1]) != 0 || reportLen == 0 ||
                reportLen > 4096)
            {
                return JS_ThrowTypeError(ctx, "invalid report length");
            }
        }
        else
        {
            reportLen = static_cast<uint32_t>(hc->featureDataLen > 0
                                                  ? hc->featureDataLen
                                                  : hc->outDataLen) +
                        1;
        }
        if (packet.size() > reportLen)
        {
            return JS_ThrowTypeError(ctx, "packet (%zu bytes) exceeds report length %u",
                                     packet.size(), reportLen);
        }

        std::vector<uint8_t> buf(reportLen, 0);
        std::copy(packet.begin(), packet.end(), buf.begin());
        const int written = hid_send_feature_report(hc->handle, buf.data(), buf.size());
        if (written < 0)
        {
            return JS_ThrowTypeError(ctx, "hid_send_feature_report failed: %s",
                                     hidErrorUtf8(hc->handle).c_str());
        }
        return JS_NewInt32(ctx, written);
    }

    JSValue jsDeviceGetFeature(JSContext *ctx, JSValueConst, int argc,
                               JSValueConst *argv)
    {
        auto *hc = hostContext(ctx);
        if (!hc || !hc->handle)
        {
            return JS_ThrowTypeError(ctx, "no device is being queried");
        }

        // length:feature 报告总长(含 Report ID);省略时同 sendFeature。
        uint32_t len = 0;
        if (argc >= 1 && !JS_IsUndefined(argv[0]))
        {
            if (JS_ToUint32(ctx, &len, argv[0]) != 0 || len == 0 || len > 4096)
            {
                return JS_ThrowTypeError(ctx, "invalid feature report length");
            }
        }
        else
        {
            len = static_cast<uint32_t>(hc->featureDataLen > 0 ? hc->featureDataLen
                                                               : hc->outDataLen) +
                  1;
        }

        std::vector<uint8_t> buf(len, 0);
        buf[0] = 0; // Report ID(设备只有单一 feature 报告)
        const int n = hid_get_feature_report(hc->handle, buf.data(), buf.size());
        if (n <= 0)
        {
            return JS_ThrowTypeError(ctx, "hid_get_feature_report failed: %s",
                                     hidErrorUtf8(hc->handle).c_str());
        }
        return JS_NewUint8ArrayCopy(ctx, buf.data(), static_cast<size_t>(n));
    }

    JSValue jsDevicePause(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (argc < 1)
        {
            return JS_ThrowTypeError(ctx, "device.pause(ms)");
        }
        uint32_t ms = 0;
        if (JS_ToUint32(ctx, &ms, argv[0]) != 0)
        {
            return JS_ThrowTypeError(ctx, "invalid pause duration");
        }
        ms = std::min(ms, static_cast<uint32_t>(kMaxPauseMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return JS_UNDEFINED;
    }

    // log / logWarn / logError 共用:参数逐个字符串化后空格拼接。
    void hostLog(JSContext *ctx, int argc, JSValueConst *argv, Logger::Level level)
    {
        auto *hc = hostContext(ctx);
        std::string line = "[js:";
        line += hc ? hc->pluginId : std::string("?");
        line += "] ";
        for (int i = 0; i < argc; ++i)
        {
            if (i > 0)
            {
                line += ' ';
            }
            size_t len = 0;
            const char *s = JS_ToCStringLen(ctx, &len, argv[i]);
            if (s)
            {
                line.append(s, len);
                JS_FreeCString(ctx, s);
            }
        }
        Logger::instance().log(line, level);
    }

    JSValue jsDeviceLog(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        hostLog(ctx, argc, argv, Logger::Level::Verbose);
        return JS_UNDEFINED;
    }

    JSValue jsDeviceLogWarn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        hostLog(ctx, argc, argv, Logger::Level::Warning);
        return JS_UNDEFINED;
    }

    JSValue jsDeviceLogError(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        hostLog(ctx, argc, argv, Logger::Level::Error);
        return JS_UNDEFINED;
    }

    JSValue jsDeviceProductId(JSContext *ctx, JSValueConst, int, JSValueConst *)
    {
        const auto *hc = hostContext(ctx);
        return hc ? JS_NewUint32(ctx, hc->productId) : JS_UNDEFINED;
    }

    JSValue jsDeviceUsagePage(JSContext *ctx, JSValueConst, int, JSValueConst *)
    {
        const auto *hc = hostContext(ctx);
        return hc ? JS_NewUint32(ctx, hc->usagePage) : JS_UNDEFINED;
    }

    JSValue jsDeviceUsage(JSContext *ctx, JSValueConst, int, JSValueConst *)
    {
        const auto *hc = hostContext(ctx);
        return hc ? JS_NewUint32(ctx, hc->usage) : JS_UNDEFINED;
    }

    JSValue jsDeviceName(JSContext *ctx, JSValueConst, int, JSValueConst *)
    {
        const auto *hc = hostContext(ctx);
        if (!hc)
        {
            return JS_UNDEFINED;
        }
        const std::string utf8 = wideToUtf8(hc->name);
        return JS_NewStringLen(ctx, utf8.data(), utf8.size());
    }

    JSValue jsDevicePath(JSContext *ctx, JSValueConst, int, JSValueConst *)
    {
        const auto *hc = hostContext(ctx);
        if (!hc)
        {
            return JS_UNDEFINED;
        }
        return JS_NewStringLen(ctx, hc->path.data(), hc->path.size());
    }
} // namespace

JsPluginProvider::JsPluginProvider(std::string pluginId, std::wstring filePath)
    : m_id(std::move(pluginId)), m_filePath(std::move(filePath)),
      m_displayName(L"js:" + utf8ToWide(m_id))
{
}

JsPluginProvider::~JsPluginProvider()
{
    // m_getBattery 的引用必须在引擎销毁前显式释放。
    if (m_engine && m_engine->context() && !JS_IsUninitialized(m_getBattery))
    {
        JS_FreeValue(m_engine->context(), m_getBattery);
        m_getBattery = JS_UNDEFINED;
    }
}

std::wstring JsPluginProvider::displayName() const
{
    return m_displayName;
}

bool JsPluginProvider::ensureLoaded()
{
    // 读取源码(UTF-8)。
    {
        std::ifstream in(std::filesystem::path(m_filePath), std::ios::binary);
        if (!in)
        {
            LOG_ERR("plugin '" + m_id + "' cannot be opened: " + wideToUtf8(m_filePath));
            return false;
        }
        m_source.assign(std::istreambuf_iterator<char>(in), {});
    }

    m_engine = std::make_unique<QuickjsEngine>();
    if (!m_engine->context())
    {
        LOG_ERR("plugin '" + m_id + "': QuickJS engine creation failed");
        return false;
    }
    m_host.pluginId = m_id;
    installBindings();

    std::string error;
    if (!m_engine->loadModuleSource(m_source, m_id + ".js", &error))
    {
        LOG_ERR("plugin '" + m_id + "' failed to load: " + error);
        return false;
    }

    JSContext *ctx = m_engine->context();

    // 调用具名导出,取插件声明。缺 Name/VendorId/ProductId 任一即加载失败。
    auto callExportInt = [&](const char *name, int32_t *out) -> bool
    {
        JSValue fn = m_engine->moduleExport(name);
        if (!JS_IsFunction(ctx, fn))
        {
            JS_FreeValue(ctx, fn);
            return false;
        }
        JSValue val = JS_Call(ctx, fn, JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(ctx, fn);
        if (JS_IsException(val))
        {
            LOG_ERR("plugin '" + m_id + "': " + name + "() threw: " +
                    m_engine->takePendingException());
            return false;
        }
        const bool ok = JS_ToInt32(ctx, out, val) == 0;
        JS_FreeValue(ctx, val);
        return ok;
    };

    // 把导出值(单个 number 或 number 数组)解析为 VID/PID 列表。
    auto parseIntList = [&](const char *name, std::vector<uint16_t> *out) -> bool
    {
        JSValue fn = m_engine->moduleExport(name);
        if (!JS_IsFunction(ctx, fn))
        {
            JS_FreeValue(ctx, fn);
            return false;
        }
        JSValue val = JS_Call(ctx, fn, JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(ctx, fn);
        if (JS_IsException(val))
        {
            LOG_ERR("plugin '" + m_id + "': " + name + "() threw: " +
                    m_engine->takePendingException());
            return false;
        }
        // 单个 number → 单元素列表。必须用 JS_IsNumber 严格判断:不能靠
        // JS_ToInt32 的隐式转换——单元素数组 [0x83] 也会被转成 131,
        // 把 PID 表误当单个 PID。
        int32_t single = 0;
        if (JS_IsNumber(val))
        {
            const bool ok = JS_ToInt32(ctx, &single, val) == 0;
            JS_FreeValue(ctx, val);
            if (ok && single > 0 && single <= 0xFFFF)
            {
                out->push_back(static_cast<uint16_t>(single));
            }
            return !out->empty();
        }
        // 数组。
        int64_t len = 0;
        const bool isArray = JS_IsArray(val) && JS_GetLength(ctx, val, &len) == 0;
        const int64_t maxLen = 1024;
        if (isArray && len > 0 && len <= maxLen)
        {
            out->reserve(static_cast<size_t>(len));
            for (int64_t i = 0; i < len; ++i)
            {
                JSValue item = JS_GetPropertyUint32(ctx, val, static_cast<uint32_t>(i));
                int32_t id = 0;
                if (!JS_IsException(item) && JS_ToInt32(ctx, &id, item) == 0 &&
                    id > 0 && id <= 0xFFFF)
                {
                    out->push_back(static_cast<uint16_t>(id));
                }
                JS_FreeValue(ctx, item);
            }
        }
        JS_FreeValue(ctx, val);
        return !out->empty();
    };

    // VendorId:单个 VID(number)或多个(number[],如 VGN 的关联品牌簇)。
    // 它同时就是该插件可见的总线范围。
    std::vector<uint16_t> vendorIds;
    if (!parseIntList("VendorId", &vendorIds))
    {
        LOG_ERR("plugin '" + m_id +
                "': VendorId() must return a USB vendor id or array of ids");
        return false;
    }

    std::vector<uint16_t> productIds;
    if (!parseIntList("ProductId", &productIds))
    {
        LOG_ERR("plugin '" + m_id +
                "': ProductId() must return a product id or non-empty array of ids");
        return false;
    }

    // GetBattery 必须是函数。
    m_getBattery = m_engine->moduleExport("GetBattery");
    if (!JS_IsFunction(ctx, m_getBattery))
    {
        JS_FreeValue(ctx, m_getBattery);
        m_getBattery = JS_UNDEFINED;
        LOG_ERR("plugin '" + m_id + "': missing GetBattery() export");
        return false;
    }

    // Name() 可选,缺省用 "js:<id>"。
    {
        JSValue fn = m_engine->moduleExport("Name");
        if (JS_IsFunction(ctx, fn))
        {
            JSValue val = JS_Call(ctx, fn, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, fn);
            if (JS_IsException(val))
            {
                LOG_ERR("plugin '" + m_id + "': Name() threw: " +
                        m_engine->takePendingException());
                return false;
            }
            size_t len = 0;
            const char *s = JS_ToCStringLen(ctx, &len, val);
            if (s && len > 0)
            {
                m_displayName = utf8ToWide(std::string(s, len));
            }
            if (s)
            {
                JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, val);
        }
    }

    m_probe = std::make_unique<HidProbe>(std::move(vendorIds), std::move(productIds));
    LOG_W(utf8ToWide(m_id) + L" loaded: " + m_displayName);
    return true;
}

void JsPluginProvider::installBindings()
{
    JSContext *ctx = m_engine->context();

    JSValue deviceObj = JS_NewObject(ctx);
    auto setFn = [ctx, &deviceObj](const char *name, JSCFunction *fn)
    {
        JS_SetPropertyStr(ctx, deviceObj, name,
                          JS_NewCFunction(ctx, fn, name, 2));
    };
    setFn("write", &jsDeviceWrite);
    setFn("read", &jsDeviceRead);
    setFn("sendFeature", &jsDeviceSendFeature);
    setFn("getFeature", &jsDeviceGetFeature);
    setFn("pause", &jsDevicePause);
    setFn("log", &jsDeviceLog);
    setFn("logWarn", &jsDeviceLogWarn);
    setFn("logError", &jsDeviceLogError);
    setFn("productId", &jsDeviceProductId);
    setFn("usagePage", &jsDeviceUsagePage);
    setFn("usage", &jsDeviceUsage);
    setFn("name", &jsDeviceName);
    setFn("path", &jsDevicePath);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "device", deviceObj); // deviceObj 所有权移交
    JS_FreeValue(ctx, global);

    // HostContext 挂 context opaque:一个 provider 一个 context,原生函数据此
    // 拿到当前绑定的接口句柄与插件信息。
    JS_SetContextOpaque(ctx, &m_host);
}

void JsPluginProvider::bindInterface(const HidProbe::Interface &itf, hid_device *handle)
{
    m_host.handle = handle;
    m_host.outDataLen = itf.outDataLen;
    m_host.inDataLen = itf.inDataLen;
    m_host.featureDataLen = itf.featureDataLen;
    m_host.vendorId = itf.vendorId;
    m_host.productId = itf.productId;
    m_host.usagePage = itf.usagePage;
    m_host.usage = itf.usage;
    m_host.name = itf.productString;
    m_host.path = itf.path;
}

std::vector<BatteryDevice> JsPluginProvider::queryInterface(
    const HidProbe::Interface &itf)
{
    JSContext *ctx = m_engine->context();
    JSValue result = JS_Call(ctx, m_getBattery, JS_UNDEFINED, 0, nullptr);
    if (JS_IsException(result))
    {
        const std::string err = m_engine->takePendingException();
        LOG_ERR("plugin '" + m_id + "' GetBattery threw on " + itf.path + ": " + err);
        m_roundHadException = true;
        return {};
    }

    std::vector<BatteryDevice> out;

    // 解码单个电量对象;缺少 percentage 与 level 返回 std::nullopt。
    auto decodeObject = [&](JSValueConst obj,
                            const std::wstring &idSuffix) -> std::optional<BatteryDevice>
    {
        // percentage 与 level 二选一;都缺省视为读不到。
        bool hasPercentage = false;
        int32_t percentage = -1;
        JSValue pctVal = JS_GetPropertyStr(ctx, obj, "percentage");
        if (!JS_IsUndefined(pctVal) && !JS_IsNull(pctVal))
        {
            if (JS_ToInt32(ctx, &percentage, pctVal) == 0)
            {
                hasPercentage = (percentage >= 0 && percentage <= 100);
            }
        }
        JS_FreeValue(ctx, pctVal);

        std::optional<BatteryLevel> level;
        {
            JSValue lvlVal = JS_GetPropertyStr(ctx, obj, "level");
            if (JS_IsString(lvlVal))
            {
                size_t len = 0;
                const char *s = JS_ToCStringLen(ctx, &len, lvlVal);
                if (s)
                {
                    level = levelFromString(std::string(s, len));
                    JS_FreeCString(ctx, s);
                }
            }
            JS_FreeValue(ctx, lvlVal);
        }

        if (!hasPercentage && !level)
        {
            return std::nullopt;
        }

        BatteryDevice device;
        device.id = L"js:" + utf8ToWide(m_id) + L":" + toHex4(itf.vendorId) + L":" +
                    toHex4(itf.productId) + idSuffix;
        device.name = !itf.productString.empty()
                          ? itf.productString
                          : L"JS Device 0x" + toHex4(itf.vendorId) + L":0x" +
                                toHex4(itf.productId);
        // 插件可用 name 字段覆盖默认名(按 PID 表起名的插件用)。
        {
            JSValue v = JS_GetPropertyStr(ctx, obj, "name");
            if (JS_IsString(v))
            {
                size_t len = 0;
                const char *s = JS_ToCStringLen(ctx, &len, v);
                if (s && len > 0)
                {
                    device.name = utf8ToWide(std::string(s, len));
                }
                if (s)
                {
                    JS_FreeCString(ctx, s);
                }
            }
            JS_FreeValue(ctx, v);
        }
        device.type = BatteryDevice::Type::Hid;
        device.percentage = hasPercentage ? percentage : -1;
        device.level = level ? *level : levelFromPercentage(hasPercentage ? percentage : -1);
        device.connected = true;

        auto readBool = [&](const char *prop, bool fallback)
        {
            JSValue v = JS_GetPropertyStr(ctx, obj, prop);
            if (JS_IsBool(v))
            {
                fallback = JS_ToBool(ctx, v) == 1;
            }
            JS_FreeValue(ctx, v);
            return fallback;
        };
        device.charging = readBool("charging", false);
        device.wired = readBool("wired", false);
        return device;
    };

    if (JS_IsArray(result))
    {
        // 电量对象数组:一拖多接收器(一个 dongle 多台子设备)。
        // 条目可用 id 字段(字符串)作唯一后缀,name 字段覆盖默认名。
        int64_t len = 0;
        if (JS_GetLength(ctx, result, &len) == 0 && len > 0 &&
            len <= static_cast<int64_t>(kMaxDevicesPerRound))
        {
            for (int64_t i = 0; i < len; ++i)
            {
                JSValue item = JS_GetPropertyUint32(ctx, result, static_cast<uint32_t>(i));
                if (JS_IsObject(item) && !JS_IsArray(item))
                {
                    std::wstring suffix = L"#" + std::to_wstring(i);
                    {
                        JSValue idVal = JS_GetPropertyStr(ctx, item, "id");
                        if (JS_IsString(idVal))
                        {
                            size_t slen = 0;
                            const char *s = JS_ToCStringLen(ctx, &slen, idVal);
                            if (s && slen > 0 && slen <= 64)
                            {
                                suffix = L":" + utf8ToWide(std::string(s, slen));
                            }
                            if (s)
                            {
                                JS_FreeCString(ctx, s);
                            }
                        }
                        JS_FreeValue(ctx, idVal);
                    }
                    if (auto device = decodeObject(item, suffix))
                    {
                        out.push_back(std::move(*device));
                    }
                }
                JS_FreeValue(ctx, item);
            }
        }
    }
    else if (JS_IsObject(result))
    {
        if (auto device = decodeObject(result, std::wstring()))
        {
            out.push_back(std::move(*device));
        }
        else
        {
            LOG_VERBOSE("plugin '" + m_id +
                        "': GetBattery result has neither percentage nor level");
        }
    }
    // null / undefined:设备本轮不在线或读不到,属正常路径,不打日志。

    JS_FreeValue(ctx, result);
    return out;
}

std::vector<BatteryDevice> JsPluginProvider::readDevices()
{
    if (m_disabled)
    {
        return {};
    }
    if (!m_loadAttempted)
    {
        m_loadAttempted = true;
        if (!ensureLoaded())
        {
            m_disabled = true;
            return {};
        }
    }

    std::vector<BatteryDevice> devices;
    std::unordered_set<uint32_t> processedDevices;
    m_roundHadException = false;
    m_engine->setDeadline(std::chrono::steady_clock::now() + kRoundTimeout);

    // 与原生 provider 相同:整轮持有 hidapi 互斥锁,所有 hid 调用串行化。
    std::lock_guard<std::recursive_mutex> hidLock(hidApiMutex());

    if (hid_init() != 0)
    {
        LOG_ERR("plugin '" + m_id + "': hid_init failed");
        return devices;
    }

    for (const HidProbe::Interface &candidate : m_probe->candidates())
    {
        if (devices.size() >= kMaxDevicesPerRound || m_engine->pastDeadline())
        {
            break;
        }
        const uint32_t key =
            (static_cast<uint32_t>(candidate.vendorId) << 16) | candidate.productId;
        if (processedDevices.count(key) != 0)
        {
            continue; // 该设备本轮已成功,跳过同设备其它接口
        }

        LOG_VERBOSE("[js:" + m_id + "] interface: vid=0x" + hex4Utf8(candidate.vendorId) +
                    " pid=0x" + hex4Utf8(candidate.productId) +
                    " usage_page=0x" + hex4Utf8(candidate.usagePage) +
                    " usage=0x" + hex4Utf8(candidate.usage) + " path=" + candidate.path);

        // 查报告长度;既无输出报告也无 feature 报告的接口(纯输入)无法承载
        // 查询协议,直接跳过。
        HidProbe::Interface itf = candidate;
        if (!HidProbe::fillReportLengths(&itf))
        {
            LOG_VERBOSE("[js:" + m_id + "]   skipped (no output/feature report)");
            continue;
        }

        hid_device *dev = hid_open_path(itf.path.c_str());
        if (!dev)
        {
            LOG_VERBOSE("[js:" + m_id + "]   hid_open_path failed");
            continue; // 接口被独占等,试下一个接口
        }

        bindInterface(itf, dev);
        std::vector<BatteryDevice> result = queryInterface(itf);
        // 每轮强制关闭全部句柄:插件无法也不需要自己管理句柄生命周期。
        hid_close(dev);
        m_host.handle = nullptr;

        if (!result.empty())
        {
            for (BatteryDevice &d : result)
            {
                if (devices.size() >= kMaxDevicesPerRound)
                {
                    break;
                }
                LOG_VERBOSE("[js:" + m_id + "]   " + wideToUtf8(d.name) + " = " +
                            std::to_string(d.percentage) + "%");
                devices.push_back(std::move(d));
            }
            processedDevices.insert(key);
            m_probe->rememberGoodPath(itf.path); // 下轮优先且只先查该接口
        }
        else
        {
            LOG_VERBOSE("[js:" + m_id + "]   GetBattery returned no devices");
        }
    }

    // 连续多轮异常(协议死循环 / 超时 / 崩溃)后自动停用,避免每次轮询刷错误。
    // 只统计「本轮有异常且一台设备都没读到」的整轮失败:多接口设备部分接口
    // 查询抛异常属常态(纯输入接口),不能因此计数。
    if (m_roundHadException && devices.empty())
    {
        ++m_consecutiveFailures;
        if (m_consecutiveFailures >= kMaxConsecutiveFailures)
        {
            m_disabled = true;
            LOG_W(utf8ToWide(m_id) +
                  L": disabled after " + std::to_wstring(m_consecutiveFailures) +
                  L" consecutive failing rounds");
        }
    }
    else
    {
        m_consecutiveFailures = 0;
    }

    return devices;
}
