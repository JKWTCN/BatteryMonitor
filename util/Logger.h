#pragma once

#include <string>

// 极简文件日志器（纯 C++ / Win32，零 Qt 依赖，便于跨项目复用）。
//
// 设计要点：
//   - 日志写入 %APPDATA%/BatteryMonitor/BatteryMonitor.log
//     （Windows 上即 C:/Users/<user>/AppData/Roaming/BatteryMonitor/）。
//   - 支持日志等级：Verbose / Info / Warning / Error，可设最小输出等级做过滤。
//   - 同时支持 std::string（UTF-8）与 std::wstring（宽字符，Win32/WinRT 常返回），
//     内部统一以 UTF-8 写入文件，不会乱码。
//   - 线程安全（SRWLock）；单文件超过 maxBytes 时截断重写。
//   - 不依赖任何第三方库，可被任意 Win32 C++ 项目直接复用。
//
// 等级说明：
//   - Verbose: 高频诊断明细（如每轮刷新的逐设备读数）。默认被丢弃，
//              仅在显式 setLevel(Verbose) 时才落盘——避免日志文件膨胀。
//   - Info:    启动 / 关键状态变化等“低频但有用”的信息。默认输出。
//   - Warning: 可恢复的异常情况。
//   - Error:   不可恢复的错误。
//
// 用法：
//   Logger::instance().log(L"消息");                    // std::wstring
//   Logger::instance().log("utf-8 msg");                // std::string
//   Logger::instance().log(L"msg", Logger::Level::Error);
//   Logger::instance().setLevel(Logger::Level::Warning); // 只看 Warning 及以上
//   Logger::instance().setLevel(Logger::Level::Verbose); // 打开全部明细（排障时）
//
//   便捷宏（同时提供窄/宽两套）：
//     LOG_VERBOSE("msg") LOG_VERBOSE_W(L"msg")   // 仅排障时打开
//     LOG("msg")        LOG_W(L"msg")
//     LOG_WARN("msg")   LOG_WARN_W(L"msg")
//     LOG_ERR("msg")    LOG_ERR_W(L"msg")
class Logger
{
public:
    enum class Level
    {
        Verbose = 0,
        Info = 1,
        Warning = 2,
        Error = 3
    };

    // 取得单例。首次写日志时惰性创建目录与文件。
    static Logger &instance();

    // 设置最小输出等级。低于该等级的日志会被丢弃。默认 Info（不含 Verbose）。
    // 必须在首次写日志前调用效果最佳（运行中修改也会即时生效）。
    void setLevel(Level level);

    // 设置日志目录名（%APPDATA% 下的子目录名），默认 "BatteryMonitor"。
    // 必须在首次写日志前调用。
    void setDirName(const std::wstring &dirName);

    // 设置日志文件名（不含目录），默认 "BatteryMonitor.log"。
    // 必须在首次写日志前调用。
    void setFileName(const std::wstring &fileName);

    // —— 写入接口（线程安全，重载一组方便复用）——
    // 宽字符（Win32/WinRT 友好）。
    void log(const std::wstring &message, Level level = Level::Info);
    // UTF-8 / ASCII 窄字符串。
    void log(const std::string &message, Level level = Level::Info);
    void log(const char *message, Level level = Level::Info);

private:
    Logger();
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    // 懒初始化：首次写日志时解析路径并确保目录存在。
    void ensureInitialized();

    // 解析最终日志文件路径（%APPDATA%/<dirName>/<fileName>）。
    std::wstring resolveFilePath();

    // 文件超过上限时清空重写。
    void rotateIfNeeded();

    // 实际写一行（调用前已加锁、已格式化为 UTF-8）。
    void writeLine(const std::string &utf8Line);

    // 把一行消息格式化（时间戳 + 等级 + 正文）为 UTF-8 字符串。
    std::string formatLine(const std::wstring &message, Level level);

    std::wstring m_filePath;
    std::wstring m_dirName = L"BatteryMonitor";
    std::wstring m_fileName = L"BatteryMonitor.log";
    unsigned long long m_maxBytes = 1024 * 1024; // 1MB 上限
    Level m_minLevel = Level::Info;
    bool m_initialized = false;
};

// —— 便捷宏 ——
// 窄字符串（std::string / const char* / 字符串字面量）。
#define LOG_VERBOSE(msg) Logger::instance().log((msg), Logger::Level::Verbose)
#define LOG(msg)         Logger::instance().log((msg), Logger::Level::Info)
#define LOG_WARN(msg)    Logger::instance().log((msg), Logger::Level::Warning)
#define LOG_ERR(msg)     Logger::instance().log((msg), Logger::Level::Error)

// 宽字符串（std::wstring / const wchar_t* / L"字面量"）。
#define LOG_VERBOSE_W(msg) Logger::instance().log((msg), Logger::Level::Verbose)
#define LOG_W(msg)         Logger::instance().log((msg), Logger::Level::Info)
#define LOG_WARN_W(msg)    Logger::instance().log((msg), Logger::Level::Warning)
#define LOG_ERR_W(msg)     Logger::instance().log((msg), Logger::Level::Error)
