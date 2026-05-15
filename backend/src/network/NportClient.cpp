#include "NportClient.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Refresh interval: 3h30 (kill + restart nport to keep the tunnel alive).
static constexpr int kRefreshIntervalMs = 3 * 3600 * 1000 + 30 * 60 * 1000;

/// Subdomain prefix used for the tunnel hostname.
static const QString kSubdomainPrefix = QStringLiteral("moonlightweb-");

/// Base URL for the nport API (POST to create/reset, DELETE to release).
static const QString kApiBaseUrl = QStringLiteral("https://api.nport.link");

/// Timeout for nport API calls (POST create, etc.).
static constexpr int kApiTimeoutMs = 10000;

/// Timeout for waiting on nport process to signal ready.
static constexpr int kNportStartTimeoutMs = 15000;

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

NportClient::NportClient(QObject* parent)
    : QObject(parent)
{
    m_RefreshTimer = new QTimer(this);
    m_RefreshTimer->setSingleShot(true);
    connect(m_RefreshTimer, &QTimer::timeout, this, &NportClient::onRefreshTimeout);

    m_NetworkManager = new QNetworkAccessManager(this);

    // Probe for bundled nport binary once at construction
    findNportBinary();
}

NportClient::~NportClient()
{
    stop();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool NportClient::isAvailable() const
{
    return !m_NportPath.isEmpty() && !m_Subdomain.isEmpty();
}

void NportClient::start()
{
    if (m_Process) {
        qWarning() << "[NportClient] Already running, call stop() first";
        return;
    }

    if (m_Subdomain.isEmpty()) {
        emit tunnelError("nport subdomain not configured");
        return;
    }

    if (m_NportPath.isEmpty()) {
        emit tunnelError("nport binary not found — run LAN-only mode");
        return;
    }

    doStart();
}

void NportClient::resumeRefresh()
{
    m_RefreshPaused = false;
    if (m_PendingRefresh) {
        qInfo() << "[NportClient] Executing deferred refresh after signaling";
        onRefreshTimeout();
    }
}

void NportClient::stop()
{
    m_RefreshTimer->stop();
    if (m_StartTimeoutTimer) {
        m_StartTimeoutTimer->stop();
    }
    m_Retried = false;

    if (!m_Process) {
        m_Active = false;
        return;
    }

    if (m_Process->state() == QProcess::NotRunning) {
        m_Process->deleteLater();
        m_Process = nullptr;
        m_Active = false;
        m_PublicUrl.clear();
        m_LastOutput.clear();
        return;
    }

    qInfo() << "[NportClient] Stopping nport gracefully...";

    // Step 1: Try graceful shutdown via Ctrl+C (triggers nport cleanup() -> DELETE API)
    if (sendCtrlC()) {
        qInfo() << "[NportClient] nport exited gracefully";
    } else {
        qWarning() << "[NportClient] Graceful shutdown failed — force killing nport";
        forceKill();
    }

    m_Active = false;
    m_PublicUrl.clear();
    m_TunnelId.clear();
    m_LastOutput.clear();

    // The finished signal may have already fired (during sendCtrlC/forceKill)
    // and set m_Process to nullptr. Only delete if still valid.
    if (m_Process) {
        m_Process->deleteLater();
        m_Process = nullptr;
    }

    qInfo() << "[NportClient] Tunnel stopped";
}

// ---------------------------------------------------------------------------
// Internal start
// ---------------------------------------------------------------------------

void NportClient::doStart()
{
    m_LastOutput.clear();
    m_PublicUrl.clear();
    m_TunnelId.clear();
    m_Retried = false;

    // Pre-create the tunnel via API to obtain tunnelId and public URL.
    // The API returns the full public URL directly — no need to construct it locally.
    {
        QString apiTunnelId, apiUrl;
        if (createTunnelViaApi(apiTunnelId, apiUrl)) {
            m_TunnelId = apiTunnelId;
            m_PublicUrl = apiUrl;
            qInfo() << "[NportClient] Tunnel pre-created via API:"
                    << "id=" << m_TunnelId << "url=" << m_PublicUrl;
        } else {
            qWarning() << "[NportClient] Failed to pre-create tunnel via API —"
                       << "will rely on nport's own creation";
        }
    }

    launchNport();
}

// ---------------------------------------------------------------------------
// nport binary detection
// ---------------------------------------------------------------------------

bool NportClient::findNportBinary()
{
    // Look for the nport binary bundled with the nport npm package.
    //
    // npm install creates a script wrapper in node_modules/.bin/:
    //   Windows:  runtime/nport/node_modules/.bin/nport.cmd
    //   Unix:     runtime/nport/node_modules/.bin/nport
    //
    // Some packages also ship a native binary in the package bin/ dir:
    //   runtime/nport/node_modules/nport/bin/nport.exe  (Windows)
    //
    // The .cmd wrapper is the most reliable target on Windows — QProcess
    // can execute .cmd files natively via cmd.exe.
    //
    // Search order:
    //   1. Search known directory patterns relative to applicationDirPath()
    //   2. Walk up parent directories looking for runtime/nport/...
    //   3. Fallback to PATH lookup

    QString appDir = QCoreApplication::applicationDirPath();

    // Binary names to try, in priority order.
    // On Windows, .cmd (npm wrapper) is preferred over .exe because most
    // npm packages are Node.js CLI tools, not native binaries.
    QStringList binaryNames;
#ifdef Q_OS_WIN
    binaryNames << QStringLiteral("nport.cmd")
                << QStringLiteral("nport.exe")
                << QStringLiteral("nport");
#else
    binaryNames << QStringLiteral("nport");
#endif

    // Search paths, ordered by likelihood.
    QStringList searchDirs;
    // nport package bin/ directory
    searchDirs << (appDir + "/../../../runtime/nport/node_modules/nport/bin");
    searchDirs << (appDir + "/../../../../runtime/nport/node_modules/nport/bin");
    searchDirs << (appDir + "/../../runtime/nport/node_modules/nport/bin");
    searchDirs << (appDir + "/../runtime/nport/node_modules/nport/bin");
    searchDirs << (appDir + "/runtime/nport/node_modules/nport/bin");
    searchDirs << (appDir + "/../../../../../runtime/nport/node_modules/nport/bin");
    // npm .bin/ directory (wrapper scripts — most reliable on npm)
    searchDirs << (appDir + "/../../../runtime/nport/node_modules/.bin");
    searchDirs << (appDir + "/../../../../runtime/nport/node_modules/.bin");
    searchDirs << (appDir + "/../../runtime/nport/node_modules/.bin");
    searchDirs << (appDir + "/../runtime/nport/node_modules/.bin");
    searchDirs << (appDir + "/runtime/nport/node_modules/.bin");
    searchDirs << (appDir + "/../../../../../runtime/nport/node_modules/.bin");

    for (const QString& dirPath : searchDirs) {
        for (const QString& name : binaryNames) {
            QFileInfo fi(dirPath + "/" + name);
            if (fi.exists() && fi.isFile()) {
                m_NportPath = QDir::toNativeSeparators(fi.absoluteFilePath());
                qInfo() << "[NportClient] Found nport:" << m_NportPath;
                return true;
            }
        }
    }

    // Smart fallback: walk up parent directories looking for runtime/nport/...
    {
        QDir dir(appDir);
        for (int i = 0; i < 8; ++i) {
            if (!dir.cdUp()) break;

            for (const QString& name : binaryNames) {
                // Check nport/bin/ path
                QString candidate = dir.absoluteFilePath(
                    "runtime/nport/node_modules/nport/bin/" + name);
                QFileInfo fi(candidate);
                if (fi.exists() && fi.isFile()) {
                    m_NportPath = QDir::toNativeSeparators(fi.absoluteFilePath());
                    qInfo() << "[NportClient] Found nport (parent walk):" << m_NportPath;
                    return true;
                }

                // Check .bin/ path
                candidate = dir.absoluteFilePath(
                    "runtime/nport/node_modules/.bin/" + name);
                QFileInfo fi2(candidate);
                if (fi2.exists() && fi2.isFile()) {
                    m_NportPath = QDir::toNativeSeparators(fi2.absoluteFilePath());
                    qInfo() << "[NportClient] Found nport (parent walk, .bin):" << m_NportPath;
                    return true;
                }
            }
        }
    }

    // Fallback: check PATH for nport (without extension to find .cmd too)
    QProcess which;
#ifdef Q_OS_WIN
    which.start("where", QStringList() << "nport");
#else
    which.start("which", QStringList() << "nport");
#endif

    if (which.waitForFinished(10000) && which.exitCode() == 0) {
        m_NportPath = QString::fromUtf8(which.readAllStandardOutput())
                          .trimmed().split('\n').first().trimmed();
        if (!m_NportPath.isEmpty()) {
            qInfo() << "[NportClient] Found nport (via PATH):" << m_NportPath;
            return true;
        }
    }

    m_NportPath.clear();
    qWarning() << "[NportClient] nport binary not found — tunnel unavailable";
    return false;
}

// ---------------------------------------------------------------------------
// API: create/reset tunnel (POST)
// ---------------------------------------------------------------------------

bool NportClient::createTunnelViaApi(QString& outTunnelId, QString& outUrl)
{
    QUrl url(kApiBaseUrl);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["subdomain"] = buildSubdomain();
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    qInfo() << "[NportClient] Creating/resetting tunnel via API for subdomain:" << buildSubdomain();

    QNetworkReply* reply = m_NetworkManager->post(request, payload);

    // Synchronous wait
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(kApiTimeoutMs);
    loop.exec();

    if (!timeout.isActive()) {
        qWarning() << "[NportClient] API create tunnel timed out";
        reply->abort();
        reply->deleteLater();
        return false;
    }
    timeout.stop();

    int statusCode = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray responseData = reply->readAll();
    reply->deleteLater();

    if (statusCode != 200) {
        qWarning() << "[NportClient] API create tunnel returned HTTP" << statusCode
                   << ":" << QString::fromUtf8(responseData);
        return false;
    }

    QJsonDocument doc = QJsonDocument::fromJson(responseData);
    if (!doc.isObject()) {
        qWarning() << "[NportClient] API create tunnel returned invalid JSON";
        return false;
    }

    QJsonObject resp = doc.object();
    if (!resp.value("success").toBool()) {
        // Non-failure error codes: SUBDOMAIN_IN_USE, etc.
        QString errMsg = resp.value("error").toString("Unknown error");
        qWarning() << "[NportClient] API create tunnel error:" << errMsg;
        return false;
    }

    outTunnelId = resp.value("tunnelId").toString();
    outUrl = resp.value("url").toString();

    if (outTunnelId.isEmpty()) {
        qWarning() << "[NportClient] API response missing tunnelId";
        return false;
    }

    qInfo() << "[NportClient] Tunnel created successfully via API:"
            << "id=" << outTunnelId << "url=" << outUrl;
    return true;
}

// ---------------------------------------------------------------------------
// nport launch
// ---------------------------------------------------------------------------

void NportClient::launchNport()
{
    m_Process = new QProcess(this);
    m_Process->setProcessChannelMode(QProcess::SeparateChannels);

    // Connect stdout/stderr for output parsing
    connect(m_Process, &QProcess::readyReadStandardOutput,
            this, &NportClient::onNportStdout);
    connect(m_Process, &QProcess::readyReadStandardError,
            this, &NportClient::onNportStderr);
    connect(m_Process, &QProcess::errorOccurred,
            this, &NportClient::onNportError);

    // Connect finished to detect exit/crash and trigger retry logic
    connect(m_Process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
        // Read any remaining output
        if (m_Process) {
            m_LastOutput += QString::fromUtf8(m_Process->readAllStandardOutput());
            m_LastOutput += QString::fromUtf8(m_Process->readAllStandardError());
        }

        qInfo() << "[NportClient] nport finished: exitCode=" << exitCode
                << "status=" << (status == QProcess::NormalExit ? "normal" : "crash");

        bool wasActive = m_Active;
        m_Active = false;

        // If we haven't detected ready and process died, check for retry patterns
        if (!wasActive && !m_Retried) {
            if (m_LastOutput.contains("already in use", Qt::CaseInsensitive)
                || m_LastOutput.contains("conflict", Qt::CaseInsensitive)
                || m_LastOutput.contains("exists", Qt::CaseInsensitive)
                || m_LastOutput.contains("taken", Qt::CaseInsensitive)) {
                qInfo() << "[NportClient] Subdomain already in use — resetting via API";
                QString newTunnelId, newUrl;
                if (createTunnelViaApi(newTunnelId, newUrl)) {
                    m_TunnelId = newTunnelId;
                    if (!newUrl.isEmpty())
                        m_PublicUrl = newUrl;
                    m_Retried = true;
                    m_LastOutput.clear();

                    // Clean up the finished process before retrying
                    m_Process->deleteLater();
                    m_Process = nullptr;

                    launchNport();
                    return;
                }
            }
        }

        // Report error if we never got a ready signal
        if (!wasActive) {
            m_LastError = "nport exited with code " + QString::number(exitCode)
                          + " (" + (status == QProcess::CrashExit ? "crash" : "normal") + ")";
            emit tunnelError(m_LastError);
        }

        emit tunnelStopped();

        m_Process->deleteLater();
        m_Process = nullptr;
    });

    m_LastError.clear();

    // Build nport command:
    //   nport.exe <targetPort> -s moonlightweb-<subdomain>
    QStringList args;
    args << QString::number(m_TargetPort) << "-s" << buildSubdomain();

    qInfo() << "[NportClient] Launching nport:" << m_NportPath;
    qInfo() << "[NportClient] Args:" << m_TargetPort << "-s" << buildSubdomain();

    m_Process->start(m_NportPath, args);

    if (!m_Process->waitForStarted(kNportStartTimeoutMs)) {
        QString err = "Failed to start nport: " + m_Process->errorString();
        m_LastError = err;
        qWarning() << "[NportClient]" << err;
        emit tunnelError(err);

        m_Process->deleteLater();
        m_Process = nullptr;
        return;
    }

    // Start timeout: if nport doesn't signal ready within the timeout, report error.
    if (m_StartTimeoutTimer) {
        m_StartTimeoutTimer->stop();
        m_StartTimeoutTimer->deleteLater();
    }
    m_StartTimeoutTimer = new QTimer(this);
    m_StartTimeoutTimer->setSingleShot(true);
    connect(m_StartTimeoutTimer, &QTimer::timeout, this, [this]() {
        if (!m_Active && m_Process && m_Process->state() == QProcess::Running) {
            // One final check on accumulated output
            checkNportOutput();

            if (!m_Active) {
                qWarning() << "[NportClient] nport start timed out (no ready signal within"
                           << (kNportStartTimeoutMs / 1000) << "s) — stopping";
                stop();
                QString err = "nport start timed out (no ready signal within "
                              + QString::number(kNportStartTimeoutMs / 1000) + "s)";
                m_LastError = err;
                emit tunnelError(err);
            }
        }
    });
    m_StartTimeoutTimer->start(kNportStartTimeoutMs);
}

// ---------------------------------------------------------------------------
// Process slots
// ---------------------------------------------------------------------------

void NportClient::onNportStdout()
{
    QByteArray data = m_Process->readAllStandardOutput();
    m_LastOutput += QString::fromUtf8(data);

    QString text = QString::fromUtf8(data).trimmed();
    if (!text.isEmpty()) {
        qInfo() << "[NportClient] nport:" << text;
    }

    checkNportOutput();
}

void NportClient::onNportStderr()
{
    QByteArray data = m_Process->readAllStandardError();
    m_LastOutput += QString::fromUtf8(data);

    QString text = QString::fromUtf8(data).trimmed();
    if (!text.isEmpty()) {
        qInfo() << "[NportClient] nport stderr:" << text;
    }

    checkNportOutput();
}

void NportClient::onNportError(QProcess::ProcessError error)
{
    Q_UNUSED(error);
    // Don't emit tunnelError here — the connected finished signal will handle it.
    // Just log for debugging.
    qWarning() << "[NportClient] nport process error occurred:"
               << m_Process->errorString();
}

void NportClient::checkNportOutput()
{
    if (m_Active || m_Process == nullptr)
        return;

    // Detect tunnel ready.
    // nport prints output lines containing "ready" or "running" when the
    // tunnel is established and accepting connections.
    if (m_LastOutput.contains("ready", Qt::CaseInsensitive)
        || m_LastOutput.contains("running", Qt::CaseInsensitive))
    {
        m_Active = true;
        m_LastError.clear();

        // Use the URL from the API if we pre-created the tunnel.
        // Otherwise, construct it from the subdomain.
        if (m_PublicUrl.isEmpty()) {
            m_PublicUrl = QStringLiteral("https://") + buildSubdomain()
                          + QStringLiteral(".nport.link");
        }

        qInfo() << "[NportClient] Tunnel ready at" << m_PublicUrl;
        emit tunnelReady(m_PublicUrl);

        // Start auto-refresh timer now that the tunnel is connected
        m_RefreshTimer->start(kRefreshIntervalMs);
        qInfo() << "[NportClient] Auto-refresh scheduled in"
                << (kRefreshIntervalMs / 1000 / 60) << "minutes";
    }
}

// ---------------------------------------------------------------------------
// Graceful shutdown (Ctrl+C) and force kill
// ---------------------------------------------------------------------------

bool NportClient::sendCtrlC()
{
    if (!m_Process || m_Process->state() == QProcess::NotRunning)
        return false;

#ifdef Q_OS_WIN
    qint64 pid = m_Process->processId();
    if (pid <= 0) return false;

    qInfo() << "[NportClient] Sending Ctrl+C to nport (PID" << pid << ") —"
            << "this triggers nport's cleanup() which calls the DELETE API with tunnelId";

    // Make our process ignore Ctrl+C so we don't terminate ourselves
    SetConsoleCtrlHandler(NULL, TRUE);

    // Send Ctrl+C to the entire process group (nport is our child, same group).
    // nport receives Ctrl+C -> Node.js SIGINT handler fires ->
    //   TunnelOrchestrator.cleanup() -> kills cloudflared -> DELETE API -> exit(0)
    // cloudflared also receives Ctrl+C -> exits gracefully
    if (GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
        qInfo() << "[NportClient] Ctrl+C sent, waiting up to 10s for graceful exit...";
        if (m_Process->waitForFinished(10000)) {
            qInfo() << "[NportClient] nport exited gracefully after Ctrl+C";
            SetConsoleCtrlHandler(NULL, FALSE);
            return true;
        }
        qWarning() << "[NportClient] nport did not exit within 10s after Ctrl+C";
    } else {
        qWarning() << "[NportClient] GenerateConsoleCtrlEvent failed:"
                   << QString::number(::GetLastError());
    }

    SetConsoleCtrlHandler(NULL, FALSE);
    return false;
#else
    // Unix: send SIGINT via QProcess::terminate()
    m_Process->terminate();
    if (m_Process->waitForFinished(10000))
        return true;
    return false;
#endif
}

void NportClient::forceKill()
{
    if (!m_Process || m_Process->state() == QProcess::NotRunning)
        return;

    qint64 pid = m_Process->processId();
    qInfo() << "[NportClient] Force killing nport (PID" << pid << ")";

#ifdef Q_OS_WIN
    if (pid > 0) {
        QProcess taskkill;
        taskkill.start("taskkill", QStringList()
            << "/T" << "/PID" << QString::number(pid) << "/F");
        taskkill.waitForFinished(5000);
    }
#endif

    // The finished signal may have fired during taskkill's waitForFinished,
    // which nullified m_Process. Only proceed if still valid.
    if (!m_Process)
        return;

    if (m_Process->state() != QProcess::NotRunning) {
        m_Process->terminate();
        if (!m_Process->waitForFinished(5000)) {
            m_Process->kill();
            m_Process->waitForFinished(3000);
        }
    }
}

// ---------------------------------------------------------------------------
// Refresh mechanism
// ---------------------------------------------------------------------------

void NportClient::onRefreshTimeout()
{
    qInfo() << "[NportClient] Refresh timer fired (every"
            << (kRefreshIntervalMs / 1000 / 60) << "min)";

    if (m_RefreshPaused) {
        qInfo() << "[NportClient] Refresh paused (signaling in progress) — deferring";
        m_PendingRefresh = true;
        return;
    }

    m_PendingRefresh = false;
    qInfo() << "[NportClient] Restarting tunnel...";

    QString subdomain = m_Subdomain;
    stop();
    m_Subdomain = subdomain;
    doStart();
}

// ---------------------------------------------------------------------------
// Subdomain helpers
// ---------------------------------------------------------------------------

QString NportClient::buildSubdomain() const
{
    return kSubdomainPrefix + m_Subdomain;
}
