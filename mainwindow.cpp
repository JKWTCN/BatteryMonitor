#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "src/core/BatteryManager.h"
#include "util/AppSettings.h"
#include "util/DeviceSettings.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLayout>
#include <QLayoutItem>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QComboBox>
#include <QDateTime>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QLabel>

namespace
{
// 按档位返回电量进度条配色。
QColor levelColor(BatteryLevel level)
{
    switch (level) {
    case BatteryLevel::Full:
        return QColor(76, 175, 80);   // 绿
    case BatteryLevel::Medium:
        return QColor(76, 175, 80);   // 绿（≥50 仍属安全）
    case BatteryLevel::Low:
        return QColor(255, 152, 0);   // 橙
    case BatteryLevel::Empty:
        return QColor(244, 67, 54);   // 红
    case BatteryLevel::Unknown:
    default:
        return QColor(158, 158, 158); // 灰
    }
}

// 仅供视觉一致的档位代表值（Xbox 进度条填充用，标签仍显示档位文本）。
int levelRepresentativePercent(BatteryLevel level)
{
    switch (level) {
    case BatteryLevel::Full:   return 100;
    case BatteryLevel::Medium: return 55;
    case BatteryLevel::Low:    return 25;
    case BatteryLevel::Empty:  return 5;
    default:                   return 0;
    }
}

// 离散档位的可读文本。
QString levelText(BatteryLevel level)
{
    switch (level) {
    case BatteryLevel::Full:   return MainWindow::tr("Full");
    case BatteryLevel::Medium: return MainWindow::tr("Medium");
    case BatteryLevel::Low:    return MainWindow::tr("Low");
    case BatteryLevel::Empty:  return MainWindow::tr("Empty");
    default:                   return MainWindow::tr("Unknown");
    }
}

// 电量列显示文本：AirPods 三路电量；蓝牙精确百分比；Xbox 档位名；有线单独标注。
QString batteryText(const BatteryDevice &device)
{
    if (device.wired) {
        return MainWindow::tr("Wired");
    }
    // AirPods / Beats：显示左/右/盒三路电量，未知路显示为 "-"。
    if (device.subType == BatteryDevice::SubType::AirPods) {
        const auto fmt = [](int v) {
            return (v >= 0) ? QString::number(v) + QLatin1Char('%')
                            : QStringLiteral("-");
        };
        QString text = QStringLiteral("L:") + fmt(device.leftPercent) +
                       QStringLiteral("  R:") + fmt(device.rightPercent);
        if (device.casePercent >= 0) {
            text += QStringLiteral("  Case:") + fmt(device.casePercent);
        }
        if (device.charging) {
            text += QStringLiteral(" ⚡");
        }
        return text;
    }
    if (device.percentage >= 0) {
        return QString::number(device.percentage) + QLatin1Char('%');
    }
    return levelText(device.level);
}

// 状态列文本。
QString statusText(const BatteryDevice &device)
{
    if (device.stale) {
        // 计算距上次成功读到该设备过了多少秒；负值 / 0 兜底为 0。
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 ageSec = (now - device.lastSeenMsecs) / 1000;
        return MainWindow::tr("Stale (last seen %1s ago)")
            .arg(ageSec > 0 ? ageSec : 0);
    }
    if (device.wired) {
        return MainWindow::tr("Wired power");
    }
    if (!device.connected) {
        return MainWindow::tr("Disconnected");
    }
    return MainWindow::tr("Connected");
}

// 程序化绘制电池形托盘 / 窗口图标，按最低档位设备着色，无需外部资源。
QIcon drawBatteryIcon(BatteryLevel level, int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QColor bodyColor = levelColor(level);
    const QColor edgeColor(245, 247, 251);

    const qreal margin = size * 0.18;
    const qreal bodyW = size - margin * 2;
    const qreal bodyH = bodyW * 1.6;
    const qreal bodyX = margin;
    const qreal bodyY = (size - bodyH) / 2;

    // 电池正极凸起。
    const qreal capW = bodyW * 0.4;
    const qreal capH = bodyH * 0.12;
    QRectF capRect(bodyX + (bodyW - capW) / 2, bodyY - capH, capW, capH);
    painter.setPen(Qt::NoPen);
    painter.setBrush(edgeColor);
    painter.drawRoundedRect(capRect, 2, 2);

    // 电池主体（描边）。
    QRectF bodyRect(bodyX, bodyY, bodyW, bodyH);
    painter.setBrush(QColor(30, 30, 35));
    painter.setPen(QPen(edgeColor, size * 0.06));
    painter.drawRoundedRect(bodyRect, 4, 4);

    // 电量填充。
    int fillPercent = levelRepresentativePercent(level);
    if (fillPercent > 0) {
        QRectF fillRect = bodyRect.adjusted(
            bodyRect.width() * 0.12,
            bodyRect.height() * 0.12,
            -bodyRect.width() * 0.12,
            -bodyRect.height() * 0.12);
        const qreal fillH = fillRect.height() * fillPercent / 100.0;
        fillRect.setTop(fillRect.bottom() - fillH);
        painter.setBrush(bodyColor);
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(fillRect, 2, 2);
    }

    return QIcon(pixmap);
}

// 找到列表中电量最低的设备档位，用于托盘图标着色。
BatteryLevel lowestLevel(const QList<BatteryDevice> &devices)
{
    BatteryLevel lowest = BatteryLevel::Unknown;
    for (const auto &device : devices) {
        if (device.wired) {
            continue;
        }
        if (device.level < lowest || lowest == BatteryLevel::Unknown) {
            lowest = device.level;
        }
    }
    return lowest;
}

// 轮询间隔下拉框选项 -> 毫秒。
int intervalFromIndex(int index)
{
    switch (index) {
    case 0:  return 5000;
    case 1:  return 10000;
    case 2:  return 30000;
    case 3:  return 60000;
    default: return 10000;
    }
}

// 粘性缓存保留下拉框选项 -> 秒。
// 0 = 从不缓存（瞬时失败即移除，等价改造前行为）。
int staleRetentionSecFromIndex(int index)
{
    switch (index) {
    case 0:  return 0;     // 从不缓存
    case 1:  return 60;    // 1 分钟
    case 2:  return 180;   // 3 分钟（默认）
    case 3:  return 300;   // 5 分钟
    default: return 180;
    }
}

QString percentText(int value)
{
    return value >= 0 ? QString::number(value) + QLatin1Char('%')
                      : MainWindow::tr("Unknown");
}

QString yesNoText(bool value)
{
    return value ? MainWindow::tr("Yes") : MainWindow::tr("No");
}

// 该设备当前是否已达到用户为它配置的低电量阈值。
//
// 调用方应先用 DeviceSettings::alertEnabled(id) 判断是否启用提醒；
// 本函数只负责“假设已启用，是否低于阈值”。
//
// 阈值是按“百分比”存储的；但部分设备只有离散档位（如 XInput 手柄，
// percentage = -1），无法与百分比阈值直接比较。对此采用保守映射：
//   Empty  -> 视为 5%
//   Low    -> 视为 25%
//   Medium -> 视为 55%
//   Full   -> 视为 100%
// 与 mainwindow.cpp 中 levelRepresentativePercent 保持一致。
// 当设备精确百分比 >= 0 时直接与阈值比较。
bool isAtOrBelowThreshold(const BatteryDevice &device, int threshold)
{
    // 阈值合法范围 1..100，<=0 视为关闭（防御性，正常路径走 alertEnabled）。
    if (threshold <= 0) {
        return false;
    }
    if (device.wired || !device.connected) {
        return false;
    }
    if (device.percentage >= 0) {
        return device.percentage <= threshold;
    }
    return levelRepresentativePercent(device.level) <= threshold;
}

// 策略 -> 最小重复间隔（秒）。
//   Once   返回 -1：特殊处理（用 m_lowBatteryNotified set，不查时间戳）。
//   Always 返回  0：不节流，每次刷新都允许提醒。
//   周期性 返回对应秒数：距上次提醒不足该值则不提醒。
int policyIntervalSec(AlertPolicy policy)
{
    switch (policy) {
    case AlertPolicy::Always:     return 0;
    case AlertPolicy::Every5Min:  return 5 * 60;
    case AlertPolicy::Every15Min: return 15 * 60;
    case AlertPolicy::Every30Min: return 30 * 60;
    case AlertPolicy::Every60Min: return 60 * 60;
    case AlertPolicy::Once:
    default:                      return -1;
    }
}

QLabel *makeValueLabel(const QString &text = QString())
{
    auto *label = new QLabel(text);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

// 重载：接受预创建的 title 标签 + 任意 value 控件（用于设置页的 ComboBox 行）。
// 这样后续 retranslateUi 可以直接 setText 已有 title 标签。
// 必须声明在 (const QString&, QLabel*) 重载之前，否则该重载内调用本重载时
// 编译器尚未看到它，会因重载决议失败而报错。
void addInfoRow(QVBoxLayout *layout, QLabel *titleLabel, QWidget *valueWidget)
{
    auto *row = new QWidget();
    row->setObjectName(QStringLiteral("infoRow"));
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(16, 10, 16, 10);
    rowLayout->setSpacing(18);

    titleLabel->setObjectName(QStringLiteral("rowTitle"));
    titleLabel->setMinimumWidth(120);
    titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    valueWidget->setObjectName(QStringLiteral("rowValue"));
    valueWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    rowLayout->addWidget(titleLabel);
    rowLayout->addWidget(valueWidget, 1);
    layout->addWidget(row);
}

void addInfoRow(QVBoxLayout *layout, const QString &title, QLabel *valueLabel)
{
    auto *titleLabel = new QLabel(title);
    addInfoRow(layout, titleLabel, valueLabel);
}

QFrame *makeInfoGroup()
{
    auto *group = new QFrame();
    group->setObjectName(QStringLiteral("detailGroup"));
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 4, 0, 4);
    layout->setSpacing(0);
    return group;
}
} // namespace

MainWindow::MainWindow(BatteryManager *manager, QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_manager(manager)
{
    ui->setupUi(this);
    setupPages();
    setMinimumSize(560, 380);

    // 表格基础外观。
    ui->deviceTable->setColumnCount(4);
    ui->deviceTable->horizontalHeader()->setStretchLastSection(false);
    ui->deviceTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->deviceTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->deviceTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    ui->deviceTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    ui->deviceTable->verticalHeader()->setDefaultSectionSize(56);
    ui->deviceTable->setShowGrid(false);
    ui->deviceTable->setAlternatingRowColors(false);
    ui->deviceTable->setRowCount(1);
    ui->deviceTable->setItem(0, 0, new QTableWidgetItem(tr("No devices detected")));

    setupTray();
    setupConnections();
    applyTheme();
    loadSettingsIntoUi();

    // 默认图标（未知/灰色）。
    setWindowIcon(drawBatteryIcon(BatteryLevel::Unknown, 64));
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::setupPages()
{
    auto *table = ui->deviceTable;
    QWidget *central = ui->centralwidget;
    QLayout *oldLayout = central->layout();

    if (oldLayout) {
        while (QLayoutItem *item = oldLayout->takeAt(0)) {
            if (QWidget *widget = item->widget()) {
                widget->setParent(nullptr);
            } else if (QLayout *childLayout = item->layout()) {
                while (QLayoutItem *childItem = childLayout->takeAt(0)) {
                    if (QWidget *widget = childItem->widget()) {
                        widget->setParent(nullptr);
                    }
                    delete childItem;
                }
            }
            delete item;
        }
    }

    central->setObjectName(QStringLiteral("centralwidget"));
    auto *rootLayout = oldLayout ? qobject_cast<QVBoxLayout *>(oldLayout) : nullptr;
    if (!rootLayout) {
        rootLayout = new QVBoxLayout(central);
    }
    rootLayout->setContentsMargins(18, 18, 18, 18);
    rootLayout->setSpacing(0);

    m_stack = new QStackedWidget(central);
    m_stack->setObjectName(QStringLiteral("contentStack"));
    rootLayout->addWidget(m_stack);

    m_listPage = new QWidget(m_stack);
    auto *listLayout = new QVBoxLayout(m_listPage);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(12);
    listLayout->addWidget(table, 1);

    auto *controlBar = new QWidget(m_listPage);
    controlBar->setObjectName(QStringLiteral("controlBar"));
    auto *controlLayout = new QHBoxLayout(controlBar);
    controlLayout->setContentsMargins(2, 0, 2, 0);
    controlLayout->setSpacing(10);
    controlLayout->addStretch(1);
    m_refreshButton = new QPushButton(tr("Refresh"));
    m_refreshButton->setObjectName(QStringLiteral("refreshButton"));
    m_refreshButton->setCursor(Qt::PointingHandCursor);
    m_settingsButton = new QPushButton(tr("Settings"));
    m_settingsButton->setObjectName(QStringLiteral("settingsButton"));
    m_settingsButton->setCursor(Qt::PointingHandCursor);
    controlLayout->addWidget(m_refreshButton);
    controlLayout->addWidget(m_settingsButton);
    listLayout->addWidget(controlBar);
    m_stack->addWidget(m_listPage);

    m_detailPage = new QWidget(m_stack);
    auto *detailLayout = new QVBoxLayout(m_detailPage);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(14);

    auto *navLayout = new QHBoxLayout();
    navLayout->setContentsMargins(0, 0, 0, 0);
    auto *backButton = new QPushButton(tr("Back"));
    backButton->setObjectName(QStringLiteral("backButton"));
    backButton->setCursor(Qt::PointingHandCursor);
    navLayout->addWidget(backButton);
    navLayout->addStretch(1);
    detailLayout->addLayout(navLayout);
    connect(backButton, &QPushButton::clicked, this, &MainWindow::showDeviceList);

    m_detailNameLabel = new QLabel();
    m_detailNameLabel->setObjectName(QStringLiteral("detailName"));
    m_detailNameLabel->setWordWrap(true);
    detailLayout->addWidget(m_detailNameLabel);

    auto *summaryLayout = new QHBoxLayout();
    summaryLayout->setContentsMargins(0, 0, 0, 0);
    summaryLayout->setSpacing(10);
    m_detailBatteryLabel = new QLabel();
    m_detailBatteryLabel->setObjectName(QStringLiteral("detailBattery"));
    m_detailStatusLabel = new QLabel();
    m_detailStatusLabel->setObjectName(QStringLiteral("detailStatus"));
    summaryLayout->addWidget(m_detailBatteryLabel);
    summaryLayout->addWidget(m_detailStatusLabel);
    summaryLayout->addStretch(1);
    detailLayout->addLayout(summaryLayout);

    auto *overviewGroup = makeInfoGroup();
    auto *overviewLayout = qobject_cast<QVBoxLayout *>(overviewGroup->layout());
    m_detailTypeValue = makeValueLabel();
    m_detailStateValue = makeValueLabel();
    m_detailBatteryValue = makeValueLabel();
    m_detailWiredValue = makeValueLabel();
    m_detailIdValue = makeValueLabel();
    addInfoRow(overviewLayout, tr("Type"), m_detailTypeValue);
    addInfoRow(overviewLayout, tr("Status"), m_detailStateValue);
    addInfoRow(overviewLayout, tr("Battery"), m_detailBatteryValue);
    addInfoRow(overviewLayout, tr("Wired"), m_detailWiredValue);
    addInfoRow(overviewLayout, tr("Device ID"), m_detailIdValue);
    detailLayout->addWidget(overviewGroup);

    m_airPodsGroup = makeInfoGroup();
    auto *airPodsLayout = qobject_cast<QVBoxLayout *>(m_airPodsGroup->layout());
    m_leftBatteryValue = makeValueLabel();
    m_rightBatteryValue = makeValueLabel();
    m_caseBatteryValue = makeValueLabel();
    m_chargingValue = makeValueLabel();
    addInfoRow(airPodsLayout, tr("Left battery"), m_leftBatteryValue);
    addInfoRow(airPodsLayout, tr("Right battery"), m_rightBatteryValue);
    addInfoRow(airPodsLayout, tr("Case battery"), m_caseBatteryValue);
    addInfoRow(airPodsLayout, tr("Charging"), m_chargingValue);
    detailLayout->addWidget(m_airPodsGroup);

    // —— 设备设置（每台设备持久化的偏好）——
    // 显示在设备信息页底部：是否加入托盘 / 是否启用低电量提醒 / 提醒阈值。
    // 控件值会随当前展示的设备切换而重新回填（refreshDetailPage 中处理）。
    m_deviceSettingsGroup = makeInfoGroup();
    auto *deviceSettingsLayout =
        qobject_cast<QVBoxLayout *>(m_deviceSettingsGroup->layout());
    m_deviceTrayRowTitle = new QLabel(tr("Show in tray"));
    m_deviceThresholdRowTitle = new QLabel(tr("Low battery alert"));
    m_deviceAlertPolicyRowTitle = new QLabel(tr("Repeat"));
    m_deviceTrayCheck = new QCheckBox();
    m_deviceTrayCheck->setObjectName(QStringLiteral("deviceTrayCheck"));
    m_deviceAlertCheck = new QCheckBox();
    m_deviceAlertCheck->setObjectName(QStringLiteral("deviceAlertCheck"));
    m_deviceThresholdSpin = new QSpinBox();
    m_deviceThresholdSpin->setObjectName(QStringLiteral("deviceThresholdSpin"));
    // 阈值范围 1..100：启用 / 关闭由独立的 m_deviceAlertCheck 控制，
    // 因此阈值本身不再允许 0。
    m_deviceThresholdSpin->setRange(1, DeviceSettings::kMaxThreshold);
    m_deviceThresholdSpin->setSuffix(QStringLiteral("%"));
    // 提醒策略下拉框：条目文本由 retranslateCombos() / retranslateAlertPolicyCombo()
    // 用 tr 填充，便于翻译；index 顺序固定对应 AlertPolicy 枚举（见下方映射）。
    m_deviceAlertPolicyCombo = new QComboBox();
    m_deviceAlertPolicyCombo->setObjectName(QStringLiteral("deviceAlertPolicyCombo"));
    m_deviceAlertPolicyCombo->addItem(
        tr("Once", "alert policy"), QVariant(static_cast<int>(AlertPolicy::Once)));
    m_deviceAlertPolicyCombo->addItem(
        tr("Every refresh", "alert policy"), QVariant(static_cast<int>(AlertPolicy::Always)));
    m_deviceAlertPolicyCombo->addItem(
        tr("Every 5 minutes", "alert policy"), QVariant(static_cast<int>(AlertPolicy::Every5Min)));
    m_deviceAlertPolicyCombo->addItem(
        tr("Every 15 minutes", "alert policy"), QVariant(static_cast<int>(AlertPolicy::Every15Min)));
    m_deviceAlertPolicyCombo->addItem(
        tr("Every 30 minutes", "alert policy"), QVariant(static_cast<int>(AlertPolicy::Every30Min)));
    m_deviceAlertPolicyCombo->addItem(
        tr("Every 60 minutes", "alert policy"), QVariant(static_cast<int>(AlertPolicy::Every60Min)));

    // “低电量提醒阈值”行的 value 列是个复合控件：左为复选框（启用开关），
    // 右为阈值数值框。复选框未勾选时阈值框灰显，避免产生“勾掉却还能调”的歧义。
    auto *alertValueWidget = new QWidget();
    auto *alertValueLayout = new QHBoxLayout(alertValueWidget);
    alertValueLayout->setContentsMargins(0, 0, 0, 0);
    alertValueLayout->setSpacing(10);
    alertValueLayout->addWidget(m_deviceAlertCheck);
    alertValueLayout->addWidget(m_deviceThresholdSpin, 1);

    addInfoRow(deviceSettingsLayout, m_deviceTrayRowTitle, m_deviceTrayCheck);
    addInfoRow(deviceSettingsLayout, m_deviceThresholdRowTitle, alertValueWidget);
    addInfoRow(deviceSettingsLayout, m_deviceAlertPolicyRowTitle, m_deviceAlertPolicyCombo);
    detailLayout->addWidget(m_deviceSettingsGroup);
    detailLayout->addStretch(1);

    m_stack->addWidget(m_detailPage);

    // —— 设置页（堆栈第三页）——
    m_settingsPage = new QWidget(m_stack);
    auto *settingsLayout = new QVBoxLayout(m_settingsPage);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->setSpacing(14);

    auto *settingsNavLayout = new QHBoxLayout();
    settingsNavLayout->setContentsMargins(0, 0, 0, 0);
    m_settingsBackButton = new QPushButton(tr("Back"));
    m_settingsBackButton->setObjectName(QStringLiteral("backButton"));
    m_settingsBackButton->setCursor(Qt::PointingHandCursor);
    settingsNavLayout->addWidget(m_settingsBackButton);
    settingsNavLayout->addStretch(1);
    settingsLayout->addLayout(settingsNavLayout);

    m_settingsTitleLabel = new QLabel(tr("Settings"));
    m_settingsTitleLabel->setObjectName(QStringLiteral("detailName"));
    m_settingsTitleLabel->setWordWrap(true);
    settingsLayout->addWidget(m_settingsTitleLabel);

    auto *settingsGroup = makeInfoGroup();
    auto *settingsGroupLayout = qobject_cast<QVBoxLayout *>(settingsGroup->layout());
    // 行标题先建占位，retranslateUi() 中会刷新翻译。
    m_intervalRowTitle = new QLabel(tr("Refresh interval"));
    m_languageRowTitle = new QLabel(tr("Language"));
    m_themeRowTitle = new QLabel(tr("Theme"));
    m_startupRowTitle = new QLabel(tr("Start with Windows"));
    m_staleRetentionRowTitle = new QLabel(tr("Stale retention"));
    m_intervalCombo = new QComboBox();
    m_intervalCombo->setObjectName(QStringLiteral("settingsCombo"));
    m_languageCombo = new QComboBox();
    m_languageCombo->setObjectName(QStringLiteral("settingsCombo"));
    m_themeCombo = new QComboBox();
    m_themeCombo->setObjectName(QStringLiteral("settingsCombo"));
    m_staleRetentionCombo = new QComboBox();
    m_staleRetentionCombo->setObjectName(QStringLiteral("settingsCombo"));
    m_startupCheck = new QCheckBox();
    m_startupCheck->setObjectName(QStringLiteral("startupCheck"));
    addInfoRow(settingsGroupLayout, m_intervalRowTitle, m_intervalCombo);
    addInfoRow(settingsGroupLayout, m_languageRowTitle, m_languageCombo);
    addInfoRow(settingsGroupLayout, m_themeRowTitle, m_themeCombo);
    addInfoRow(settingsGroupLayout, m_staleRetentionRowTitle, m_staleRetentionCombo);
    addInfoRow(settingsGroupLayout, m_startupRowTitle, m_startupCheck);
    settingsLayout->addWidget(settingsGroup);
    settingsLayout->addStretch(1);

    m_stack->addWidget(m_settingsPage);
    m_stack->setCurrentWidget(m_listPage);
}

void MainWindow::applyTheme()
{
    if (m_applyingTheme) {
        return;
    }
    m_applyingTheme = true;

    const QPalette pal = QApplication::palette();
    const bool dark = pal.color(QPalette::Window).lightness() < 128;

    const QString base = dark ? QStringLiteral("#1c1c1e") : QStringLiteral("#f5f5f7");
    const QString group = dark ? QStringLiteral("#2c2c2e") : QStringLiteral("#ffffff");
    const QString groupAlt = dark ? QStringLiteral("#242426") : QStringLiteral("#fbfbfd");
    const QString text = dark ? QStringLiteral("#f5f5f7") : QStringLiteral("#1d1d1f");
    const QString secondary = dark ? QStringLiteral("#a1a1a6") : QStringLiteral("#6e6e73");
    const QString border = dark ? QStringLiteral("#3a3a3c") : QStringLiteral("#d8d8dc");
    const QString hover = dark ? QStringLiteral("#343438") : QStringLiteral("#eeeeef");
    const QString accent = dark ? QStringLiteral("#0a84ff") : QStringLiteral("#007aff");

    setStyleSheet(QStringLiteral(
        "QWidget#centralwidget { background: %1; color: %4; }"
        "QWidget { color: %4; font-size: 10pt; }"
        "QStackedWidget#contentStack { background: transparent; }"
        "QTableWidget { background: %2; border: 1px solid %6; border-radius: 12px;"
        "  padding: 4px; gridline-color: transparent; outline: none; }"
        "QTableWidget::item { border: none; padding: 8px 10px; }"
        "QTableWidget::item:hover { background: %7; border-radius: 8px; }"
        "QHeaderView::section { background: %3; color: %5; border: none;"
        "  border-bottom: 1px solid %6; padding: 8px 10px; font-weight: 600; }"
        "QWidget#controlBar { background: transparent; }"
        "QLabel { color: %4; }"
        "QLabel#detailName { font-size: 23pt; font-weight: 700; }"
        "QLabel#detailBattery { color: %8; font-size: 12pt; font-weight: 700; }"
        "QLabel#detailStatus { color: %5; font-size: 12pt; }"
        "QLabel#rowTitle { color: %4; }"
        "QLabel#rowValue { color: %5; }"
        "QFrame#detailGroup { background: %2; border: 1px solid %6; border-radius: 12px; }"
        "QWidget#infoRow { background: transparent; }"
        "QPushButton { background: %2; color: %4; border: 1px solid %6; border-radius: 8px;"
        "  padding: 7px 14px; font-weight: 600; }"
        "QPushButton:hover { background: %7; }"
        "QPushButton#backButton { background: transparent; color: %8; border: none;"
        "  padding: 6px 2px; font-weight: 600; }"
        "QComboBox { background: %2; color: %4; border: 1px solid %6; border-radius: 8px;"
        "  padding: 6px 28px 6px 10px; }"
        "QComboBox:hover { background: %7; }"
        "QComboBox QAbstractItemView { background: %2; color: %4; border: 1px solid %6;"
        "  selection-background-color: %8; selection-color: white; }"
        "QSpinBox { background: %2; color: %4; border: 1px solid %6; border-radius: 8px;"
        "  padding: 6px 10px; }"
        "QSpinBox::up-button, QSpinBox::down-button { width: 18px; }"
        "QCheckBox { spacing: 8px; }"
        "QCheckBox::indicator { width: 18px; height: 18px; border-radius: 5px;"
        "  border: 1px solid %6; background: %2; }"
        "QCheckBox::indicator:hover { background: %7; }"
        "QCheckBox::indicator:checked { background: %8; border-color: %8; }")
        .arg(base, group, groupAlt, text, secondary, border, hover, accent));

    m_applyingTheme = false;
}

void MainWindow::setupTray()
{
    m_tray = new QSystemTrayIcon(drawBatteryIcon(BatteryLevel::Unknown, 32), this);
    m_tray->setToolTip(tr("Battery Monitor"));

    auto *menu = new QMenu(this);
    m_toggleAction = menu->addAction(tr("Hide"));
    m_refreshAction = menu->addAction(tr("Refresh"));
    m_settingsAction = menu->addAction(tr("Settings..."));
    menu->addSeparator();
    m_quitAction = menu->addAction(tr("Quit"));

    m_tray->setContextMenu(menu);
    m_tray->show();
}

void MainWindow::setupConnections()
{
    connect(m_manager, &BatteryManager::devicesUpdated,
            this, &MainWindow::onDevicesUpdated);

    connect(m_refreshButton, &QPushButton::clicked,
            this, &MainWindow::onRefreshClicked);
    connect(m_settingsButton, &QPushButton::clicked,
            this, &MainWindow::showSettingsPage);
    connect(m_settingsBackButton, &QPushButton::clicked,
            this, &MainWindow::showDeviceList);
    connect(m_intervalCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onIntervalChanged);
    connect(m_languageCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onLanguageChanged);
    connect(m_themeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onThemeChanged);
    connect(m_staleRetentionCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onStaleRetentionChanged);
    connect(m_startupCheck, &QCheckBox::toggled,
            this, &MainWindow::onStartupToggled);
    connect(ui->deviceTable, &QTableWidget::cellDoubleClicked,
            this, &MainWindow::showDeviceDetail);

    // 设备信息页“显示到托盘” / “启用提醒” / “阈值” / “策略”变化即写盘。
    connect(m_deviceTrayCheck, &QCheckBox::toggled,
            this, &MainWindow::onDeviceTrayVisibleChanged);
    connect(m_deviceAlertCheck, &QCheckBox::toggled,
            this, &MainWindow::onDeviceAlertEnabledChanged);
    connect(m_deviceThresholdSpin,
            qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onDeviceThresholdChanged);
    connect(m_deviceAlertPolicyCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onDeviceAlertPolicyChanged);

    connect(m_tray, &QSystemTrayIcon::activated,
            this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick
            || reason == QSystemTrayIcon::Trigger) {
            onTrayActivated();
        }
    });

    connect(m_toggleAction, &QAction::triggered, this, &MainWindow::onToggleVisible);
    connect(m_refreshAction, &QAction::triggered, this, &MainWindow::onRefreshClicked);
    connect(m_settingsAction, &QAction::triggered, this, &MainWindow::showSettingsPage);
    connect(m_quitAction, &QAction::triggered, this, &MainWindow::onQuit);
}

void MainWindow::onDevicesUpdated(const QList<BatteryDevice> &devices)
{
    m_devices = devices;
    rebuildTable(devices);
    refreshDetailPage();
    updateTray(devices);
    notifyLowBattery(devices);
}

void MainWindow::onRefreshClicked()
{
    if (m_manager) {
        m_manager->refreshNow();
    }
}

void MainWindow::onIntervalChanged(int index)
{
    const int msec = intervalFromIndex(index);
    AppSettings::setRefreshInterval(msec);
    if (m_manager) {
        m_manager->setInterval(msec);
    }
}

void MainWindow::onLanguageChanged(int index)
{
    // combo 顺序：0=跟随系统，1=en，2=zh_CN。
    QString code;
    switch (index) {
    case 1:  code = QStringLiteral("en"); break;
    case 2:  code = QStringLiteral("zh_CN"); break;
    default: code = QString(); break;  // 跟随系统
    }
    AppSettings::setLanguage(code);
    // 实际切换由 main.cpp 中的全局 retranslate() 完成，通过信号触发本类刷新。
    emit languageChanged(code);
}

void MainWindow::onThemeChanged(int index)
{
    // combo 顺序：0=跟随系统，1=浅色，2=深色。
    QString theme;
    switch (index) {
    case 1:  theme = QStringLiteral("light"); break;
    case 2:  theme = QStringLiteral("dark"); break;
    default: theme = QStringLiteral("system"); break;
    }
    AppSettings::setTheme(theme);
    // 实际 palette 应用由 main.cpp 中的全局 applyApplicationTheme() 完成。
    emit themeChanged(theme);
}

void MainWindow::onStartupToggled(bool checked)
{
    // 直接读写注册表 HKCU\...\Run。启用后开机时以 --minimized 静默启动。
    // 这里失败（如权限问题）暂不弹错——下一次打开设置页时复选框会反映真实状态。
    AppSettings::setStartupAutoStart(checked);
}

void MainWindow::onStaleRetentionChanged(int index)
{
    const int sec = staleRetentionSecFromIndex(index);
    AppSettings::setStaleRetentionSec(sec);
    if (m_manager) {
        m_manager->setStaleRetention(sec);
    }
}

void MainWindow::onTrayActivated()
{
    if (isHidden() || isMinimized()) {
        showNormal();
        activateWindow();
        raise();
    } else {
        hide();
    }
}

void MainWindow::onToggleVisible()
{
    if (isHidden() || isMinimized()) {
        showNormal();
        activateWindow();
        raise();
    } else {
        hide();
    }
}

void MainWindow::onQuit()
{
    // 清理托盘图标，避免退出后残留。
    if (m_tray) {
        m_tray->hide();
    }
    // 关闭窗口（关闭事件此时会被 ignore，因为 quitOnLastWindowClosed=false；
    // 因此直接请求应用退出）。
    QCoreApplication::exit(0);
}

void MainWindow::onDeviceTrayVisibleChanged(bool checked)
{
    if (m_currentDetailId.isEmpty()) {
        return;
    }
    DeviceSettings::setTrayVisible(m_currentDetailId, checked);
    // 立即反映到托盘（被取消勾选的设备从 tooltip / 最低档位计算中移除）。
    updateTray(m_devices);
}

void MainWindow::onDeviceAlertEnabledChanged(bool checked)
{
    if (m_currentDetailId.isEmpty()) {
        return;
    }
    DeviceSettings::setAlertEnabled(m_currentDetailId, checked);
    // 关闭提醒后阈值框 + 策略下拉框灰显；开启后可继续编辑。
    if (m_deviceThresholdSpin) {
        m_deviceThresholdSpin->setEnabled(checked);
    }
    if (m_deviceAlertPolicyCombo) {
        m_deviceAlertPolicyCombo->setEnabled(checked);
    }
    // notifyLowBattery 自身已具备完整状态机：
    //   - 关闭后设备会被移出 currentLow，已提醒记录随之清除；
    //   - 重新开启时若仍低于阈值，会作为“新出现的低电量”被提醒。
    // 因此这里不需要手动操作 m_lowBatteryNotified。
    notifyLowBattery(m_devices);
}

void MainWindow::onDeviceThresholdChanged(int value)
{
    if (m_currentDetailId.isEmpty()) {
        return;
    }
    DeviceSettings::setLowBatteryThreshold(m_currentDetailId, value);
    // 不能在这里 m_lowBatteryNotified.remove(id)！
    // notifyLowBattery 内部已会基于最新阈值重新判断：
    //   - 调高后仍低于阈值：设备在 m_lowBatteryNotified 里，不会重复提醒；
    //   - 调低到不再低于阈值：自动从 m_lowBatteryNotified 移除，回升后能再提醒。
    // 若在这里 remove，则每按一下数值框箭头都会清掉记录，触发一次提醒，
    // 形成“调阈值过程中不停响”的现象。
    notifyLowBattery(m_devices);
}

void MainWindow::onDeviceAlertPolicyChanged(int index)
{
    if (m_currentDetailId.isEmpty() || !m_deviceAlertPolicyCombo) {
        return;
    }
    // combo 的每个 item 都把对应 AlertPolicy 枚举值存进 QVariant(userData)。
    const QVariant data = m_deviceAlertPolicyCombo->itemData(index);
    if (!data.isValid()) {
        return;
    }
    const auto policy = static_cast<AlertPolicy>(data.toInt());
    DeviceSettings::setAlertPolicy(m_currentDetailId, policy);
    // 切换策略时清掉该设备的“上次提醒时间”，避免新策略受旧时间戳误伤：
    //   例如从 Every5Min 切回 Once 时，旧时间戳不应再被任何分支使用。
    m_lastAlertTime.remove(m_currentDetailId);
    notifyLowBattery(m_devices);
}

void MainWindow::showTrayHintOnce()
{
    if (m_trayHintShown) {
        return;
    }
    m_trayHintShown = true;
    if (m_tray) {
        m_tray->showMessage(
            tr("Battery Monitor"),
            tr("The application will keep running in the system tray. "
               "Use the tray menu to quit."),
            QSystemTrayIcon::Information, 3000);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 关闭窗口 -> 最小化到托盘（不退出）。仅“退出”菜单真正退出。
    if (m_tray && m_tray->isVisible()) {
        showTrayHintOnce();
        hide();
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (m_applyingTheme) {
        return;
    }
    if (event->type() == QEvent::ApplicationPaletteChange) {
        applyTheme();
        rebuildTable(m_devices);
        refreshDetailPage();
    }
}

void MainWindow::showDeviceDetail(int row)
{
    if (row < 0 || row >= m_devices.size()) {
        return;
    }

    const BatteryDevice &device = m_devices.at(row);
    const QString id = QString::fromStdWString(device.id);
    if (id.isEmpty()) {
        return;
    }

    m_currentDetailId = id;
    refreshDetailPage();
    if (m_stack && m_detailPage) {
        m_stack->setCurrentWidget(m_detailPage);
    }
}

void MainWindow::showDeviceList()
{
    m_currentDetailId.clear();
    if (m_stack && m_listPage) {
        m_stack->setCurrentWidget(m_listPage);
    }
}

void MainWindow::showSettingsPage()
{
    if (m_stack && m_settingsPage) {
        m_stack->setCurrentWidget(m_settingsPage);
    }
}

int MainWindow::intervalIndex(int msec) const
{
    switch (msec) {
    case 5000:  return 0;
    case 10000: return 1;
    case 30000: return 2;
    case 60000: return 3;
    default:    return 1;
    }
}

int MainWindow::staleRetentionIndex(int sec) const
{
    switch (sec) {
    case 0:   return 0;
    case 60:  return 1;
    case 180: return 2;
    case 300: return 3;
    default:  return 2; // 默认 3 分钟
    }
}

void MainWindow::loadSettingsIntoUi()
{
    // 先填入 combo 的可选项文本（带 tr，便于翻译）。
    retranslateCombos();

    const QSignalBlocker b1(m_intervalCombo);
    const QSignalBlocker b2(m_languageCombo);
    const QSignalBlocker b3(m_themeCombo);
    const QSignalBlocker b4(m_startupCheck);
    const QSignalBlocker b5(m_staleRetentionCombo);

    m_intervalCombo->setCurrentIndex(intervalIndex(AppSettings::refreshInterval()));

    const QString lang = AppSettings::language();
    int langIndex = 0;
    if (lang == QLatin1String("en")) langIndex = 1;
    else if (lang == QLatin1String("zh_CN")) langIndex = 2;
    m_languageCombo->setCurrentIndex(langIndex);

    const QString theme = AppSettings::theme();
    int themeIndex = 0;
    if (theme == QLatin1String("light")) themeIndex = 1;
    else if (theme == QLatin1String("dark")) themeIndex = 2;
    m_themeCombo->setCurrentIndex(themeIndex);

    m_staleRetentionCombo->setCurrentIndex(
        staleRetentionIndex(AppSettings::staleRetentionSec()));

    // 自启状态直接读注册表，与任务管理器显示保持一致。
    m_startupCheck->setChecked(AppSettings::startupAutoStart());

    // 让 manager 同步到持久化的间隔与粘性缓存窗口。
    if (m_manager) {
        m_manager->setInterval(AppSettings::refreshInterval());
        m_manager->setStaleRetention(AppSettings::staleRetentionSec());
    }
}

void MainWindow::retranslateCombos()
{
    if (m_intervalCombo) {
        m_intervalCombo->blockSignals(true);
        // 保留当前 index，仅替换条目文本。
        const int cur = m_intervalCombo->currentIndex();
        m_intervalCombo->clear();
        m_intervalCombo->addItem(tr("5 seconds"));
        m_intervalCombo->addItem(tr("10 seconds"));
        m_intervalCombo->addItem(tr("30 seconds"));
        m_intervalCombo->addItem(tr("60 seconds"));
        m_intervalCombo->setCurrentIndex(cur < 0 ? 1 : cur);
        m_intervalCombo->blockSignals(false);
    }
    if (m_languageCombo) {
        m_languageCombo->blockSignals(true);
        const int cur = m_languageCombo->currentIndex();
        m_languageCombo->clear();
        m_languageCombo->addItem(tr("System"));
        m_languageCombo->addItem(tr("English"));
        m_languageCombo->addItem(tr("Chinese (Simplified)"));
        m_languageCombo->setCurrentIndex(cur < 0 ? 0 : cur);
        m_languageCombo->blockSignals(false);
    }
    if (m_themeCombo) {
        m_themeCombo->blockSignals(true);
        const int cur = m_themeCombo->currentIndex();
        m_themeCombo->clear();
        m_themeCombo->addItem(tr("System"));
        m_themeCombo->addItem(tr("Light"));
        m_themeCombo->addItem(tr("Dark"));
        m_themeCombo->setCurrentIndex(cur < 0 ? 0 : cur);
        m_themeCombo->blockSignals(false);
    }
    if (m_staleRetentionCombo) {
        m_staleRetentionCombo->blockSignals(true);
        const int cur = m_staleRetentionCombo->currentIndex();
        m_staleRetentionCombo->clear();
        m_staleRetentionCombo->addItem(tr("Never cache"));
        m_staleRetentionCombo->addItem(tr("1 minute"));
        m_staleRetentionCombo->addItem(tr("3 minutes"));
        m_staleRetentionCombo->addItem(tr("5 minutes"));
        m_staleRetentionCombo->setCurrentIndex(cur < 0 ? 2 : cur);
        m_staleRetentionCombo->blockSignals(false);
    }
}

void MainWindow::retranslateUi()
{
    // 重译设置页静态控件文本。
    if (m_refreshButton) m_refreshButton->setText(tr("Refresh"));
    if (m_settingsButton) m_settingsButton->setText(tr("Settings"));
    if (m_settingsBackButton) m_settingsBackButton->setText(tr("Back"));
    if (m_settingsTitleLabel) m_settingsTitleLabel->setText(tr("Settings"));
    if (m_intervalRowTitle) m_intervalRowTitle->setText(tr("Refresh interval"));
    if (m_languageRowTitle) m_languageRowTitle->setText(tr("Language"));
    if (m_themeRowTitle) m_themeRowTitle->setText(tr("Theme"));
    if (m_startupRowTitle) m_startupRowTitle->setText(tr("Start with Windows"));
    if (m_staleRetentionRowTitle) m_staleRetentionRowTitle->setText(tr("Stale retention"));
    retranslateCombos();

    // 托盘菜单 / 图标提示。
    if (m_toggleAction) m_toggleAction->setText(isHidden() || isMinimized() ? tr("Show") : tr("Hide"));
    if (m_refreshAction) m_refreshAction->setText(tr("Refresh"));
    if (m_settingsAction) m_settingsAction->setText(tr("Settings..."));
    if (m_quitAction) m_quitAction->setText(tr("Quit"));
    if (m_tray) m_tray->setToolTip(tr("Battery Monitor"));

    // 设备信息页“设备设置”分组的标题与控件。
    if (m_deviceTrayRowTitle) m_deviceTrayRowTitle->setText(tr("Show in tray"));
    if (m_deviceThresholdRowTitle) m_deviceThresholdRowTitle->setText(tr("Low battery alert"));
    if (m_deviceAlertPolicyRowTitle) m_deviceAlertPolicyRowTitle->setText(tr("Repeat"));
    // 策略下拉框的条目文本需重译，但保留当前选中项与各 item 的 userData(策略枚举)。
    if (m_deviceAlertPolicyCombo) {
        const int cur = m_deviceAlertPolicyCombo->currentIndex();
        const QSignalBlocker b(m_deviceAlertPolicyCombo);
        m_deviceAlertPolicyCombo->setItemText(0, tr("Once", "alert policy"));
        m_deviceAlertPolicyCombo->setItemText(1, tr("Every refresh", "alert policy"));
        m_deviceAlertPolicyCombo->setItemText(2, tr("Every 5 minutes", "alert policy"));
        m_deviceAlertPolicyCombo->setItemText(3, tr("Every 15 minutes", "alert policy"));
        m_deviceAlertPolicyCombo->setItemText(4, tr("Every 30 minutes", "alert policy"));
        m_deviceAlertPolicyCombo->setItemText(5, tr("Every 60 minutes", "alert policy"));
        m_deviceAlertPolicyCombo->setCurrentIndex(cur);
    }

    // 设置窗口标题（也会随 tr 刷新）。
    setWindowTitle(tr("Battery Monitor"));

    // 重译动态内容：表格 + 详情页（其中所有可见文本都已用 tr 包装）。
    rebuildTable(m_devices);
    refreshDetailPage();
    updateTray(m_devices);
}

void MainWindow::refreshDetailPage()
{
    if (m_currentDetailId.isEmpty()) {
        return;
    }

    const BatteryDevice *current = nullptr;
    for (const auto &device : m_devices) {
        if (QString::fromStdWString(device.id) == m_currentDetailId) {
            current = &device;
            break;
        }
    }

    if (!current) {
        showDeviceList();
        return;
    }

    const BatteryDevice &device = *current;
    const QString name = QString::fromStdWString(device.name);
    const QString id = QString::fromStdWString(device.id);
    const bool isAirPods = device.subType == BatteryDevice::SubType::AirPods;

    m_detailNameLabel->setText(name.isEmpty() ? tr("Unknown device") : name);
    m_detailBatteryLabel->setText(batteryText(device));
    m_detailStatusLabel->setText(statusText(device));
    m_detailTypeValue->setText(BatteryManager::typeLabel(device.type));
    m_detailStateValue->setText(statusText(device));
    m_detailBatteryValue->setText(batteryText(device));
    m_detailWiredValue->setText(yesNoText(device.wired));
    m_detailIdValue->setText(id.isEmpty() ? tr("Unknown") : id);

    m_airPodsGroup->setVisible(isAirPods);
    if (isAirPods) {
        m_leftBatteryValue->setText(percentText(device.leftPercent));
        m_rightBatteryValue->setText(percentText(device.rightPercent));
        m_caseBatteryValue->setText(percentText(device.casePercent));
        m_chargingValue->setText(yesNoText(device.charging));
    }

    // —— 回填设备级设置（屏蔽信号，避免回调写盘）——
    if (m_deviceTrayCheck && m_deviceAlertCheck && m_deviceThresholdSpin &&
        m_deviceAlertPolicyCombo) {
        const QSignalBlocker b1(m_deviceTrayCheck);
        const QSignalBlocker b2(m_deviceAlertCheck);
        const QSignalBlocker b3(m_deviceThresholdSpin);
        const QSignalBlocker b4(m_deviceAlertPolicyCombo);
        m_deviceTrayCheck->setChecked(DeviceSettings::trayVisible(id));
        const bool alertOn = DeviceSettings::alertEnabled(id);
        m_deviceAlertCheck->setChecked(alertOn);
        m_deviceThresholdSpin->setValue(DeviceSettings::lowBatteryThreshold(id));
        // 复选框未勾选时阈值框 + 策略下拉框一并灰显（提醒都关了，策略也无意义）。
        m_deviceThresholdSpin->setEnabled(alertOn);
        m_deviceAlertPolicyCombo->setEnabled(alertOn);
        // 找到 itemData 等于当前策略的下标。
        const auto currentPolicy = static_cast<int>(DeviceSettings::alertPolicy(id));
        int policyIndex = 0;
        for (int i = 0; i < m_deviceAlertPolicyCombo->count(); ++i) {
            if (m_deviceAlertPolicyCombo->itemData(i).toInt() == currentPolicy) {
                policyIndex = i;
                break;
            }
        }
        m_deviceAlertPolicyCombo->setCurrentIndex(policyIndex);
    }
}

void MainWindow::rebuildTable(const QList<BatteryDevice> &devices)
{
    QTableWidget *table = ui->deviceTable;

    if (devices.isEmpty()) {
        table->clearContents();
        table->clearSpans();
        table->setRowCount(1);
        // 占位提示跨整行显示。
        auto *placeholder = new QTableWidgetItem(tr("No devices detected"));
        placeholder->setTextAlignment(Qt::AlignCenter);
        const QColor muted(160, 160, 160);
        placeholder->setForeground(muted);
        table->setSpan(0, 0, 1, 4);
        table->setItem(0, 0, placeholder);
        table->setItem(0, 1, new QTableWidgetItem());
        table->setItem(0, 2, new QTableWidgetItem());
        table->setItem(0, 3, new QTableWidgetItem());
        return;
    }

    table->clearSpans();
    table->clearContents();
    table->setRowCount(static_cast<int>(devices.size()));

    for (int row = 0; row < devices.size(); ++row) {
        const auto &device = devices.at(row);

        // stale（缓存沿用）设备整行标灰：电量数保留但视觉降级，提示非实时值。
        const QColor muted(160, 160, 160);

        auto *nameItem = new QTableWidgetItem(QString::fromStdWString(device.name));
        if (device.stale) {
            nameItem->setForeground(muted);
        }
        table->setItem(row, 0, nameItem);

        auto *typeItem = new QTableWidgetItem(BatteryManager::typeLabel(device.type));
        typeItem->setTextAlignment(Qt::AlignCenter);
        if (device.stale) {
            typeItem->setForeground(muted);
        }
        table->setItem(row, 1, typeItem);

        // 电量列：用进度条单元格直观展示。
        auto *bar = new QProgressBar();
        bar->setRange(0, 100);
        const bool hasPercent = device.percentage >= 0;
        const int displayPercent = hasPercent
            ? device.percentage
            : levelRepresentativePercent(device.level);
        bar->setValue(device.wired ? 100 : displayPercent);
        bar->setTextVisible(false);
        bar->setFixedHeight(16);

        // 进度条配色（通过 stylesheet 设置 chunk 颜色）。
        const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
        // stale 设备把 chunk 也降级为中性灰，进一步区分实时值与缓存值。
        const QColor color = device.stale
            ? muted
            : (device.wired ? QColor(142, 142, 147) : levelColor(device.level));
        const QString trackColor = dark ? QStringLiteral("#3a3a3c") : QStringLiteral("#e5e5ea");
        bar->setStyleSheet(QStringLiteral(
            "QProgressBar { background: %1; border-radius: 6px; }"
            "QProgressBar::chunk { background: %2; border-radius: 6px; }")
            .arg(trackColor, color.name()));

        // 用标签文字叠加在进度条上显示百分比 / 档位 / 有线。
        auto *overlay = new QLabel(batteryText(device));
        overlay->setAlignment(Qt::AlignCenter);
        const QString overlayColor = dark ? QStringLiteral("#f5f5f7") : QStringLiteral("#1d1d1f");
        overlay->setStyleSheet(QStringLiteral("color: %1; font-weight: %2;")
            .arg(device.stale ? muted.name() : overlayColor,
                 device.stale ? QStringLiteral("400") : QStringLiteral("600")));

        auto *cell = new QWidget();
        auto *cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(6, 2, 6, 2);
        cellLayout->setSpacing(0);
        cellLayout->addWidget(bar);
        cellLayout->addWidget(overlay);
        table->setCellWidget(row, 2, cell);

        auto *statusItem = new QTableWidgetItem(statusText(device));
        statusItem->setTextAlignment(Qt::AlignCenter);
        if (device.stale) {
            statusItem->setForeground(muted);
        }
        table->setItem(row, 3, statusItem);
    }

    table->resizeColumnsToContents();
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
}

void MainWindow::updateTray(const QList<BatteryDevice> &devices)
{
    // 仅把“勾选了显示到托盘”的设备纳入托盘展示。
    // 设备掉线重连后，因为偏好按 id 持久化，筛选结果保持一致。
    QList<BatteryDevice> visible;
    visible.reserve(devices.size());
    for (const auto &device : devices) {
        const QString id = QString::fromStdWString(device.id);
        if (DeviceSettings::trayVisible(id)) {
            visible.append(device);
        }
    }

    const BatteryLevel lowest = lowestLevel(visible);
    const QIcon icon = drawBatteryIcon(lowest, 32);
    m_tray->setIcon(icon);
    setWindowIcon(drawBatteryIcon(lowest, 64));

    if (visible.isEmpty()) {
        m_tray->setToolTip(tr("Battery Monitor - No devices"));
        return;
    }

    // tooltip 每设备一行。
    QStringList lines;
    lines.reserve(visible.size());
    for (const auto &device : visible) {
        // stale 设备在电量后追加「(stale)」标记，让用户一眼区分缓存值。
        QString suffix;
        if (device.stale) {
            suffix = QStringLiteral(" (") + tr("stale") + QStringLiteral(")");
        }
        lines << QStringLiteral("%1: %2%3")
                    .arg(QString::fromStdWString(device.name),
                         batteryText(device),
                         suffix);
    }
    m_tray->setToolTip(lines.join(QLatin1Char('\n')));
}

void MainWindow::notifyLowBattery(const QList<BatteryDevice> &devices)
{
    // —— 第一步：确定当前哪些设备“低于阈值”，同时拿到每台设备的策略 ——
    // currentLow: 本轮刷新中，达到提醒条件的设备集合（启用 alert 且电量 ≤ 阈值）。
    // 同时为每台设备读取策略，下面会按策略分流决定是否真的弹通知。
    QSet<QString> currentLow;
    for (const auto &device : devices) {
        // stale 设备的电量为缓存旧值，不代表实时状态：
        // 既不应据此新发提醒，也不应据此清理「已提醒」状态。直接跳过。
        if (device.stale) {
            continue;
        }
        const QString id = QString::fromStdWString(device.id);
        if (!DeviceSettings::alertEnabled(id)) {
            continue;
        }
        const int threshold = DeviceSettings::lowBatteryThreshold(id);
        if (isAtOrBelowThreshold(device, threshold)) {
            currentLow.insert(id);
        }
    }

    // —— 第二步：电量回升的设备清理状态 ——
    // 对所有策略统一适用：不再低于阈值时，重置它的“已提醒/时间戳”，
    // 这样下次再次跌破阈值时会重新走一次完整的提醒判断。
    for (auto it = m_lowBatteryNotified.begin(); it != m_lowBatteryNotified.end();) {
        if (!currentLow.contains(*it)) {
            it = m_lowBatteryNotified.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_lastAlertTime.begin(); it != m_lastAlertTime.end();) {
        if (!currentLow.contains(it.key())) {
            it = m_lastAlertTime.erase(it);
        } else {
            ++it;
        }
    }

    // —— 第三步：对仍处于低电量的设备，按各自策略决定是否弹通知 ——
    for (const auto &device : devices) {
        const QString id = QString::fromStdWString(device.id);
        if (!currentLow.contains(id)) {
            continue;
        }
        // stale 设备已在第一步被排除出 currentLow；防御性再判一次。
        if (device.stale) {
            continue;
        }
        const AlertPolicy policy = DeviceSettings::alertPolicy(id);
        const int intervalSec = policyIntervalSec(policy);

        // 策略分流：判断“本次是否应该提醒”。
        bool shouldNotify = false;
        if (intervalSec < 0) {
            // Once：首次跌破阈值时提醒一次，记录在 set 里，回升前不再响。
            if (!m_lowBatteryNotified.contains(id)) {
                shouldNotify = true;
                m_lowBatteryNotified.insert(id);
            }
        } else {
            // Always（0）/ 周期（>0）：基于“上次提醒时刻”节流。
            const QDateTime now = QDateTime::currentDateTime();
            const auto it = m_lastAlertTime.constFind(id);
            const bool due = (it == m_lastAlertTime.constEnd())
                || (it.value().secsTo(now) >= intervalSec);
            if (due) {
                shouldNotify = true;
                m_lastAlertTime.insert(id, now);
            }
        }

        if (shouldNotify) {
            m_tray->showMessage(
                tr("Low Battery"),
                tr("%1: %2").arg(QString::fromStdWString(device.name), batteryText(device)),
                QSystemTrayIcon::Warning, 4000);
        }
    }
}
