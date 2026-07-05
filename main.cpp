#include "mainwindow.h"
#include "battery/AirPodsProvider.h"
#include "battery/BatteryManager.h"
#include "battery/BluetoothProvider.h"
#include "battery/ClassicBluetoothProvider.h"
#include "battery/XboxProvider.h"
#include "util/Logger.h"

#include <QApplication>
#include <QCoreApplication>
#include <QLocale>
#include <QTranslator>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setApplicationName(QStringLiteral("BatteryMonitor"));
    a.setOrganizationName(QStringLiteral("BatteryMonitor"));
    // 关闭窗口后仍允许托盘图标保活（不设置的话某些平台会退出应用）。
    a.setQuitOnLastWindowClosed(false);

    LOG_W(L"BatteryMonitor starting, exe dir: " +
          QCoreApplication::applicationDirPath().toStdWString());

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = "BatteryMonitor_" + QLocale(locale).name();
        if (translator.load(":/i18n/" + baseName)) {
            a.installTranslator(&translator);
            break;
        }
    }

    // 聚合所有电量提供者；未来适配 USB HID 时在此追加一个 addProvider 即可。
    //   BluetoothProvider      —— BLE GATT Battery Service（BLE 耳机/手环）。
    //   ClassicBluetoothProvider —— 经典蓝牙 A2DP/HFP 耳机（SetupAPI BTHENUM），
    //                              普通蓝牙耳机走这里。
    //   AirPodsProvider        —— AirPods/Beats（Apple Continuity 广播）。
    //   XboxProvider           —— XInput / RawGameController 手柄。
    BatteryManager manager;
    manager.addProvider(std::make_unique<BluetoothProvider>());
    manager.addProvider(std::make_unique<ClassicBluetoothProvider>());
    manager.addProvider(std::make_unique<AirPodsProvider>());
    manager.addProvider(std::make_unique<XboxProvider>());
    manager.start();

    MainWindow w(&manager);
    w.show();
    return QCoreApplication::exec();
}
