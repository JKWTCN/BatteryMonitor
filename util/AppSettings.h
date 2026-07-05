#pragma once

#include <QString>

// 集中读写用户偏好（QSettings 的薄封装）。
//
// QApplication 在 main.cpp 中已设置 applicationName / organizationName
// （均为 "BatteryMonitor"），因此 QSettings 默认路径即可正确持久化，
// 无需手动指定路径或格式。
//
// 三个键：
//   - RefreshInterval : int 毫秒，默认 10000（10 秒）。
//   - Language        : QString 语言代码（如 "en"、"zh_CN"），空串表示跟随系统。
//   - Theme           : QString "system" / "light" / "dark"，默认 "system"。
//
// 全部为静态方法，任意线程调用均可（QSettings 自身线程安全）。
class AppSettings
{
public:
    // —— 刷新间隔（毫秒）——
    static int refreshInterval();
    static void setRefreshInterval(int msec);

    // —— 语言代码（空串 = 跟随系统）——
    static QString language();
    static void setLanguage(const QString &code);

    // —— 主题："system" / "light" / "dark" ——
    static QString theme();
    static void setTheme(const QString &theme);

private:
    AppSettings() = delete;
};
