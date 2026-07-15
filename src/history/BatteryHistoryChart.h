#pragma once

#include "BatteryHistoryStore.h"

#include <QColor>
#include <QRectF>
#include <QWidget>

class QPainter;

class BatteryHistoryChart : public QWidget
{
    Q_OBJECT

public:
    explicit BatteryHistoryChart(QWidget *parent = nullptr);

    void setSamples(const QList<BatteryHistorySample> &samples);
    void setErrorText(const QString &text);
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    using ValueGetter = int (*)(const BatteryHistorySample &);
    void drawSeries(QPainter &painter, const QRectF &plot,
                    qint64 firstTime, qint64 lastTime,
                    ValueGetter getter, const QColor &color) const;
    int yForValue(const QRectF &plot, int value, bool discrete) const;

    QList<BatteryHistorySample> m_samples;
    QString m_errorText;
};
