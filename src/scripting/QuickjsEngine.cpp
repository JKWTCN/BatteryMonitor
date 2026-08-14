#include "QuickjsEngine.h"

#include "util/Logger.h"

namespace
{
    // 沙箱前导:禁 Date、置常量随机源。运行在引擎创建后、插件加载前。
    constexpr char kSandboxPreamble[] =
        "Math.random = () => 0;\n"
        "delete globalThis.Date;\n";

    // 把 JSValue 转成 UTF-8 字符串副本(脱离引擎内存生命周期)。
    std::string toUtf8Copy(JSContext *ctx, JSValueConst val)
    {
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, val);
        if (!s)
        {
            return {};
        }
        std::string out(s, len);
        JS_FreeCString(ctx, s);
        return out;
    }
} // namespace

QuickjsEngine::QuickjsEngine()
    // 默认截止时刻从构造起算,避免 interrupt handler 对未设置 deadline 的
    // 引擎一律判超时(否则沙箱前导 / 模块加载会被立即误杀)。
    : m_deadline(std::chrono::steady_clock::now() + kDefaultTimeout)
{
    m_rt = JS_NewRuntime();
    if (!m_rt)
    {
        return;
    }
    JS_SetMemoryLimit(m_rt, kMemoryLimit);
    JS_SetMaxStackSize(m_rt, kMaxStackSize);
    // opaque 传 this,interrupt handler 据此检查本轮截止时刻。
    JS_SetInterruptHandler(m_rt, &QuickjsEngine::interruptHandler, this);

    m_ctx = JS_NewContext(m_rt);
    if (!m_ctx)
    {
        JS_FreeRuntime(m_rt);
        m_rt = nullptr;
        return;
    }

    // 不注册模块加载器:插件里的 import 语句会以 "could not load module"
    // 失败,维持「单文件即插件」的沙箱边界。
    JS_SetModuleLoaderFunc(m_rt, nullptr, nullptr, nullptr);

    JSValue result = JS_Eval(m_ctx, kSandboxPreamble, sizeof(kSandboxPreamble) - 1,
                             "<sandbox>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result))
    {
        // 前导代码只操作内建对象,理论上不会失败;真失败说明引擎状态异常。
        LOG_ERR("QuickjsEngine sandbox preamble failed: " + takePendingException());
    }
    JS_FreeValue(m_ctx, result);
}

QuickjsEngine::~QuickjsEngine()
{
    if (m_ctx)
    {
        JS_FreeContext(m_ctx);
    }
    if (m_rt)
    {
        JS_FreeRuntime(m_rt);
    }
}

bool QuickjsEngine::loadModuleSource(const std::string &utf8Source,
                                     const std::string &filename, std::string *error)
{
    if (!m_ctx || m_module)
    {
        if (error)
        {
            *error = !m_ctx ? "engine not initialized" : "module already loaded";
        }
        return false;
    }

    // 两步加载:先 COMPILE_ONLY 拿到模块对象(JS_TAG_MODULE 值),
    // 再 JS_EvalFunction 执行模块体。注意 quickjs-ng 执行顶层模块返回的是
    // 模块的 promise(同步模块当轮即 settle),用 JS_PromiseState 判断成败。
    JSValue val = JS_Eval(m_ctx, utf8Source.c_str(), utf8Source.size(), filename.c_str(),
                          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(val))
    {
        if (error)
        {
            *error = takePendingException();
        }
        return false;
    }
    if (!JS_IsModule(val))
    {
        if (error)
        {
            *error = "eval did not produce a module";
        }
        JS_FreeValue(m_ctx, val);
        return false;
    }
    m_module = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(val));

    // JS_EvalFunction 消耗 fun_obj(无需再 Free);返回值为模块 promise。
    JSValue promise = JS_EvalFunction(m_ctx, val);
    if (JS_IsException(promise))
    {
        m_module = nullptr;
        if (error)
        {
            *error = takePendingException();
        }
        return false;
    }

    std::string evalError;
    bool ok = false;
    switch (JS_PromiseState(m_ctx, promise))
    {
    case JS_PROMISE_FULFILLED:
        ok = true;
        break;
    case JS_PROMISE_REJECTED:
    {
        // 模块体抛出的异常被包进 promise rejection,取出作错误信息。
        JSValue reason = JS_PromiseResult(m_ctx, promise);
        if (JS_IsException(reason) || JS_IsNull(reason) || JS_IsUndefined(reason))
        {
            evalError = "module evaluation failed";
            if (JS_IsException(reason))
            {
                takePendingException(); // 清掉 pending 状态
            }
        }
        else
        {
            // 复用异常格式化:把 rejection reason 临时抛出再取回。
            JS_Throw(m_ctx, reason);
            evalError = takePendingException();
        }
        break;
    }
    default:
        evalError = "module evaluation did not settle "
                    "(top-level await is not supported)";
        break;
    }
    JS_FreeValue(m_ctx, promise);

    if (!ok)
    {
        m_module = nullptr;
        if (error)
        {
            *error = evalError;
        }
        return false;
    }
    return true;
}

JSValue QuickjsEngine::moduleExport(const char *name) const
{
    if (!m_ctx || !m_module)
    {
        return JS_UNDEFINED;
    }
    JSValue ns = JS_GetModuleNamespace(m_ctx, m_module);
    if (JS_IsException(ns))
    {
        return JS_UNDEFINED;
    }
    JSValue out = JS_GetPropertyStr(m_ctx, ns, name);
    JS_FreeValue(m_ctx, ns);
    return out;
}

void QuickjsEngine::setDeadline(std::chrono::steady_clock::time_point deadline)
{
    m_deadline = deadline;
}

bool QuickjsEngine::pastDeadline() const
{
    return std::chrono::steady_clock::now() >= m_deadline;
}

int QuickjsEngine::interruptHandler(JSRuntime *rt, void *opaque)
{
    (void)rt;
    const auto *self = static_cast<const QuickjsEngine *>(opaque);
    return self && self->pastDeadline() ? 1 : 0;
}

std::string QuickjsEngine::takePendingException()
{
    if (!m_ctx)
    {
        return "(engine not initialized)";
    }
    JSValue exc = JS_GetException(m_ctx);
    if (JS_IsNull(exc) || JS_IsUndefined(exc))
    {
        JS_FreeValue(m_ctx, exc);
        return "(no exception)";
    }

    std::string out;
    if (JS_IsError(exc))
    {
        const std::string name = toUtf8Copy(m_ctx,
                                            JS_GetPropertyStr(m_ctx, exc, "name"));
        JSValue msgVal = JS_GetPropertyStr(m_ctx, exc, "message");
        const std::string msg = toUtf8Copy(m_ctx, msgVal);
        JS_FreeValue(m_ctx, msgVal);
        out = name + ": " + msg;

        // stack 是数组(quickjs 的 Error.captureStackTrace),逐行拼接。
        JSValue stack = JS_GetPropertyStr(m_ctx, exc, "stack");
        if (JS_IsArray(stack))
        {
            int64_t len = 0;
            if (JS_GetLength(m_ctx, stack, &len) == 0 && len > 0)
            {
                out += "\nstack:";
                for (int64_t i = 0; i < len && i < 8; ++i)
                {
                    JSValue line = JS_GetPropertyUint32(
                        m_ctx, stack, static_cast<uint32_t>(i));
                    const std::string s = toUtf8Copy(m_ctx, line);
                    JS_FreeValue(m_ctx, line);
                    if (!s.empty())
                    {
                        out += "\n  " + s;
                    }
                }
            }
        }
        JS_FreeValue(m_ctx, stack);
    }
    else
    {
        // 非 Error 对象(如 throw "string")直接字符串化。
        out = toUtf8Copy(m_ctx, exc);
    }

    JS_FreeValue(m_ctx, exc);
    return out;
}
