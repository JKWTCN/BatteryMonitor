#include "BatteryHistoryChart.h"

#include <QDateTime>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>

#include <algorithm>

namespace
{
int percentageValue(const BatteryHistorySample &s) { return s.percentage; }
int leftValue(const BatteryHistorySample &s) { return s.leftPercent; }
int rightValue(const BatteryHistorySample &s) { return s.rightPercent; }
int caseValue(const BatteryHistorySample &s) { return s.casePercent; }
int levelValue(const BatteryHistorySample &s)
{
    const int value = static_cast<int>(s.level);
    return value >= static_cast<int>(BatteryLevel::Empty) &&
           value <= static_cast<int>(BatteryLevel::Full) ? value : -1;
}
} // namespace

BatteryHistoryChart::BatteryHistoryChart(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void BatteryHistoryChart::setSamples(const QList<BatteryHistorySample> &samples)
{
    m_samples = samples;
    m_errorText.clear();
    update();
}

void BatteryHistoryChart::setErrorText(const QString &text)
{
    m_errorText = text;
    update();
}

QSize BatteryHistoryChart::minimumSizeHint() const
{
    return QSize(420, 240);
}

int BatteryHistoryChart::yForValue(const QRectF &plot, int value, bool discrete) const
{
    const double ratio = discrete ? (value - 1) / 3.0 : value / 100.0;
    return qRound(plot.bottom() - qBound(0.0, ratio, 1.0) * plot.height());
}

void BatteryHistoryChart::drawSeries(QPainter &painter, const QRectF &plot,
                                     qint64 firstTime, qint64 lastTime,
                                     ValueGetter getter, const QColor &color) const
{
    const bool discrete = getter == levelValue;
    QPainterPath path;
    bool active = false;
    int lastPixelX = -1;
    int lastPixelY = -1;
    qint64 lastTimestamp = 0;

    for (const BatteryHistorySample &sample : m_samples) {
        const int value = getter(sample);
        if (value < 0 || !sample.connected || sample.stale || sample.reason == QLatin1String("missing")) {
            active = false;
            lastPixelX = -1;
            lastPixelY = -1;
            lastTimestamp = 0;
            continue;
        }
        if (lastTimestamp > 0 && sample.timestampMsecs - lastTimestamp > 15LL * 60 * 1000) {
            active = false;
            lastPixelX = -1;
            lastPixelY = -1;
        }
        const double ratio = double(sample.timestampMsecs - firstTime) /
                             double(qMax<qint64>(1, lastTime - firstTime));
        const int x = qRound(plot.left() + qBound(0.0, ratio, 1.0) * plot.width());
        const int y = yForValue(plot, value, discrete);

        // 同一像素位置的稳定心跳无需重复加入路径；变化值仍保留为竖向线段。
        // 这会把长历史压到控件分辨率内，同时不改变导出的原始数据。
        if (active && x == lastPixelX && y == lastPixelY) {
            lastTimestamp = sample.timestampMsecs;
            continue;
        }
        if (active && x == lastPixelX) {
            path.lineTo(x, y);
            lastPixelY = y;
            lastTimestamp = sample.timestampMsecs;
            continue;
        }
        if (!active) {
            path.moveTo(x, y);
            active = true;
        } else {
            path.lineTo(x, y);
        }
        lastPixelX = x;
        lastPixelY = y;
        lastTimestamp = sample.timestampMsecs;
    }

    painter.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
}

void BatteryHistoryChart::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette().color(QPalette::Base));

    if (!m_errorText.isEmpty()) {
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap,
                         m_errorText);
        return;
    }
    if (m_samples.isEmpty()) {
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(rect(), Qt::AlignCenter, tr("No history data"));
        return;
    }

    const QRectF plot = QRectF(rect()).adjusted(54, 18, -18, -38);
    if (plot.width() < 20 || plot.height() < 20) {
        return;
    }
    const qint64 firstTime = m_samples.first().timestampMsecs;
    qint64 lastTime = m_samples.last().timestampMsecs;
    if (lastTime <= firstTime) lastTime = firstTime + 60 * 1000;

    const QColor grid = palette().color(QPalette::Midlight);
    const QColor text = palette().color(QPalette::Text);
    painter.setPen(QPen(grid, 1));

    const bool airPods = std::any_of(m_samples.cbegin(), m_samples.cend(),
        [](const BatteryHistorySample &s) { return s.subType == BatteryDevice::SubType::AirPods; });
    const bool hasPercentage = std::any_of(m_samples.cbegin(), m_samples.cend(),
        [](const BatteryHistorySample &s) { return s.percentage >= 0; });
    const bool discrete = !airPods && !hasPercentage;

    const int gridSteps = discrete ? 3 : 4;
    for (int i = 0; i <= gridSteps; ++i) {
        const double ratio = double(i) / gridSteps;
        const int y = qRound(plot.bottom() - ratio * plot.height());
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(text);
        QString label;
        if (discrete) {
            if (i == 0) label = tr("Empty");
            else if (i == 1) label = tr("Low");
            else if (i == 2) label = tr("Medium");
            else label = tr("Full");
        } else {
            label = QString::number(i * 25) + QLatin1Char('%');
        }
        painter.drawText(QRectF(2, y - 10, 47, 20), Qt::AlignRight | Qt::AlignVCenter, label);
        painter.setPen(QPen(grid, 1));
    }

    painter.setPen(text);
    const QDateTime first = QDateTime::fromMSecsSinceEpoch(firstTime);
    const QDateTime last = QDateTime::fromMSecsSinceEpoch(lastTime);
    painter.drawText(QRectF(plot.left(), plot.bottom() + 8, plot.width() / 2, 20),
                     Qt::AlignLeft, first.toString(QStringLiteral("MM-dd HH:mm")));
    painter.drawText(QRectF(plot.center().x(), plot.bottom() + 8, plot.width() / 2, 20),
                     Qt::AlignRight, last.toString(QStringLiteral("MM-dd HH:mm")));

    if (airPods) {
        drawSeries(painter, plot, firstTime, lastTime, leftValue, QColor(0x00, 0x7a, 0xff));
        drawSeries(painter, plot, firstTime, lastTime, rightValue, QColor(0x34, 0xc7, 0x59));
        drawSeries(painter, plot, firstTime, lastTime, caseValue, QColor(0xff, 0x95, 0x00));
    } else if (hasPercentage) {
        drawSeries(painter, plot, firstTime, lastTime, percentageValue, QColor(0x00, 0x7a, 0xff));
    } else {
        drawSeries(painter, plot, firstTime, lastTime, levelValue, QColor(0x00, 0x7a, 0xff));
    }
}
