#pragma once

#include "quickjs.h"

#include <chrono>
#include <string>

// QuickJS-ng 引擎的 RAII 封装(纯 C++,不依赖 Qt)。
//
// 每个 JS 插件持有一个独立实例(即独立 JSRuntime/JSContext),天然实现
// 插件间堆与 GC 的隔离;配合内存 / 栈 / 执行时限三重限额,单个插件的
// 内存泄漏或死循环不会波及其它插件与主程序。
//
// 沙箱边界:
//   - 不编译 quickjs-libc,不注入 std/os 模块;插件只能使用 ECMAScript
//     标准库与宿主注册的全局对象(device)。
//   - 未设置模块加载器,插件里的 import 语句会直接报错(单文件即插件)。
//   - 预置沙箱前导代码禁用 Date、把 Math.random 置为常量桩,保证同输入
//     产出可复现,便于协议调试(见 docs/js-plugin.md 的沙箱限制)。
class QuickjsEngine
{
public:
    // 单插件资源限额(见 docs/js-plugin.md 的调试和故障处理)。
    static constexpr size_t kMemoryLimit = 16 * 1024 * 1024; // 16 MB
    static constexpr size_t kMaxStackSize = 256 * 1024;      // 256 KB
    // 引擎创建后、首轮 setDeadline 前的默认执行时限(沙箱前导、模块加载、
    // Name/VendorId/ProductId 求值都发生在窗口内)。
    static constexpr auto kDefaultTimeout = std::chrono::seconds(10);

    QuickjsEngine();
    ~QuickjsEngine();

    QuickjsEngine(const QuickjsEngine &) = delete;
    QuickjsEngine &operator=(const QuickjsEngine &) = delete;

    // 以 ES Module 方式加载插件源码(UTF-8)。每个实例只能加载一次。
    // 失败时把语法 / 运行时错误格式化写入 *error(UTF-8)并返回 false。
    bool loadModuleSource(const std::string &utf8Source, const std::string &filename,
                          std::string *error);

    // 取模块命名空间中的具名导出(如 "GetBattery")。
    // 不存在返回 JS_UNDEFINED;返回值已加引用,调用方负责 JS_FreeValue。
    JSValue moduleExport(const char *name) const;

    // 设置本轮执行的截止时刻;超时后 interrupt handler 中断 JS 执行,
    // 引擎内所有 JS 调用以异常冒泡返回。
    void setDeadline(std::chrono::steady_clock::time_point deadline);
    bool pastDeadline() const;

    // 取出并格式化当前 pending 异常(name/message/stack)为 UTF-8 字符串。
    // 没有 pending 异常时返回 "(no exception)"。调用后异常被清除。
    std::string takePendingException();

    JSContext *context() const { return m_ctx; }

private:
    static int interruptHandler(JSRuntime *rt, void *opaque);

    JSRuntime *m_rt = nullptr;
    JSContext *m_ctx = nullptr;
    JSModuleDef *m_module = nullptr;
    std::chrono::steady_clock::time_point m_deadline{};
};
