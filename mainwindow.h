#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "src/core/BatteryDevice.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QSet>
#include <QString>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class BatteryManager;
class BatteryHistoryChart;
class BatteryHistoryStore;
class RpcServer;
class QSystemTrayIcon;
class QAction;
class QCheckBox;
class QComboBox;
class QEvent;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QStackedWidget;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(BatteryManager *manager, BatteryHistoryStore *historyStore,
                        QWidget *parent = nullptr);
    ~MainWindow() override;

    // 注入 WebSocket RPC Server（可选）。设置页的 WebSocket 开关据此启动/停止。
    // 由 main.cpp 在构造后调用。
    void setRpcServer(RpcServer *server);

    // 语言切换后由外部（main.cpp 的翻译控制器）调用，重填所有静态控件文本。
    void retranslateUi();

    // 弹一次“程序在托盘里运行”的托盘通知。幂等：整个进程生命周期最多弹一次。
    // 由 main.cpp（--minimized 自启）和 closeEvent（首次最小化）共用。
    void showTrayHintOnce();

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
    void onStartupToggled(bool checked);
    // 设置页「缓存保留时长」下拉框被切换。
    void onStaleRetentionChanged(int index);
    // 设置页「隐藏未配对 AirPods」复选框被切换。
    void onHideUnpairedAirPodsChanged(bool checked);
    // 设置页「WebSocket 服务」启用复选框被切换。
    void onRpcEnabledToggled(bool checked);
    // 设置页 RPC 端口 / host / token 被修改。
    void onRpcPortChanged(int value);
    void onRpcHostEditingFinished();
    void onRpcTokenEditingFinished();
    void onTrayActivated();
    void onToggleVisible();
    void onQuit();
    // 设备信息页“显示到托盘”复选框被切换。
    void onDeviceTrayVisibleChanged(bool checked);
    // 设备信息页“低电量提醒启用”复选框被切换。
    void onDeviceAlertEnabledChanged(bool checked);
    // 设备信息页“低电量提醒阈值”数值被修改。
    void onDeviceThresholdChanged(int value);
    // 设备信息页“提醒策略”下拉框被切换。
    void onDeviceAlertPolicyChanged(int index);
    // 设备信息页“别名”输入框编辑完成。
    void onDeviceAliasEditingFinished();
    // 设备信息页“永久缓存”复选框被切换。
    void onDeviceKeepCacheChanged(bool checked);
    void onHistoryRangeChanged(int index);
    void onExportHistoryClicked();
    void onHistoryRetentionChanged(int index);
    void onHistoryChanged(const QString &deviceId);

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
    QString deviceDisplayName(const BatteryDevice &device) const;
    // 仅重填三个设置页 combo 的可选项文本（保留当前 index）。
    void retranslateCombos();
    // 把已保存的设置回填到三个设置页 combo。
    void loadSettingsIntoUi();
    // 把间隔 msec 反查为 combo index。
    int intervalIndex(int msec) const;
    // 把粘性缓存保留秒数反查为 combo index。
    int staleRetentionIndex(int sec) const;
    int historyRetentionIndex(int days) const;
    qint64 historyRangeStartMsecs() const;
    void refreshHistoryChart();

    Ui::MainWindow *ui;
    BatteryManager *m_manager;
    BatteryHistoryStore *m_historyStore = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QAction *m_toggleAction = nullptr;
    QAction *m_refreshAction = nullptr;
    QAction *m_settingsAction = nullptr;
    QAction *m_quitAction = nullptr;

    QStackedWidget *m_stack = nullptr;
    QWidget *m_listPage = nullptr;
    QLabel *m_listHintLabel = nullptr;
    QScrollArea *m_detailScrollArea = nullptr;
    QWidget *m_detailPage = nullptr;
    QScrollArea *m_settingsScrollArea = nullptr;
    QWidget *m_settingsPage = nullptr;
    QLabel *m_detailNameLabel = nullptr;
    QPushButton *m_detailBackButton = nullptr;
    QLabel *m_overviewSectionLabel = nullptr;
    QLabel *m_deviceSettingsSectionLabel = nullptr;
    QLabel *m_historySectionLabel = nullptr;
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
    QLabel *m_deviceAliasRowTitle = nullptr;
    QLabel *m_deviceTrayRowTitle = nullptr;
    QLabel *m_deviceThresholdRowTitle = nullptr;
    QLabel *m_deviceAlertPolicyRowTitle = nullptr;
    QLabel *m_deviceKeepCacheRowTitle = nullptr;
    QLineEdit *m_deviceAliasEdit = nullptr;
    QCheckBox *m_deviceTrayCheck = nullptr;
    QCheckBox *m_deviceAlertCheck = nullptr;
    QSpinBox *m_deviceThresholdSpin = nullptr;
    QComboBox *m_deviceAlertPolicyCombo = nullptr;
    QCheckBox *m_deviceKeepCacheCheck = nullptr;

    // —— 设备详情页历史图表 ——
    QFrame *m_historyGroup = nullptr;
    QLabel *m_historyTitleLabel = nullptr;
    QComboBox *m_historyRangeCombo = nullptr;
    QPushButton *m_exportHistoryButton = nullptr;
    QLabel *m_historyLegendLabel = nullptr;
    BatteryHistoryChart *m_historyChart = nullptr;

    // —— 设置页控件 ——
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_settingsButton = nullptr;
    QPushButton *m_settingsBackButton = nullptr;
    QLabel *m_settingsTitleLabel = nullptr;
    QLabel *m_generalSectionLabel = nullptr;
    QLabel *m_connectionsSectionLabel = nullptr;
    QLabel *m_aboutSectionLabel = nullptr;
    QLabel *m_intervalRowTitle = nullptr;
    QLabel *m_languageRowTitle = nullptr;
    QLabel *m_themeRowTitle = nullptr;
    QLabel *m_startupRowTitle = nullptr;
    QLabel *m_staleRetentionRowTitle = nullptr;
    QLabel *m_hideUnpairedAirPodsRowTitle = nullptr;
    QLabel *m_historyRetentionRowTitle = nullptr;
    QLabel *m_versionRowTitle = nullptr;
    QLabel *m_versionValue = nullptr;
    QLabel *m_projectRowTitle = nullptr;
    QLabel *m_projectLinkValue = nullptr;
    QComboBox *m_intervalCombo = nullptr;
    QComboBox *m_languageCombo = nullptr;
    QComboBox *m_themeCombo = nullptr;
    QComboBox *m_staleRetentionCombo = nullptr;
    QCheckBox *m_startupCheck = nullptr;
    QCheckBox *m_hideUnpairedAirPodsCheck = nullptr;
    QComboBox *m_historyRetentionCombo = nullptr;

    // —— 设置页 WebSocket RPC 行控件 ——
    QLabel *m_rpcRowTitle = nullptr;
    QCheckBox *m_rpcEnabledCheck = nullptr;
    QLabel *m_rpcPortRowTitle = nullptr;
    QSpinBox *m_rpcPortSpin = nullptr;
    QLabel *m_rpcHostRowTitle = nullptr;
    QLineEdit *m_rpcHostEdit = nullptr;
    QLabel *m_rpcTokenRowTitle = nullptr;
    QLineEdit *m_rpcTokenEdit = nullptr;
    QLabel *m_rpcStatusValue = nullptr;

    RpcServer *m_rpcServer = nullptr;

    QList<BatteryDevice> m_devices;
    QString m_currentDetailId;
    bool m_applyingTheme = false;

    // 已提醒过一次低电量的设备 id，避免重复打扰（电量回升后才会再次提醒）。
    // 仅 AlertPolicy::Once 使用；周期性策略改用 m_lastAlertTime 控制间隔。
    QSet<QString> m_lowBatteryNotified;
    // 设备 id -> 上次低电量提醒的时刻（本地时间）。
    // 用于 Always / EveryNMin 策略做节流。Once 不查这张表。
    QHash<QString, QDateTime> m_lastAlertTime;
    // 是否已通过 closeEvent 触发过首次最小化（用于首次关闭时给用户提示）。
    bool m_trayHintShown = false;
};

#endif // MAINWINDOW_H
