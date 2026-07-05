#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "battery/BatteryManager.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QHeaderView>
#include <QIcon>
#include <QLinearGradient>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QComboBox>
#include <QSystemTrayIcon>
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
} // namespace

MainWindow::MainWindow(BatteryManager *manager, QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_manager(manager)
{
    ui->setupUi(this);

    // 表格基础外观。
    ui->deviceTable->setColumnCount(4);
    ui->deviceTable->horizontalHeader()->setStretchLastSection(false);
    ui->deviceTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->deviceTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->deviceTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    ui->deviceTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    ui->deviceTable->setRowCount(1);
    ui->deviceTable->setItem(0, 0, new QTableWidgetItem(tr("No devices detected")));

    // 默认 10 秒。
    ui->intervalCombo->setCurrentIndex(1);

    setupTray();
    setupConnections();

    // 默认图标（未知/灰色）。
    setWindowIcon(drawBatteryIcon(BatteryLevel::Unknown, 64));
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::setupTray()
{
    m_tray = new QSystemTrayIcon(drawBatteryIcon(BatteryLevel::Unknown, 32), this);
    m_tray->setToolTip(tr("Battery Monitor"));

    auto *menu = new QMenu(this);
    m_toggleAction = menu->addAction(tr("Hide"));
    m_refreshAction = menu->addAction(tr("Refresh"));
    menu->addSeparator();
    m_quitAction = menu->addAction(tr("Quit"));

    m_tray->setContextMenu(menu);
    m_tray->show();
}

void MainWindow::setupConnections()
{
    connect(m_manager, &BatteryManager::devicesUpdated,
            this, &MainWindow::onDevicesUpdated);

    connect(ui->refreshButton, &QPushButton::clicked,
            this, &MainWindow::onRefreshClicked);
    connect(ui->intervalCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onIntervalChanged);

    connect(m_tray, &QSystemTrayIcon::activated,
            this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick
            || reason == QSystemTrayIcon::Trigger) {
            onTrayActivated();
        }
    });

    connect(m_toggleAction, &QAction::triggered, this, &MainWindow::onToggleVisible);
    connect(m_refreshAction, &QAction::triggered, this, &MainWindow::onRefreshClicked);
    connect(m_quitAction, &QAction::triggered, this, &MainWindow::onQuit);
}

void MainWindow::onDevicesUpdated(const QList<BatteryDevice> &devices)
{
    rebuildTable(devices);
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
    if (m_manager) {
        m_manager->setInterval(intervalFromIndex(index));
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

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 关闭窗口 -> 最小化到托盘（不退出）。仅“退出”菜单真正退出。
    if (m_tray && m_tray->isVisible()) {
        if (!m_trayHintShown) {
            m_trayHintShown = true;
            m_tray->showMessage(
                tr("Battery Monitor"),
                tr("The application will keep running in the system tray. "
                   "Use the tray menu to quit."),
                QSystemTrayIcon::Information, 3000);
        }
        hide();
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::rebuildTable(const QList<BatteryDevice> &devices)
{
    QTableWidget *table = ui->deviceTable;

    if (devices.isEmpty()) {
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
    table->setRowCount(static_cast<int>(devices.size()));

    for (int row = 0; row < devices.size(); ++row) {
        const auto &device = devices.at(row);

        auto *nameItem = new QTableWidgetItem(QString::fromStdWString(device.name));
        table->setItem(row, 0, nameItem);

        auto *typeItem = new QTableWidgetItem(BatteryManager::typeLabel(device.type));
        typeItem->setTextAlignment(Qt::AlignCenter);
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
        const QColor color = device.wired ? QColor(120, 120, 120) : levelColor(device.level);
        bar->setStyleSheet(QStringLiteral(
            "QProgressBar { background: #2a2d33; border-radius: 6px; }"
            "QProgressBar::chunk { background: %1; border-radius: 6px; }")
            .arg(color.name()));

        // 用标签文字叠加在进度条上显示百分比 / 档位 / 有线。
        auto *overlay = new QLabel(batteryText(device));
        overlay->setAlignment(Qt::AlignCenter);
        overlay->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;")
            .arg(device.wired ? "#cfd3da" : "#ffffff"));

        auto *cell = new QWidget();
        auto *cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(6, 2, 6, 2);
        cellLayout->setSpacing(0);
        cellLayout->addWidget(bar);
        cellLayout->addWidget(overlay);
        table->setCellWidget(row, 2, cell);

        auto *statusItem = new QTableWidgetItem(statusText(device));
        statusItem->setTextAlignment(Qt::AlignCenter);
        table->setItem(row, 3, statusItem);
    }

    table->resizeColumnsToContents();
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
}

void MainWindow::updateTray(const QList<BatteryDevice> &devices)
{
    const BatteryLevel lowest = lowestLevel(devices);
    const QIcon icon = drawBatteryIcon(lowest, 32);
    m_tray->setIcon(icon);
    setWindowIcon(drawBatteryIcon(lowest, 64));

    if (devices.isEmpty()) {
        m_tray->setToolTip(tr("Battery Monitor - No devices"));
        return;
    }

    // tooltip 每设备一行。
    QStringList lines;
    lines.reserve(devices.size());
    for (const auto &device : devices) {
        lines << QStringLiteral("%1: %2")
                    .arg(QString::fromStdWString(device.name), batteryText(device));
    }
    m_tray->setToolTip(lines.join(QLatin1Char('\n')));
}

void MainWindow::notifyLowBattery(const QList<BatteryDevice> &devices)
{
    // 收集当前低电量设备。
    QSet<QString> currentLow;
    for (const auto &device : devices) {
        if (device.wired) {
            continue;
        }
        if (device.level == BatteryLevel::Empty || device.level == BatteryLevel::Low) {
            currentLow.insert(QString::fromStdWString(device.id));
        }
    }

    // 新出现的低电量设备 -> 提醒一次。
    for (const auto &device : devices) {
        const QString id = QString::fromStdWString(device.id);
        if (!currentLow.contains(id)) {
            continue;
        }
        if (m_lowBatteryNotified.contains(id)) {
            continue;
        }
        m_lowBatteryNotified.insert(id);
        m_tray->showMessage(
            tr("Low Battery"),
            tr("%1: %2").arg(QString::fromStdWString(device.name), batteryText(device)),
            QSystemTrayIcon::Warning, 4000);
    }

    // 电量回升后清掉记录，下次再低可以再次提醒。
    for (auto it = m_lowBatteryNotified.begin(); it != m_lowBatteryNotified.end();) {
        if (!currentLow.contains(*it)) {
            it = m_lowBatteryNotified.erase(it);
        } else {
            ++it;
        }
    }
}
