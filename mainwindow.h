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
class QCheckBox;
class QComboBox;
class QEvent;
class QFrame;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(BatteryManager *manager, QWidget *parent = nullptr);
    ~MainWindow() override;

    // 语言切换后由外部（main.cpp 的翻译控制器）调用，重填所有静态控件文本。
    void retranslateUi();

signals:
    // 用户在设置页修改语言 / 主题时发出，由 main.cpp 中的全局控制器接收，
    // 实际的 translator / palette 切换在那里完成，再回调用 retranslateUi()。
    void languageChanged(const QString &code);
    void themeChanged(const QString &theme);

protected:
    // 关闭窗口时拦截为最小化到托盘，而非真正退出。
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private slots:
    void onDevicesUpdated(const QList<BatteryDevice> &devices);
    void onRefreshClicked();
    void onIntervalChanged(int index);
    void onLanguageChanged(int index);
    void onThemeChanged(int index);
    void onTrayActivated();
    void onToggleVisible();
    void onQuit();
    // 设备信息页“显示到托盘”复选框被切换。
    void onDeviceTrayVisibleChanged(bool checked);
    // 设备信息页“低电量提醒启用”复选框被切换。
    void onDeviceAlertEnabledChanged(bool checked);
    // 设备信息页“低电量提醒阈值”数值被修改。
    void onDeviceThresholdChanged(int value);

private:
    void setupPages();
    void setupTray();
    void setupConnections();
    void applyTheme();
    void rebuildTable(const QList<BatteryDevice> &devices);
    void showDeviceDetail(int row);
    void showDeviceList();
    void showSettingsPage();
    void refreshDetailPage();
    void updateTray(const QList<BatteryDevice> &devices);
    void notifyLowBattery(const QList<BatteryDevice> &devices);
    // 仅重填三个设置页 combo 的可选项文本（保留当前 index）。
    void retranslateCombos();
    // 把已保存的设置回填到三个设置页 combo。
    void loadSettingsIntoUi();
    // 把间隔 msec 反查为 combo index。
    int intervalIndex(int msec) const;

    Ui::MainWindow *ui;
    BatteryManager *m_manager;
    QSystemTrayIcon *m_tray = nullptr;
    QAction *m_toggleAction = nullptr;
    QAction *m_refreshAction = nullptr;
    QAction *m_settingsAction = nullptr;
    QAction *m_quitAction = nullptr;

    QStackedWidget *m_stack = nullptr;
    QWidget *m_listPage = nullptr;
    QWidget *m_detailPage = nullptr;
    QWidget *m_settingsPage = nullptr;
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

    // —— 设备信息页的“设备设置”分组控件 ——
    // 与上方“只读信息行”不同：这里的 value 列是可交互控件。
    // 这些控件只对应“当前展示的设备”（m_currentDetailId）。
    QFrame *m_deviceSettingsGroup = nullptr;
    QLabel *m_deviceTrayRowTitle = nullptr;
    QLabel *m_deviceThresholdRowTitle = nullptr;
    QCheckBox *m_deviceTrayCheck = nullptr;
    QCheckBox *m_deviceAlertCheck = nullptr;
    QSpinBox *m_deviceThresholdSpin = nullptr;

    // —— 设置页控件 ——
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_settingsButton = nullptr;
    QPushButton *m_settingsBackButton = nullptr;
    QLabel *m_settingsTitleLabel = nullptr;
    QLabel *m_intervalRowTitle = nullptr;
    QLabel *m_languageRowTitle = nullptr;
    QLabel *m_themeRowTitle = nullptr;
    QComboBox *m_intervalCombo = nullptr;
    QComboBox *m_languageCombo = nullptr;
    QComboBox *m_themeCombo = nullptr;

    QList<BatteryDevice> m_devices;
    QString m_currentDetailId;
    bool m_applyingTheme = false;

    // 已提醒过一次低电量的设备 id，避免重复打扰（电量回升后才会再次提醒）。
    QSet<QString> m_lowBatteryNotified;
    // 是否已通过 closeEvent 触发过首次最小化（用于首次关闭时给用户提示）。
    bool m_trayHintShown = false;
};

#endif // MAINWINDOW_H
