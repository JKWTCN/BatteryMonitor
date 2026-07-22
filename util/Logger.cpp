#include "Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QString>

namespace
{
QMutex g_logMutex;

const char *levelTag(Logger::Level level)
{
    switch (level) {
    case Logger::Level::Verbose: return "VRB  ";
    case Logger::Level::Info:    return "INFO ";
    case Logger::Level::Warning: return "WARN ";
    case Logger::Level::Error:   return "ERROR";
    default:                     return "INFO ";
    }
}

QString toQString(const std::wstring &value)
{
    return QString::fromStdWString(value);
}
} // namespace

Logger &Logger::instance()
{
    static Logger instance;
    return instance;
}

Logger::Logger() = default;

void Logger::setLevel(Level level)
{
    const QMutexLocker locker(&g_logMutex);
    m_minLevel = level;
}

void Logger::setDirName(const std::wstring &dirName)
{
    const QMutexLocker locker(&g_logMutex);
    if (!m_initialized && !dirName.empty()) {
        m_dirName = dirName;
    }
}

void Logger::setFileName(const std::wstring &fileName)
{
    const QMutexLocker locker(&g_logMutex);
    if (!m_initialized && !fileName.empty()) {
        m_fileName = fileName;
    }
}

void Logger::ensureInitialized()
{
    if (m_initialized) {
        return;
    }
    m_filePath = resolveFilePath();
    const QFileInfo fileInfo(toQString(m_filePath));
    QDir().mkpath(fileInfo.absolutePath());
    m_initialized = true;
}

std::wstring Logger::resolveFilePath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty()) {
        base = QDir::currentPath();
    }
    const QString path = QDir(base).filePath(
        toQString(m_dirName) + QLatin1Char('/') + toQString(m_fileName));
    return QDir::cleanPath(path).toStdWString();
}

void Logger::rotateIfNeeded()
{
    QFile file(toQString(m_filePath));
    if (file.size() > static_cast<qint64>(m_maxBytes) &&
        file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.close();
    }
}

void Logger::writeLine(const std::string &utf8Line)
{
    rotateIfNeeded();
    QFile file(toQString(m_filePath));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return;
    }
    file.write(utf8Line.data(), static_cast<qint64>(utf8Line.size()));
}

std::string Logger::formatLine(const std::wstring &message, Level level)
{
    const QString timestamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QByteArray line = QStringLiteral("[%1] [%2] %3\n")
        .arg(timestamp,
             QString::fromLatin1(levelTag(level)),
             QString::fromStdWString(message))
        .toUtf8();
    return std::string(line.constData(), static_cast<size_t>(line.size()));
}

void Logger::log(const std::wstring &message, Level level)
{
    const QMutexLocker locker(&g_logMutex);
    if (level < m_minLevel) {
        return;
    }
    ensureInitialized();
    writeLine(formatLine(message, level));
}

void Logger::log(const std::string &message, Level level)
{
    log(QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()))
            .toStdWString(),
        level);
}

void Logger::log(const char *message, Level level)
{
    log(std::string(message ? message : ""), level);
}
