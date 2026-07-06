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
//   - StaleRetentionSec : int 秒，默认 180（3 分钟）。设备单轮读不到时，
//                         沿用上次读数继续展示的保留窗口；0 = 从不缓存。
//   - HideUnpairedAirPods : bool，默认 true。隐藏 BLE 广播中未与本机配对的
//                           AirPods / Beats。
//
// 开机自启单独说明：它直接读写 Windows 注册表的 HKCU\...\Run，
// 不进 QSettings。读取时也会兼容 Windows 任务管理器写入的
// Explorer\StartupApproved\Run 禁用状态。
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

    // —— 粘性缓存保留窗口（秒）——
    // 设备单轮读不到时，沿用上次读数继续展示的保留时长。
    // 0 = 从不缓存（瞬时失败即移除，等价旧行为）。
    static int staleRetentionSec();
    static void setStaleRetentionSec(int sec);

    // —— AirPods / Beats 广播过滤 ——
    // true = 只显示与本机已配对的 Apple 音频设备；false = 显示附近所有可解析广播。
    static bool hideUnpairedAirPods();
    static void setHideUnpairedAirPods(bool hide);

    // —— 开机自启 ——
    // 读取当前注册表中是否已配置开机自启。
    static bool startupAutoStart();
    // 启用/禁用开机自启。启用后会在命令行追加 "--minimized"，
    // 这样开机时程序直接进入托盘，不弹窗口。
    static bool setStartupAutoStart(bool enabled);

private:
    AppSettings() = delete;
};
