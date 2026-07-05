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
class QEvent;
class QFrame;
class QLabel;
class QStackedWidget;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(BatteryManager *manager, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    // 关闭窗口时拦截为最小化到托盘，而非真正退出。
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private slots:
    void onDevicesUpdated(const QList<BatteryDevice> &devices);
    void onRefreshClicked();
    void onIntervalChanged(int index);
    void onTrayActivated();
    void onToggleVisible();
    void onQuit();

private:
    void setupPages();
    void setupTray();
    void setupConnections();
    void applyTheme();
    void rebuildTable(const QList<BatteryDevice> &devices);
    void showDeviceDetail(int row);
    void showDeviceList();
    void refreshDetailPage();
    void updateTray(const QList<BatteryDevice> &devices);
    void notifyLowBattery(const QList<BatteryDevice> &devices);

    Ui::MainWindow *ui;
    BatteryManager *m_manager;
    QSystemTrayIcon *m_tray = nullptr;
    QAction *m_toggleAction = nullptr;
    QAction *m_refreshAction = nullptr;
    QAction *m_quitAction = nullptr;

    QStackedWidget *m_stack = nullptr;
    QWidget *m_listPage = nullptr;
    QWidget *m_detailPage = nullptr;
    QLabel *m_detailNameLabel = nullptr;
    QLabel *m_detailStatusLabel = nullptr;
    QLabel *m_detailBatteryLabel = nullptr;
    QLabel *m_detailTypeValue = nullptr;
    QLabel *m_detailStateValue = nullptr;
    QLabel *m_detailBatteryValue = nullptr;
    QLabel *m_detailWiredValue = nullptr;
    QLabel *m_detailIdValue = nullptr;
    QFrame *m_airPodsGroup = nullptr;
    QLabel *m_leftBatteryValue = nullptr;
    QLabel *m_rightBatteryValue = nullptr;
    QLabel *m_caseBatteryValue = nullptr;
    QLabel *m_chargingValue = nullptr;

    QList<BatteryDevice> m_devices;
    QString m_currentDetailId;
    bool m_applyingTheme = false;

    // 已提醒过一次低电量的设备 id，避免重复打扰（电量回升后才会再次提醒）。
    QSet<QString> m_lowBatteryNotified;
    // 是否已通过 closeEvent 触发过首次最小化（用于首次关闭时给用户提示）。
    bool m_trayHintShown = false;
};

#endif // MAINWINDOW_H
