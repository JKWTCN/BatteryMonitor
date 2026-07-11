#include "AppSettings.h"

#include <QSettings>

#ifdef Q_OS_WIN
#include <QCoreApplication>
#include <QDir>
#include <QStringList>
#include <windows.h>
#endif

namespace
{
// 集中维护键名与默认值，避免拼写散落。
constexpr auto kKeyRefreshInterval = "RefreshInterval";
constexpr auto kKeyLanguage = "Language";
constexpr auto kKeyTheme = "Theme";
constexpr auto kKeyStaleRetentionSec = "StaleRetentionSec";
constexpr auto kKeyHideUnpairedAirPods = "HideUnpairedAirPods";

constexpr int kDefaultRefreshInterval = 10000;
const QString kDefaultLanguage = QString();      // 空 = 跟随系统
const QString kDefaultTheme = QStringLiteral("system");
constexpr int kDefaultStaleRetentionSec = 180;   // 3 分钟
constexpr bool kDefaultHideUnpairedAirPods = true;
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

int AppSettings::staleRetentionSec()
{
    return QSettings().value(kKeyStaleRetentionSec, kDefaultStaleRetentionSec).toInt();
}

void AppSettings::setStaleRetentionSec(int sec)
{
    QSettings().setValue(kKeyStaleRetentionSec, sec);
}

bool AppSettings::hideUnpairedAirPods()
{
    return QSettings().value(kKeyHideUnpairedAirPods,
                             kDefaultHideUnpairedAirPods).toBool();
}

void AppSettings::setHideUnpairedAirPods(bool hide)
{
    QSettings().setValue(kKeyHideUnpairedAirPods, hide);
}

// —— WebSocket JSON-RPC Server ——
namespace
{
constexpr auto kKeyRpcEnabled = "RpcEnabled";
constexpr auto kKeyRpcHost = "RpcHost";
constexpr auto kKeyRpcPort = "RpcPort";
constexpr auto kKeyRpcToken = "RpcToken";
} // namespace

bool AppSettings::rpcEnabled()
{
    return QSettings().value(kKeyRpcEnabled, kDefaultRpcEnabled).toBool();
}

void AppSettings::setRpcEnabled(bool enabled)
{
    QSettings().setValue(kKeyRpcEnabled, enabled);
}

QString AppSettings::rpcHost()
{
    return QSettings().value(kKeyRpcHost, QString::fromLatin1(kDefaultRpcHost))
        .toString();
}

void AppSettings::setRpcHost(const QString &host)
{
    QSettings().setValue(kKeyRpcHost, host);
}

int AppSettings::rpcPort()
{
    const int port = QSettings().value(kKeyRpcPort, kDefaultRpcPort).toInt();
    if (port < kMinRpcPort || port > kMaxRpcPort) {
        return kDefaultRpcPort;
    }
    return port;
}

void AppSettings::setRpcPort(int port)
{
    QSettings().setValue(kKeyRpcPort, qBound(kMinRpcPort, port, kMaxRpcPort));
}

QString AppSettings::rpcToken()
{
    return QSettings().value(kKeyRpcToken).toString();
}

void AppSettings::setRpcToken(const QString &token)
{
    QSettings().setValue(kKeyRpcToken, token);
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
constexpr auto kStartupApprovedRunKeyPath =
    R"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)";

// 构造写到注册表的命令行："<exe 路径>" --minimized
// exe 路径含空格时必须用双引号包起来，否则 explorer 解析会失败。
QString autoStartCommand()
{
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return QStringLiteral("\"%1\" --minimized").arg(exe);
}

bool isStartupApprovedDisabled(const QString &valueName)
{
    HKEY key = nullptr;
    const QString keyPath = QString::fromUtf8(kStartupApprovedRunKeyPath);
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      reinterpret_cast<LPCWSTR>(keyPath.utf16()),
                      0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }

    BYTE buffer[16] = {0};
    DWORD bufferSize = sizeof(buffer);
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(
        key,
        reinterpret_cast<LPCWSTR>(valueName.utf16()),
        nullptr, &type,
        buffer, &bufferSize);
    RegCloseKey(key);

    // Windows 任务管理器会在 StartupApproved\Run 中记录启停状态；
    // 常见格式下首字节 0x03 表示禁用，0x02 表示启用。
    return status == ERROR_SUCCESS
        && type == REG_BINARY
        && bufferSize > 0
        && buffer[0] == 0x03;
}

void clearStartupApprovedState(const QString &valueName)
{
    HKEY key = nullptr;
    const QString keyPath = QString::fromUtf8(kStartupApprovedRunKeyPath);
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      reinterpret_cast<LPCWSTR>(keyPath.utf16()),
                      0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    RegDeleteValueW(key, reinterpret_cast<LPCWSTR>(valueName.utf16()));
    RegCloseKey(key);
}
} // namespace
#endif

bool AppSettings::startupAutoStart()
{
#ifdef Q_OS_WIN
    const QString valueName = QCoreApplication::applicationName();

    HKEY key = nullptr;
    const QString keyPath = QString::fromUtf8(kRunKeyPath);
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      reinterpret_cast<LPCWSTR>(keyPath.utf16()),
                      0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }

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
    if (isStartupApprovedDisabled(valueName)) {
        return false;
    }
    // 只要键存在且非空就视为启用（不严格匹配命令行内容，
    // 这样用户从外部改过路径/参数也能正确显示为“已启用”）。
    return bufferSize > sizeof(WCHAR); // 至少有一个非结束符的字符
#else
    return false;
#endif
}

bool AppSettings::setStartupAutoStart(bool enabled)
{
#ifdef Q_OS_WIN
    HKEY key = nullptr;
    const QString keyPath = QString::fromUtf8(kRunKeyPath);
    const LSTATUS openStatus = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        reinterpret_cast<LPCWSTR>(keyPath.utf16()),
        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (openStatus != ERROR_SUCCESS) {
        return false;
    }

    const QString valueName = QCoreApplication::applicationName();
    LSTATUS status = ERROR_SUCCESS;
    if (enabled) {
        const QString cmd = autoStartCommand();
        const auto cmdUtf16 = cmd.utf16();
        const DWORD byteLen = static_cast<DWORD>((cmd.size() + 1) * sizeof(WCHAR));
        status = RegSetValueExW(key,
                                reinterpret_cast<LPCWSTR>(valueName.utf16()),
                                0, REG_SZ,
                                reinterpret_cast<const BYTE*>(cmdUtf16),
                                byteLen);
    } else {
        status = RegDeleteValueW(key, reinterpret_cast<LPCWSTR>(valueName.utf16()));
        if (status == ERROR_FILE_NOT_FOUND) {
            status = ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    clearStartupApprovedState(valueName);
    return true;
#else
    Q_UNUSED(enabled)
    return false;
#endif
}
