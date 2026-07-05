#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "battery/BatteryDevice.h"

#include <QMainWindow>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class BatteryManager;
class QSystemTrayIcon;
class QAction;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(BatteryManager *manager, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    // 关闭窗口时拦截为最小化到托盘，而非真正退出。
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onDevicesUpdated(const QList<BatteryDevice> &devices);
    void onRefreshClicked();
    void onIntervalChanged(int index);
    void onTrayActivated();
    void onToggleVisible();
    void onQuit();

private:
    void setupTray();
    void setupConnections();
    void rebuildTable(const QList<BatteryDevice> &devices);
    void updateTray(const QList<BatteryDevice> &devices);
    void notifyLowBattery(const QList<BatteryDevice> &devices);

    Ui::MainWindow *ui;
    BatteryManager *m_manager;
    QSystemTrayIcon *m_tray = nullptr;
    QAction *m_toggleAction = nullptr;
    QAction *m_refreshAction = nullptr;
    QAction *m_quitAction = nullptr;

    // 已提醒过一次低电量的设备 id，避免重复打扰（电量回升后才会再次提醒）。
    QSet<QString> m_lowBatteryNotified;
    // 是否已通过 closeEvent 触发过首次最小化（用于首次关闭时给用户提示）。
    bool m_trayHintShown = false;
};

#endif // MAINWINDOW_H
