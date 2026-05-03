#pragma once

#include <QObject>
#include <QFile>
#include <QMutex>
#include <QTextStream>

class Logger : public QObject
{
    Q_OBJECT

public:
    enum Level { Debug, Info, Warning, Error };

    static Logger* instance();

    void setLogFile(const QString& path);
    void log(Level level, const QString& message);

    static void debug(const QString& msg)   { instance()->log(Debug, msg); }
    static void info(const QString& msg)    { instance()->log(Info, msg); }
    static void warning(const QString& msg) { instance()->log(Warning, msg); }
    static void error(const QString& msg)   { instance()->log(Error, msg); }

private:
    explicit Logger(QObject* parent = nullptr);
    QString levelString(Level level);

    QFile m_File;
    QTextStream m_Stream;
    QMutex m_Mutex;
    bool m_FileOpen;
};
