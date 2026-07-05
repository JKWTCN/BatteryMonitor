#include "AppSettings.h"

#include <QSettings>

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
