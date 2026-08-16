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

// 大小轮转检查频率：每写多少行 stat 一次文件大小。
constexpr unsigned int kRotateCheckLines = 512;
} // namespace

Logger &Logger::instance()
{
    static Logger instance;
    return instance;
}

Logger::Logger() = default;

Logger::~Logger()
{
    delete m_file; // QFile 析构时自动关闭。
}

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

bool Logger::openFile()
{
    if (!m_file) {
        m_file = new QFile(toQString(m_filePath));
    }
    if (!m_file->isOpen() &&
        !m_file->open(QIODevice::WriteOnly | QIODevice::Append)) {
        return false;
    }
    return true;
}

void Logger::rotateIfNeeded()
{
    // 每行日志都打开一次文件查大小太贵（Verbose 下高频路径会放大成可观的
    // 系统调用 / CPU 开销），按行数降频；计数器为 0（首次写入 / 刚轮转）
    // 时必查一次，保证已超限的旧日志第一行就触发截断。
    if (m_linesUntilRotateCheck > 0) {
        --m_linesUntilRotateCheck;
        return;
    }
    m_linesUntilRotateCheck = kRotateCheckLines - 1;

    QFileInfo info(toQString(m_filePath));
    if (info.size() <= static_cast<qint64>(m_maxBytes)) {
        return;
    }
    // 超过上限：关闭常开句柄后截断重写，下一次 writeLine 会重新打开。
    if (m_file && m_file->isOpen()) {
        m_file->close();
    }
    QFile truncate(toQString(m_filePath));
    if (truncate.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        truncate.close();
    }
}

void Logger::writeLine(const std::string &utf8Line)
{
    rotateIfNeeded();
    if (!openFile()) {
        return;
    }
    if (m_file->write(utf8Line.data(), static_cast<qint64>(utf8Line.size())) >= 0) {
        return;
    }
    // 写失败（如日志文件被外部删除）：重开一次再试，仍失败则放弃本行。
    m_file->close();
    if (openFile()) {
        m_file->write(utf8Line.data(), static_cast<qint64>(utf8Line.size()));
    }
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
