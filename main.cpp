#include "mainwindow.h"
#include "src/core/BatteryManager.h"
#include "src/providers/bluetooth/AirPodsProvider.h"
#include "src/providers/bluetooth/BluetoothProvider.h"
#include "src/providers/bluetooth/ClassicBluetoothProvider.h"
#include "src/providers/hid/AulaHidProvider.h"
#include "src/providers/hid/RazerHidProvider.h"
#include "src/providers/hid/VgnHidProvider.h"
#include "src/providers/xbox/XboxProvider.h"
#include "util/AppSettings.h"
#include "util/Logger.h"
#include "GeneratedAppVersion.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QLocale>
#include <QPalette>
#include <QTranslator>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace
{
// 应用全局唯一翻译器（堆上，生命周期与 QApplication 相同）。
// 切换语言时由 retranslate() 卸载并重新 load + install。
QTranslator *g_translator = nullptr;

#ifdef Q_OS_WIN
constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\BatteryMonitor.SingleInstance";
constexpr wchar_t kShowMainWindowMessageName[] = L"BatteryMonitor.ShowMainWindow";

class SingleInstanceGuard
{
public:
    SingleInstanceGuard()
        : m_message(RegisterWindowMessageW(kShowMainWindowMessageName))
    {
        m_mutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
        const DWORD error = GetLastError();
        m_hasRunningInstance =
            (m_mutex && error == ERROR_ALREADY_EXISTS) ||
            (!m_mutex && error == ERROR_ACCESS_DENIED);
    }

    ~SingleInstanceGuard()
    {
        if (m_mutex) {
            CloseHandle(m_mutex);
        }
    }

    bool hasRunningInstance() const { return m_hasRunningInstance; }
    UINT showMainWindowMessage() const { return m_message; }

    void notifyRunningInstance() const
    {
        if (m_message != 0) {
            PostMessageW(HWND_BROADCAST, m_message, 0, 0);
        }
    }

private:
    HANDLE m_mutex = nullptr;
    UINT m_message = 0;
    bool m_hasRunningInstance = false;
};

void showExistingMainWindow(MainWindow *window)
{
    if (!window) {
        return;
    }
    window->showNormal();
    window->raise();
    window->activateWindow();
}

class ShowMainWindowEventFilter : public QAbstractNativeEventFilter
{
public:
    ShowMainWindowEventFilter(MainWindow *window, UINT message)
        : m_window(window), m_message(message)
    {
    }

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override
    {
        Q_UNUSED(eventType);
        Q_UNUSED(result);

        if (m_message == 0 || !message) {
            return false;
        }

        const auto *msg = static_cast<MSG *>(message);
        if (msg->message != m_message) {
            return false;
        }

        showExistingMainWindow(m_window);
        return true;
    }

private:
    MainWindow *m_window = nullptr;
    UINT m_message = 0;
};
#endif

// 把指定语言代码加载为当前翻译。
//   code 为空 -> 跟随系统（依次试 QLocale::system().uiLanguages()）。
//   code 非空 -> 直接尝试 "BatteryMonitor_<code>"。
void retranslate(const QString &code)
{
    if (!g_translator) {
        return;
    }
    QCoreApplication::removeTranslator(g_translator);

    bool loaded = false;
    if (code.isEmpty()) {
        // 跟随系统：选首个能 load 的翻译。
        const QStringList uiLanguages = QLocale::system().uiLanguages();
        for (const QString &locale : uiLanguages) {
            const QString baseName = "BatteryMonitor_" + QLocale(locale).name();
            if (g_translator->load(":/i18n/" + baseName)) {
                loaded = true;
                break;
            }
        }
    } else {
        loaded = g_translator->load(":/i18n/BatteryMonitor_" + code);
    }

    if (loaded) {
        QCoreApplication::installTranslator(g_translator);
    }
}

// 构造与 mainwindow.cpp applyTheme() 中深色配色一致的深色调色板，
// 浅色用 Qt 默认 palette。
QPalette paletteForTheme(const QString &theme)
{
    if (theme == QLatin1String("dark")) {
        QPalette pal;
        pal.setColor(QPalette::Window, QColor(0x1c, 0x1c, 0x1e));
        pal.setColor(QPalette::WindowText, QColor(0xf5, 0xf5, 0xf7));
        pal.setColor(QPalette::Base, QColor(0x1c, 0x1c, 0x1e));
        pal.setColor(QPalette::AlternateBase, QColor(0x24, 0x24, 0x26));
        pal.setColor(QPalette::Text, QColor(0xf5, 0xf5, 0xf7));
        pal.setColor(QPalette::Button, QColor(0x2c, 0x2c, 0x2e));
        pal.setColor(QPalette::ButtonText, QColor(0xf5, 0xf5, 0xf7));
        pal.setColor(QPalette::BrightText, Qt::white);
        pal.setColor(QPalette::Highlight, QColor(0x0a, 0x84, 0xff));
        pal.setColor(QPalette::HighlightedText, Qt::white);
        pal.setColor(QPalette::ToolTipBase, QColor(0x2c, 0x2c, 0x2e));
        pal.setColor(QPalette::ToolTipText, QColor(0xf5, 0xf5, 0xf7));
        return pal;
    }
    if (theme == QLatin1String("light")) {
        QPalette pal;
        pal.setColor(QPalette::Window, QColor(0xf5, 0xf5, 0xf7));
        pal.setColor(QPalette::WindowText, QColor(0x1d, 0x1d, 0x1f));
        pal.setColor(QPalette::Base, QColor(0xff, 0xff, 0xff));
        pal.setColor(QPalette::AlternateBase, QColor(0xfb, 0xfb, 0xfd));
        pal.setColor(QPalette::Text, QColor(0x1d, 0x1d, 0x1f));
        pal.setColor(QPalette::Button, QColor(0xff, 0xff, 0xff));
        pal.setColor(QPalette::ButtonText, QColor(0x1d, 0x1d, 0x1f));
        pal.setColor(QPalette::BrightText, Qt::black);
        pal.setColor(QPalette::Highlight, QColor(0x00, 0x7a, 0xff));
        pal.setColor(QPalette::HighlightedText, Qt::white);
        pal.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xff));
        pal.setColor(QPalette::ToolTipText, QColor(0x1d, 0x1d, 0x1f));
        return pal;
    }
    // system: 恢复 Qt 默认 palette。
    return QPalette();
}

void applyApplicationTheme(const QString &theme)
{
    if (theme == QLatin1String("system")) {
        // 让 Qt 重新采用系统 palette。
        QApplication::setPalette(QPalette());
    } else {
        QApplication::setPalette(paletteForTheme(theme));
    }
    // QApplication::setPalette 会自动派发 ApplicationPaletteChange，
    // MainWindow::changeEvent 会据此刷新主题与表格。
}
} // namespace

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    SingleInstanceGuard singleInstance;
    if (singleInstance.hasRunningInstance()) {
        LOG_W(L"BatteryMonitor instance already running; notifying existing instance");
        singleInstance.notifyRunningInstance();
        return 0;
    }
#endif

    QApplication a(argc, argv);
    a.setApplicationName(QStringLiteral("BatteryMonitor"));
    a.setOrganizationName(QStringLiteral("BatteryMonitor"));
    a.setApplicationVersion(QStringLiteral(APP_VERSION_STRING));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
    // 关闭窗口后仍允许托盘图标保活（不设置的话某些平台会退出应用）。
    a.setQuitOnLastWindowClosed(false);

    LOG_W(L"BatteryMonitor starting, exe dir: " +
          QCoreApplication::applicationDirPath().toStdWString());
    LOG_W(L"BatteryMonitor arguments: " +
          QCoreApplication::arguments().join(QLatin1Char(' ')).toStdWString());

    // 翻译器：堆上分配，与 QApplication 同生命周期，便于运行时切换。
    g_translator = new QTranslator(&a);
    retranslate(AppSettings::language());

    // 启动时按已保存的主题应用 application palette。
    applyApplicationTheme(AppSettings::theme());

    // 聚合所有电量提供者；新增传输方式（如 USB 2.4G 接收器）时，
    // 只需在 src/providers/<传输方式>/ 下实现一个 IBatteryProvider，
    // 然后在此追加一行 addProvider，UI / 轮询 / 托盘逻辑无需改动。
    //   BluetoothProvider         —— BLE GATT Battery Service（BLE 耳机/手环）。
    //   ClassicBluetoothProvider  —— 经典蓝牙 A2DP/HFP 耳机（SetupAPI BTHENUM），
    //                                普通蓝牙耳机走这里。
    //   AirPodsProvider           —— AirPods/Beats（Apple Continuity 广播）。
    //   XboxProvider              —— XInput / RawGameController 手柄。
    //   AulaHidProvider           —— AULA 等 2.4G 接收器（HID Output/Input Report）。
    //   RazerHidProvider          —— Razer 鼠标 / 键盘的 HID 电量读取。
    //   VgnHidProvider            —— VGN / 关联品牌 2.4G 接收器键盘 / 鼠标，
    //                                按协议族分派（ThreeMode / Weisheng / Beiying /
    //                                VgnRyMouse / Yongjiaxin / VgnLdMs / Arbit / VgnKc2）。
    BatteryManager manager;
    manager.addProvider(std::make_unique<BluetoothProvider>());
    manager.addProvider(std::make_unique<ClassicBluetoothProvider>());
    manager.addProvider(std::make_unique<AirPodsProvider>());
    manager.addProvider(std::make_unique<XboxProvider>());
    manager.addProvider(std::make_unique<AulaHidProvider>());
    manager.addProvider(std::make_unique<RazerHidProvider>());
    manager.addProvider(std::make_unique<VgnHidProvider>());
    manager.start();

    MainWindow w(&manager);
#ifdef Q_OS_WIN
    ShowMainWindowEventFilter showMainWindowEventFilter(&w, singleInstance.showMainWindowMessage());
    a.installNativeEventFilter(&showMainWindowEventFilter);
#endif

    // --minimized：开机自启时由注册表命令行追加，让程序静默进入托盘，
    // 不弹主窗口。普通双击启动不带这个参数，正常显示窗口。
    const bool startMinimized = QCoreApplication::arguments()
        .contains(QStringLiteral("--minimized"), Qt::CaseInsensitive);
    LOG_W(std::wstring(L"BatteryMonitor start minimized: ") +
          (startMinimized ? L"true" : L"false"));
    if (startMinimized) {
        // 不调用 show*()，窗口保持隐藏；托盘图标已由 MainWindow 构造时建立。
        // 首次自启也提示一下用户程序在托盘里（避免“装了找不到”的困惑）。
        w.showTrayHintOnce();
    } else {
        w.showNormal();
        w.raise();
        w.activateWindow();
    }

    // 设置页语言/主题切换 -> 立即生效。
    //   languageChanged: 重新 load + install translator，再回调界面重译。
    //   themeChanged:    重新设置 application palette（changeEvent 自动联动）。
    QObject::connect(&w, &MainWindow::languageChanged, &a, [&w](const QString &code) {
        retranslate(code);
        w.retranslateUi();
    });
    QObject::connect(&w, &MainWindow::themeChanged, &a,
                     [](const QString &theme) { applyApplicationTheme(theme); });

    return QCoreApplication::exec();
}
