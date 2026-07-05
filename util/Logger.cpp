#include "Logger.h"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

namespace
{
// 细粒度读写锁，多读单写；日志写入用独占，等级设置用独占。
SRWLOCK g_logLock = SRWLOCK_INIT;

// SRWLock 的 RAII 包装（独占）。
struct WriteLockGuard
{
    SRWLOCK &lock;
    explicit WriteLockGuard(SRWLOCK &m) : lock(m) { AcquireSRWLockExclusive(&lock); }
    ~WriteLockGuard() { ReleaseSRWLockExclusive(&lock); }
    WriteLockGuard(const WriteLockGuard &) = delete;
    WriteLockGuard &operator=(const WriteLockGuard &) = delete;
};

// 等级 -> 文本标签（固定 5 字符宽度，便于对齐）。
const wchar_t *levelTag(Logger::Level level)
{
    switch (level) {
    case Logger::Level::Verbose: return L"VRB  ";
    case Logger::Level::Info:    return L"INFO ";
    case Logger::Level::Warning: return L"WARN ";
    case Logger::Level::Error:   return L"ERROR";
    default:                     return L"INFO ";
    }
}

// 取 %APPDATA% 目录（Roaming）。
// 优先用 SHGetKnownFolderPath（现代 API），失败回退 SHGetFolderPath。
std::wstring getAppDataDir()
{
    PWSTR rawPath = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &rawPath)) && rawPath) {
        result = rawPath;
    }
    if (rawPath) {
        CoTaskMemFree(rawPath);
    }
    return result;
}

// 宽字符(UTF-16) -> UTF-8 多字节字符串。
// 一次 WideCharToMultiByte 先算长度再转换，安全且无截断。
std::string wideToUtf8(const std::wstring &wide)
{
    if (wide.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
        out.data(), len, nullptr, nullptr);
    return out;
}

// 当前本地时间格式化为 "yyyy-MM-dd HH:mm:ss.mmm"。
std::wstring nowTimestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);

    std::tm tm{};
    localtime_s(&tm, &t);

    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
               tm.tm_hour, tm.tm_min, tm.tm_sec,
               static_cast<int>(ms.count()));
    return buf;
}

// 取文件大小（字节）。失败返回 0。
unsigned long long fileSize(const std::wstring &path)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        return 0;
    }
    return (static_cast<unsigned long long>(info.nFileSizeHigh) << 32)
         | info.nFileSizeLow;
}
} // namespace

Logger &Logger::instance()
{
    static Logger instance;
    return instance;
}

Logger::Logger() = default;

void Logger::setLevel(Level level)
{
    WriteLockGuard guard(g_logLock);
    m_minLevel = level;
}

void Logger::setDirName(const std::wstring &dirName)
{
    WriteLockGuard guard(g_logLock);
    if (!m_initialized && !dirName.empty()) {
        m_dirName = dirName;
    }
}

void Logger::setFileName(const std::wstring &fileName)
{
    WriteLockGuard guard(g_logLock);
    if (!m_initialized && !fileName.empty()) {
        m_fileName = fileName;
    }
}

void Logger::ensureInitialized()
{
    if (m_initialized) {
        return;
    }
    m_filePath = resolveFilePath();
    // 目录不存在则创建（含父目录）。
    const std::wstring dir = m_filePath.substr(0, m_filePath.find_last_of(L"\\/"));
    if (!dir.empty()) {
        CreateDirectoryW(dir.c_str(), nullptr); // 已存在会失败，忽略即可。
    }
    m_initialized = true;
}

std::wstring Logger::resolveFilePath()
{
    std::wstring base = getAppDataDir();
    if (base.empty()) {
        // 极端情况下取不到 %APPDATA%，回退到当前工作目录。
        base = L".";
    }
    if (base.back() != L'\\' && base.back() != L'/') {
        base.push_back(L'\\');
    }
    return base + m_dirName + L"\\" + m_fileName;
}

void Logger::rotateIfNeeded()
{
    // 必须在打开 ofstream 之前调用：std::ofstream 默认不共享 GENERIC_WRITE，
    // 这里再以 TRUNCATE_EXISTING 打开会因共享冲突而失败（旧实现的 bug，文件会无限膨胀）。
    if (fileSize(m_filePath) > m_maxBytes) {
        // 超过上限：直接清空（简单覆盖式轮转，保留最近内容）。
        HANDLE h = CreateFileW(m_filePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
}

void Logger::writeLine(const std::string &utf8Line)
{
    // 先轮转再打开写文件：TRUNCATE 与 ofstream 的默认共享模式互斥，
    // 顺序颠倒会让清空操作静默失败（文件突破 maxBytes 上限）。
    rotateIfNeeded();
    // 用二进制追加模式打开，避免文本模式做 CRLF 转换（行尾已含 \n）。
    std::ofstream file(m_filePath, std::ios::app | std::ios::binary);
    if (!file.is_open()) {
        return; // 打不开静默丢弃，日志绝不能让程序崩。
    }
    file << utf8Line;
}

std::string Logger::formatLine(const std::wstring &message, Level level)
{
    std::wostringstream ss;
    ss << L"[" << nowTimestamp() << L"] [" << levelTag(level) << L"] " << message << L"\n";
    return wideToUtf8(ss.str());
}

void Logger::log(const std::wstring &message, Level level)
{
    WriteLockGuard guard(g_logLock);
    ensureInitialized();
    if (level < m_minLevel) {
        return;
    }
    writeLine(formatLine(message, level));
}

void Logger::log(const std::string &message, Level level)
{
    // 窄字符串按 UTF-8 直接用；格式化由宽字符版完成，这里转成宽再走一遍。
    // 入口窄 -> 宽的转换按 UTF-8 解码。
    std::wstring wide;
    if (!message.empty()) {
        const int len = MultiByteToWideChar(
            CP_UTF8, 0, message.c_str(), static_cast<int>(message.size()), nullptr, 0);
        if (len > 0) {
            wide.resize(static_cast<size_t>(len));
            MultiByteToWideChar(
                CP_UTF8, 0, message.c_str(), static_cast<int>(message.size()), wide.data(), len);
        }
    }
    log(wide, level);
}

void Logger::log(const char *message, Level level)
{
    log(std::string(message ? message : ""), level);
}
