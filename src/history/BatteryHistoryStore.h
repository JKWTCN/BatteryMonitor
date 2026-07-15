#pragma once

#include "src/core/BatteryDevice.h"

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>

#include <functional>

struct BatteryHistorySample
{
    qint64 timestampMsecs = 0;
    QString deviceId;
    QString deviceName;
    BatteryDevice::Type type = BatteryDevice::Type::Bluetooth;
    BatteryDevice::SubType subType = BatteryDevice::SubType::Generic;
    int percentage = -1;
    BatteryLevel level = BatteryLevel::Unknown;
    int leftPercent = -1;
    int rightPercent = -1;
    int casePercent = -1;
    bool charging = false;
    bool wired = false;
    bool connected = false;
    bool stale = false;
    QString reason;
};

class QSqlDatabase;

class BatteryHistoryStore : public QObject
{
    Q_OBJECT

public:
    explicit BatteryHistoryStore(QObject *parent = nullptr,
                                 const QString &databasePath = QString(),
                                 std::function<qint64()> clock = {});
    ~BatteryHistoryStore() override;

    bool isAvailable() const { return m_available; }
    QString errorString() const { return m_errorString; }
    QString databasePath() const { return m_databasePath; }

    QList<BatteryHistorySample> samples(const QString &deviceId,
                                        qint64 fromMsecs = 0,
                                        qint64 toMsecs = 0) const;
    bool exportCsv(const QString &filePath,
                   const QString &deviceId,
                   qint64 fromMsecs,
                   qint64 toMsecs,
                   QString *error = nullptr) const;
    void setRetentionDays(int days);

public slots:
    void recordSnapshot(const QList<BatteryDevice> &devices);

signals:
    void historyChanged(const QString &deviceId);

private:
    QString signature(const BatteryDevice &device) const;
    bool insertSample(const BatteryHistorySample &sample);
    bool initialize();
    void cleanupExpired();
    void setError(const QString &message);

    QString m_connectionName;
    QString m_databasePath;
    bool m_available = false;
    QString m_errorString;
    QHash<QString, BatteryDevice> m_currentDevices;
    QHash<QString, QString> m_lastSignatures;
    QHash<QString, qint64> m_lastRecordedMsecs;
    qint64 m_lastCleanupMsecs = 0;
    std::function<qint64()> m_clock;
};

Q_DECLARE_METATYPE(BatteryHistorySample)
