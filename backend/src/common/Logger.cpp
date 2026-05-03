#include "Logger.h"
#include <QDateTime>
#include <QDebug>
#include <iostream>

Logger* Logger::instance()
{
    static Logger s_Instance;
    return &s_Instance;
}

Logger::Logger(QObject* parent)
    : QObject(parent)
    , m_FileOpen(false)
{
}

void Logger::setLogFile(const QString& path)
{
    QMutexLocker lock(&m_Mutex);
    if (m_File.isOpen())
        m_File.close();

    m_File.setFileName(path);
    m_FileOpen = m_File.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    if (m_FileOpen)
        m_Stream.setDevice(&m_File);
}

void Logger::log(Level level, const QString& message)
{
    QString line = QString("[%1] [%2] %3\n")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz"))
        .arg(levelString(level))
        .arg(message);

    QMutexLocker lock(&m_Mutex);

    // Always print to console
    if (level >= Warning)
        std::cerr << line.toStdString();
    else
        std::cout << line.toStdString();

    // Write to file if configured
    if (m_FileOpen) {
        m_Stream << line;
        m_Stream.flush();
    }
}

QString Logger::levelString(Level level)
{
    switch (level) {
    case Debug:   return "DEBUG";
    case Info:    return "INFO";
    case Warning: return "WARN";
    case Error:   return "ERROR";
    }
    return "???";
}
