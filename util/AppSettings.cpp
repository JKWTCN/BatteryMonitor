#include "AppSettings.h"

#include <QSettings>

#ifdef Q_OS_WIN
#include <QCoreApplication>
#include <QStringList>
#include <windows.h>
#endif

namespace
{
// 集中维护键名与默认值，避免拼写散落。
constexpr auto kKeyRefreshInterval = "RefreshInterval";
constexpr auto kKeyLanguage = "Language";
constexpr auto kKeyTheme = "Theme";

constexpr int kDefaultRefreshInterval = 10000;
const QString kDefaultLanguage = QString();      // 空 = 跟随系统
const QString kDefaultTheme = QStringLiteral("system");
} // namespace

int AppSettings::refreshInterval()
{
    return QSettings().value(kKeyRefreshInterval, kDefaultRefreshInterval).toInt();
}

void AppSettings::setRefreshInterval(int msec)
{
    QSettings().setValue(kKeyRefreshInterval, msec);
}

QString AppSettings::language()
{
    return QSettings().value(kKeyLanguage, kDefaultLanguage).toString();
}

void AppSettings::setLanguage(const QString &code)
{
    QSettings().setValue(kKeyLanguage, code);
}

QString AppSettings::theme()
{
    return QSettings().value(kKeyTheme, kDefaultTheme).toString();
}

void AppSettings::setTheme(const QString &theme)
{
    QSettings().setValue(kKeyTheme, theme);
}

// —— 开机自启 ——
//
// Windows 上把自身路径写到 HKCU\Software\Microsoft\Windows\CurrentVersion\Run，
// 由资源管理器在登录时拉起。HKCU 是当前用户级，无需管理员权限。
// 值写成带引号的 exe 路径 + " --minimized"，开机后程序据此直接进托盘。
//
// 注意：命令行参数用程序自己解析的 "--minimized"（main.cpp），不依赖系统语义。
#ifdef Q_OS_WIN
namespace
{
// 自启在注册表里的值名（用 applicationName，默认 "BatteryMonitor"）。
// 用 Qt 自身路径相关宏确保 Win64 / Win32 一致。
constexpr auto kRunKeyPath =
    R"(Software\Microsoft\Windows\CurrentVersion\Run)";

// 构造写到注册表的命令行："<exe 路径>" --minimized
// exe 路径含空格时必须用双引号包起来，否则 explorer 解析会失败。
QString autoStartCommand()
{
    const QString exe = QCoreApplication::applicationFilePath();
    return QStringLiteral("\"%1\" --minimized").arg(exe);
}
} // namespace
#endif

bool AppSettings::startupAutoStart()
{
#ifdef Q_OS_WIN
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      reinterpret_cast<LPCWSTR>(QString::fromUtf8(kRunKeyPath).utf16()),
                      0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }

    const QString valueName = QCoreApplication::applicationName();
    WCHAR buffer[MAX_PATH * 2] = {0};
    DWORD bufferSize = sizeof(buffer);
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(
        key,
        reinterpret_cast<LPCWSTR>(valueName.utf16()),
        nullptr, &type,
        reinterpret_cast<LPBYTE>(buffer), &bufferSize);
    RegCloseKey(key);

    if (status != ERROR_SUCCESS || type != REG_SZ) {
        return false;
    }
    // 只要键存在且非空就视为启用（不严格匹配命令行内容，
    // 这样用户从外部改过路径/参数也能正确显示为“已启用”）。
    return bufferSize > sizeof(WCHAR); // 至少有一个非结束符的字符
#else
    return false;
#endif
}

void AppSettings::setStartupAutoStart(bool enabled)
{
#ifdef Q_OS_WIN
    HKEY key = nullptr;
    const LSTATUS openStatus = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        reinterpret_cast<LPCWSTR>(QString::fromUtf8(kRunKeyPath).utf16()),
        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (openStatus != ERROR_SUCCESS) {
        return;
    }

    const QString valueName = QCoreApplication::applicationName();
    if (enabled) {
        const QString cmd = autoStartCommand();
        const auto cmdUtf16 = cmd.utf16();
        const DWORD byteLen = static_cast<DWORD>(cmd.size() * sizeof(WCHAR));
        RegSetValueExW(key,
                       reinterpret_cast<LPCWSTR>(valueName.utf16()),
                       0, REG_SZ,
                       reinterpret_cast<const BYTE*>(cmdUtf16),
                       byteLen);
    } else {
        RegDeleteValueW(key, reinterpret_cast<LPCWSTR>(valueName.utf16()));
    }
    RegCloseKey(key);
#else
    Q_UNUSED(enabled)
#endif
}
