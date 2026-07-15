#include "BatteryHistoryStore.h"

#include "util/AppSettings.h"
#include "util/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTextStream>
#include <QUuid>

#include <utility>

namespace
{
constexpr qint64 kHeartbeatMsecs = 5 * 60 * 1000;

QString csvField(QString value)
{
    if (value.contains(QLatin1Char('"'))) {
        value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    }
    if (value.contains(QLatin1Char(',')) || value.contains(QLatin1Char('"')) ||
        value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r'))) {
        return QLatin1Char('"') + value + QLatin1Char('"');
    }
    return value;
}

QString nullableInt(int value)
{
    return value >= 0 ? QString::number(value) : QString();
}

QString typeKey(BatteryDevice::Type type)
{
    switch (type) {
    case BatteryDevice::Type::Bluetooth: return QStringLiteral("bluetooth");
    case BatteryDevice::Type::Xbox: return QStringLiteral("xbox");
    case BatteryDevice::Type::Hid: return QStringLiteral("hid");
    }
    return QStringLiteral("unknown");
}

QString subTypeKey(BatteryDevice::SubType type)
{
    return type == BatteryDevice::SubType::AirPods
        ? QStringLiteral("airpods") : QStringLiteral("generic");
}

QString levelKey(BatteryLevel level)
{
    switch (level) {
    case BatteryLevel::Empty: return QStringLiteral("empty");
    case BatteryLevel::Low: return QStringLiteral("low");
    case BatteryLevel::Medium: return QStringLiteral("medium");
    case BatteryLevel::Full: return QStringLiteral("full");
    case BatteryLevel::Unknown: break;
    }
    return QStringLiteral("unknown");
}

BatteryHistorySample fromQuery(const QSqlQuery &query)
{
    BatteryHistorySample sample;
    sample.timestampMsecs = query.value(0).toLongLong();
    sample.deviceId = query.value(1).toString();
    sample.deviceName = query.value(2).toString();
    sample.type = static_cast<BatteryDevice::Type>(query.value(3).toInt());
    sample.subType = static_cast<BatteryDevice::SubType>(query.value(4).toInt());
    sample.percentage = query.value(5).isNull() ? -1 : query.value(5).toInt();
    sample.level = static_cast<BatteryLevel>(query.value(6).toInt());
    sample.leftPercent = query.value(7).isNull() ? -1 : query.value(7).toInt();
    sample.rightPercent = query.value(8).isNull() ? -1 : query.value(8).toInt();
    sample.casePercent = query.value(9).isNull() ? -1 : query.value(9).toInt();
    sample.charging = query.value(10).toBool();
    sample.wired = query.value(11).toBool();
    sample.connected = query.value(12).toBool();
    sample.stale = query.value(13).toBool();
    sample.reason = query.value(14).toString();
    return sample;
}
} // namespace

BatteryHistoryStore::BatteryHistoryStore(QObject *parent, const QString &databasePath,
                                         std::function<qint64()> clock)
    : QObject(parent)
    , m_connectionName(QStringLiteral("BatteryHistory-%1")
                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
    , m_clock(clock ? std::move(clock) : [] { return QDateTime::currentMSecsSinceEpoch(); })
{
    if (databasePath.isEmpty()) {
        const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        m_databasePath = QDir(dataDir).filePath(QStringLiteral("battery-history.sqlite"));
    } else {
        m_databasePath = databasePath;
    }
    m_available = initialize();
    if (m_available) {
        cleanupExpired();
    }
}

BatteryHistoryStore::~BatteryHistoryStore()
{
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
            db.close();
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

void BatteryHistoryStore::setError(const QString &message)
{
    m_available = false;
    m_errorString = message;
    LOG_WARN_W(L"[History] " + message.toStdWString());
}

bool BatteryHistoryStore::initialize()
{
    const QFileInfo info(m_databasePath);
    if (!QDir().mkpath(info.absolutePath())) {
        setError(tr("Failed to create the history data directory."));
        return false;
    }

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(m_databasePath);
    if (!db.open()) {
        setError(db.lastError().text());
        return false;
    }

    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS battery_samples ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "timestamp_ms INTEGER NOT NULL, device_id TEXT NOT NULL, device_name TEXT NOT NULL,"
            "device_type INTEGER NOT NULL, subtype INTEGER NOT NULL, percentage INTEGER NULL,"
            "level INTEGER NOT NULL, left_percent INTEGER NULL, right_percent INTEGER NULL,"
            "case_percent INTEGER NULL, charging INTEGER NOT NULL, wired INTEGER NOT NULL,"
            "connected INTEGER NOT NULL, stale INTEGER NOT NULL, reason TEXT NOT NULL)"))) {
        setError(query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_battery_samples_device_time "
            "ON battery_samples(device_id, timestamp_ms)"))) {
        setError(query.lastError().text());
        return false;
    }
    query.exec(QStringLiteral("PRAGMA user_version = 1"));
    return true;
}

QString BatteryHistoryStore::signature(const BatteryDevice &d) const
{
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10|%11")
        .arg(d.percentage).arg(static_cast<int>(d.level))
        .arg(d.leftPercent).arg(d.rightPercent).arg(d.casePercent)
        .arg(d.charging).arg(d.wired).arg(d.connected).arg(d.stale)
        .arg(static_cast<int>(d.type)).arg(static_cast<int>(d.subType));
}

bool BatteryHistoryStore::insertSample(const BatteryHistorySample &s)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT INTO battery_samples(timestamp_ms,device_id,device_name,device_type,subtype,"
        "percentage,level,left_percent,right_percent,case_percent,charging,wired,connected,stale,reason) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(s.timestampMsecs);
    query.addBindValue(s.deviceId);
    query.addBindValue(s.deviceName);
    query.addBindValue(static_cast<int>(s.type));
    query.addBindValue(static_cast<int>(s.subType));
    query.addBindValue(s.percentage >= 0 ? QVariant(s.percentage) : QVariant());
    query.addBindValue(static_cast<int>(s.level));
    query.addBindValue(s.leftPercent >= 0 ? QVariant(s.leftPercent) : QVariant());
    query.addBindValue(s.rightPercent >= 0 ? QVariant(s.rightPercent) : QVariant());
    query.addBindValue(s.casePercent >= 0 ? QVariant(s.casePercent) : QVariant());
    query.addBindValue(s.charging);
    query.addBindValue(s.wired);
    query.addBindValue(s.connected);
    query.addBindValue(s.stale);
    query.addBindValue(s.reason);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}

void BatteryHistoryStore::recordSnapshot(const QList<BatteryDevice> &devices)
{
    if (!m_available) {
        return;
    }
    const qint64 now = m_clock();
    if (now - m_lastCleanupMsecs >= 24LL * 60 * 60 * 1000) {
        cleanupExpired();
    }
    QSet<QString> seen;
    QSet<QString> changedIds;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    db.transaction();

    for (const BatteryDevice &device : devices) {
        const QString id = QString::fromStdWString(device.id);
        if (id.isEmpty()) {
            continue;
        }
        seen.insert(id);
        const QString sig = signature(device);
        QString reason;
        if (!m_lastSignatures.contains(id) || m_lastSignatures.value(id) != sig) {
            reason = QStringLiteral("change");
        } else if (now - m_lastRecordedMsecs.value(id, 0) >= kHeartbeatMsecs) {
            reason = QStringLiteral("heartbeat");
        }

        m_currentDevices.insert(id, device);
        m_lastSignatures.insert(id, sig);
        if (reason.isEmpty()) {
            continue;
        }
        BatteryHistorySample sample;
        sample.timestampMsecs = now;
        sample.deviceId = id;
        sample.deviceName = QString::fromStdWString(device.name);
        sample.type = device.type;
        sample.subType = device.subType;
        sample.percentage = device.percentage;
        sample.level = device.level;
        sample.leftPercent = device.leftPercent;
        sample.rightPercent = device.rightPercent;
        sample.casePercent = device.casePercent;
        sample.charging = device.charging;
        sample.wired = device.wired;
        sample.connected = device.connected;
        sample.stale = device.stale;
        sample.reason = reason;
        if (insertSample(sample)) {
            m_lastRecordedMsecs.insert(id, now);
            changedIds.insert(id);
        }
    }

    const auto previousIds = m_currentDevices.keys();
    for (const QString &id : previousIds) {
        if (seen.contains(id)) {
            continue;
        }
        const BatteryDevice previous = m_currentDevices.take(id);
        m_lastSignatures.remove(id);
        BatteryHistorySample sample;
        sample.timestampMsecs = now;
        sample.deviceId = id;
        sample.deviceName = QString::fromStdWString(previous.name);
        sample.type = previous.type;
        sample.subType = previous.subType;
        sample.level = BatteryLevel::Unknown;
        sample.reason = QStringLiteral("missing");
        if (insertSample(sample)) {
            m_lastRecordedMsecs.insert(id, now);
            changedIds.insert(id);
        }
    }

    if (!db.commit()) {
        setError(db.lastError().text());
        return;
    }
    for (const QString &id : std::as_const(changedIds)) {
        emit historyChanged(id);
    }
}

QList<BatteryHistorySample> BatteryHistoryStore::samples(const QString &deviceId,
                                                         qint64 fromMsecs,
                                                         qint64 toMsecs) const
{
    QList<BatteryHistorySample> result;
    if (!m_available || deviceId.isEmpty()) {
        return result;
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
    QString sql = QStringLiteral(
        "SELECT timestamp_ms,device_id,device_name,device_type,subtype,percentage,level,"
        "left_percent,right_percent,case_percent,charging,wired,connected,stale,reason "
        "FROM battery_samples WHERE device_id=?");
    if (fromMsecs > 0) sql += QStringLiteral(" AND timestamp_ms>=?");
    if (toMsecs > 0) sql += QStringLiteral(" AND timestamp_ms<=?");
    sql += QStringLiteral(" ORDER BY timestamp_ms ASC,id ASC");
    query.prepare(sql);
    query.addBindValue(deviceId);
    if (fromMsecs > 0) query.addBindValue(fromMsecs);
    if (toMsecs > 0) query.addBindValue(toMsecs);
    if (!query.exec()) {
        return result;
    }
    while (query.next()) {
        result.append(fromQuery(query));
    }
    return result;
}

bool BatteryHistoryStore::exportCsv(const QString &filePath,
                                    const QString &deviceId,
                                    qint64 fromMsecs,
                                    qint64 toMsecs,
                                    QString *error) const
{
    const QList<BatteryHistorySample> rows = samples(deviceId, fromMsecs, toMsecs);
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = file.errorString();
        return false;
    }
    file.write("\xEF\xBB\xBF", 3);
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "timestamp_utc,timestamp_local,timestamp_ms,device_id,device_name,device_type,subtype,"
              "percentage,level,left_percent,right_percent,case_percent,charging,wired,connected,stale,sample_reason\r\n";
    for (const BatteryHistorySample &s : rows) {
        const QDateTime dt = QDateTime::fromMSecsSinceEpoch(s.timestampMsecs);
        const QStringList fields = {
            dt.toUTC().toString(Qt::ISODateWithMs), dt.toString(Qt::ISODateWithMs),
            QString::number(s.timestampMsecs), s.deviceId, s.deviceName,
            typeKey(s.type), subTypeKey(s.subType),
            nullableInt(s.percentage), levelKey(s.level),
            nullableInt(s.leftPercent), nullableInt(s.rightPercent), nullableInt(s.casePercent),
            s.charging ? QStringLiteral("1") : QStringLiteral("0"),
            s.wired ? QStringLiteral("1") : QStringLiteral("0"),
            s.connected ? QStringLiteral("1") : QStringLiteral("0"),
            s.stale ? QStringLiteral("1") : QStringLiteral("0"), s.reason
        };
        QStringList escaped;
        escaped.reserve(fields.size());
        for (const QString &field : fields) escaped.append(csvField(field));
        stream << escaped.join(QLatin1Char(',')) << "\r\n";
    }
    stream.flush();
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

void BatteryHistoryStore::cleanupExpired()
{
    m_lastCleanupMsecs = m_clock();
    const int days = AppSettings::historyRetentionDays();
    if (!m_available || days <= 0) {
        return;
    }
    const qint64 cutoff = m_clock() - qint64(days) * 24 * 60 * 60 * 1000;
    QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
    query.prepare(QStringLiteral("DELETE FROM battery_samples WHERE timestamp_ms < ?"));
    query.addBindValue(cutoff);
    if (!query.exec()) {
        setError(query.lastError().text());
    }
}

void BatteryHistoryStore::setRetentionDays(int days)
{
    AppSettings::setHistoryRetentionDays(days);
    cleanupExpired();
}
