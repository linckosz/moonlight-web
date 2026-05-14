#include "ZrokClient.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ZrokClient::ZrokClient(QObject* parent)
    : QObject(parent)
{
}

ZrokClient::~ZrokClient()
{
    stop();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ZrokClient::start()
{
    if (m_Process) {
        qWarning() << "[ZrokClient] Already running, call stop() first";
        return;
    }

    if (m_ReservedName.isEmpty()) {
        emit tunnelError("zrok reserved name not configured");
        return;
    }

    // Locate the zrok binary if not already resolved
    if (m_ZrokBinaryPath.isEmpty()) {
        m_ZrokBinaryPath = findZrokBinary();
    }

    if (m_ZrokBinaryPath.isEmpty()) {
        QString err = "zrok binary not found — searched <exe_dir>/tools/, <cwd>/backend/tools/, and PATH";
        qWarning() << "[ZrokClient]" << err;
        emit tunnelError(err);
        return;
    }

    qInfo() << "[ZrokClient] Starting tunnel, reservedName=" << m_ReservedName
            << "targetPort=" << m_TargetPort
            << "binary=" << m_ZrokBinaryPath;

    m_Process = new QProcess(this);
    m_Process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_Process, &QProcess::started,
            this, &ZrokClient::onProcessStarted);
    connect(m_Process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ZrokClient::onProcessFinished);
    connect(m_Process, &QProcess::readyReadStandardOutput,
            this, &ZrokClient::onReadyReadStdout);
    connect(m_Process, &QProcess::readyReadStandardError,
            this, &ZrokClient::onReadyReadStderr);

    // Command: <binary> share reserved <name> --headless
    QStringList args;
    args << "share" << "reserved" << m_ReservedName << "--headless";

    qInfo() << "[ZrokClient] Launching:" << m_ZrokBinaryPath << args.join(' ');
    m_Process->start(m_ZrokBinaryPath, args);

    if (!m_Process->waitForStarted(10000)) {
        QString err = m_Process->errorString();
        qWarning() << "[ZrokClient] Failed to start zrok:" << err;
        emit tunnelError("Failed to start zrok: " + err);

        m_Process->deleteLater();
        m_Process = nullptr;
    }
}

void ZrokClient::stop()
{
    if (!m_Process) return;

    qInfo() << "[ZrokClient] Stopping tunnel...";

    if (m_Process->state() != QProcess::NotRunning) {
        m_Process->terminate();
        if (!m_Process->waitForFinished(5000)) {
            m_Process->kill();
            m_Process->waitForFinished(3000);
        }
    }

    m_Active = false;
    m_PublicUrl.clear();

    m_Process->deleteLater();
    m_Process = nullptr;

    qInfo() << "[ZrokClient] Tunnel stopped";
}

// ---------------------------------------------------------------------------
// Process slots
// ---------------------------------------------------------------------------

void ZrokClient::onProcessStarted()
{
    qInfo() << "[ZrokClient] Process started (PID" << m_Process->processId() << ")";
}

void ZrokClient::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    qInfo() << "[ZrokClient] Process finished: exitCode=" << exitCode
            << "status=" << (status == QProcess::NormalExit ? "normal" : "crash");

    m_Active = false;

    if (status == QProcess::CrashExit) {
        emit tunnelError("zrok process crashed");
    } else if (exitCode != 0) {
        emit tunnelError("zrok exited with code " + QString::number(exitCode));
    }

    emit tunnelStopped();

    m_Process->deleteLater();
    m_Process = nullptr;
}

void ZrokClient::onReadyReadStdout()
{
    QByteArray data = m_Process->readAllStandardOutput();
    parseShareOutput(data);
}

void ZrokClient::onReadyReadStderr()
{
    QByteArray data = m_Process->readAllStandardError();
    QString text = QString::fromUtf8(data).trimmed();
    if (!text.isEmpty()) {
        qInfo() << "[ZrokClient]" << text;
    }
}

// ---------------------------------------------------------------------------
// Output parsing
// ---------------------------------------------------------------------------

void ZrokClient::parseShareOutput(const QByteArray& data)
{
    QString text = QString::fromUtf8(data);
    qInfo() << "[ZrokClient] stdout:" << text.trimmed();

    // zrok outputs lines like:
    //   "Your share is ready: https://moonlightweb-a1b2.share.zrok.io"
    // We extract the URL from such lines.
    static QRegularExpression urlRe(
        R"(https://([a-zA-Z0-9-]+\.share\.zrok\.io))");

    QRegularExpressionMatch match = urlRe.match(text);
    if (match.hasMatch()) {
        m_PublicUrl = match.captured(1);  // e.g. "moonlightweb-a1b2.share.zrok.io"
        m_Active = true;

        QString fullUrl = "https://" + m_PublicUrl;
        qInfo() << "[ZrokClient] Tunnel ready at" << fullUrl;

        emit tunnelReady(m_PublicUrl);
    }
}

// ---------------------------------------------------------------------------
// Binary location
// ---------------------------------------------------------------------------

/// Returns the platform-specific binary filename ("zrok.exe" on Windows, "zrok" otherwise).
static QString platformBinaryName()
{
#ifdef Q_OS_WIN
    return "zrok.exe";
#else
    return "zrok";
#endif
}

/// Returns the platform-specific subdirectory name ("windows", "linux", "macos").
static QString platformSubdir()
{
#ifdef Q_OS_WIN
    return "windows";
#elif defined(Q_OS_LINUX)
    return "linux";
#elif defined(Q_OS_MACOS)
    return "macos";
#else
    return "windows"; // fallback
#endif
}

/// Searches for the zrok binary in a given base path, trying platform-specific subdirectories
/// first, then falling back to the flat directory (legacy).
static QString searchInDir(const QString& baseDir)
{
    // 1. Platform-specific subdirectory: <baseDir>/<platform>/zrok[.exe]
    {
        QString path = baseDir + "/" + platformSubdir() + "/" + platformBinaryName();
        if (QFileInfo::exists(path)) {
            qInfo() << "[ZrokClient] Found zrok at" << path;
            return QDir::toNativeSeparators(path);
        }
    }

    // 2. Legacy flat layout: <baseDir>/zrok[.exe]
    {
        QString path = baseDir + "/" + platformBinaryName();
        if (QFileInfo::exists(path)) {
            qInfo() << "[ZrokClient] Found zrok at" << path;
            return QDir::toNativeSeparators(path);
        }
    }

    return {};
}

QString ZrokClient::findZrokBinary() const
{
    // 1. <exe_dir>/tools/  (bundled alongside the executable)
    {
        QString path = searchInDir(
            QCoreApplication::applicationDirPath() + "/tools");
        if (!path.isEmpty()) return path;
    }

    // 2. <cwd>/backend/tools/  (source-tree layout, dev mode)
    {
        QString path = searchInDir(
            QDir::currentPath() + "/backend/tools");
        if (!path.isEmpty()) return path;
    }

    // 3. <cwd>/tools/
    {
        QString path = searchInDir(
            QDir::currentPath() + "/tools");
        if (!path.isEmpty()) return path;
    }

    // 4. Check PATH via `where zrok` (Windows) or `which zrok` (Unix)
    {
        QProcess which;
#ifdef Q_OS_WIN
        which.start("where", QStringList() << "zrok");
#else
        which.start("which", QStringList() << "zrok");
#endif
        if (which.waitForFinished(5000) && which.exitCode() == 0) {
            QString path = QString::fromUtf8(which.readAllStandardOutput())
                               .trimmed()
                               .split(QRegularExpression("[\r\n]+"))
                               .first()
                               .trimmed();
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                qInfo() << "[ZrokClient] Found zrok in PATH at" << path;
                return QDir::toNativeSeparators(path);
            }
        }
    }

    qWarning() << "[ZrokClient] zrok binary not found — LAN-only mode";
    return {};
}
