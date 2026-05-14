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

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Refresh interval: 3h30 (kill + restart cloudflared to keep the tunnel alive).
static constexpr int kRefreshIntervalMs = 3 * 3600 * 1000 + 30 * 60 * 1000;

/// Subdomain prefix used for the tunnel hostname.
static const QString kSubdomainPrefix = QStringLiteral("moonlightweb-");

/// Base URL for the nport API (POST to create, DELETE to release).
static const QString kApiBaseUrl = QStringLiteral("https://api.nport.link");

/// Timeout for nport API calls (POST create, DELETE release).
static constexpr int kApiTimeoutMs = 10000;

/// Timeout for waiting on cloudflared process to start.
static constexpr int kCloudflaredStartTimeoutMs = 15000;

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

    // Probe for bundled cloudflared binary once at construction
    findCloudflaredBinary();
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
    return !m_CloudflaredPath.isEmpty() && !m_Subdomain.isEmpty();
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

    if (m_CloudflaredPath.isEmpty()) {
        emit tunnelError("cloudflared binary not found — run LAN-only mode");
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
    // Release the tunnel via API first, then kill cloudflared
    releaseSubdomain();

    m_RefreshTimer->stop();
    m_TunnelId.clear();
    m_TunnelToken.clear();

    if (!m_Process) return;

    qInfo() << "[NportClient] Stopping cloudflared...";

    if (m_Process->state() != QProcess::NotRunning) {
        qint64 pid = m_Process->processId();
        if (pid > 0) {
            // Kill entire process tree (cloudflared may spawn children)
            QProcess taskkill;
            taskkill.start("taskkill", QStringList()
                << "/T" << "/PID" << QString::number(pid) << "/F");
            taskkill.waitForFinished(5000);
        }
        // Fallback if taskkill didn't work or pid was 0
        if (m_Process->state() != QProcess::NotRunning) {
            m_Process->terminate();
            if (!m_Process->waitForFinished(5000)) {
                m_Process->kill();
                m_Process->waitForFinished(3000);
            }
        }
    }

    m_Active = false;
    m_PublicUrl.clear();
    m_LastOutput.clear();

    m_Process->deleteLater();
    m_Process = nullptr;

    qInfo() << "[NportClient] Tunnel stopped";
}

void NportClient::releaseSubdomain()
{
    if (m_Subdomain.isEmpty()) {
        qInfo() << "[NportClient] No subdomain configured — nothing to release";
        return;
    }

    if (m_TunnelId.isEmpty()) {
        qInfo() << "[NportClient] No tunnel ID — nothing to release via API";
        return;
    }

    QString fullSubdomain = buildSubdomain();
    QUrl url(kApiBaseUrl);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["subdomain"] = fullSubdomain;
    body["tunnelId"] = m_TunnelId;
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    qInfo() << "[NportClient] Releasing tunnel:" << fullSubdomain
            << "ID:" << m_TunnelId;

    QNetworkReply* reply = m_NetworkManager->sendCustomRequest(
        request, "DELETE", payload);

    // Synchronous wait with timeout
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(kApiTimeoutMs);
    loop.exec();

    if (timeout.isActive()) {
        timeout.stop();
        int statusCode = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray responseData = reply->readAll();
        if (statusCode == 200) {
            qInfo() << "[NportClient] Tunnel released:" << fullSubdomain;
        } else {
            qWarning() << "[NportClient] Failed to release tunnel:"
                       << statusCode << QString::fromUtf8(responseData);
        }
    } else {
        qWarning() << "[NportClient] Tunnel release timed out";
        reply->abort();
    }

    reply->deleteLater();
}

// ---------------------------------------------------------------------------
// Internal start
// ---------------------------------------------------------------------------

void NportClient::doStart()
{
    m_LastOutput.clear();
    m_PublicUrl.clear();
    m_TunnelId.clear();
    m_TunnelToken.clear();

    // ---- Step 1: Create tunnel via nport API ----

    QUrl url(kApiBaseUrl);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["subdomain"] = buildSubdomain();
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    qInfo() << "[NportClient] Creating tunnel for subdomain:" << buildSubdomain();

    QNetworkReply* reply = m_NetworkManager->post(request, payload);

    // Synchronous wait for the API response
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(kApiTimeoutMs);
    loop.exec();

    if (!timeout.isActive()) {
        // Reply never arrived — timeout
        QString err = "nport API tunnel creation timed out";
        m_LastError = err;
        qWarning() << "[NportClient]" << err;
        emit tunnelError(err);
        reply->deleteLater();
        return;
    }
    timeout.stop();

    int statusCode = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray responseData = reply->readAll();
    reply->deleteLater();

    if (statusCode != 200) {
        QString err = QString("nport API returned HTTP %1: %2")
            .arg(statusCode).arg(QString::fromUtf8(responseData));
        m_LastError = err;
        qWarning() << "[NportClient]" << err;
        emit tunnelError(err);
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(responseData);
    if (!doc.isObject()) {
        QString err = "nport API returned invalid JSON";
        m_LastError = err;
        qWarning() << "[NportClient]" << err;
        emit tunnelError(err);
        return;
    }

    QJsonObject resp = doc.object();
    if (!resp.value("success").toBool()) {
        QString errMsg = resp.value("error").toString("Unknown error");
        QString err = "nport API error: " + errMsg;
        m_LastError = err;
        qWarning() << "[NportClient]" << err;
        emit tunnelError(err);
        return;
    }

    m_TunnelId = resp.value("tunnelId").toString();
    m_TunnelToken = resp.value("tunnelToken").toString();
    m_PublicUrl = resp.value("url").toString();

    if (m_TunnelId.isEmpty() || m_TunnelToken.isEmpty() || m_PublicUrl.isEmpty()) {
        QString err = "nport API response missing tunnelId/tunnelToken/url";
        m_LastError = err;
        qWarning() << "[NportClient]" << err;
        emit tunnelError(err);
        m_TunnelId.clear();
        m_TunnelToken.clear();
        m_PublicUrl.clear();
        return;
    }

    qInfo() << "[NportClient] Tunnel created:" << m_PublicUrl
            << "ID:" << m_TunnelId;

    // ---- Step 2: Launch cloudflared with the token ----
    launchCloudflared();

    // If cloudflared failed to start, release the tunnel we just created.
    // Otherwise the tunnelId stays reserved on the server with nothing connected.
    if (!m_Process) {
        qWarning() << "[NportClient] cloudflared did not start — releasing tunnel";
        releaseSubdomain();
    }
}

// ---------------------------------------------------------------------------
// Cloudflared binary detection
// ---------------------------------------------------------------------------

bool NportClient::findCloudflaredBinary()
{
    // Look for cloudflared.exe bundled with the nport npm package:
    //   runtime/nport/node_modules/nport/bin/cloudflared.exe

    QString appDir = QCoreApplication::applicationDirPath();

#ifdef Q_OS_WIN
    QString binaryName = QStringLiteral("cloudflared.exe");
#else
    QString binaryName = QStringLiteral("cloudflared");
#endif

    // Search paths, ordered by likelihood.
    // Relative to executable: build/release/ -> runtime/nport/node_modules/nport/bin/
    // Qt Creator: build/Desktop_Qt_6_11_0_MSVC2022_64bit-Debug/backend/debug/ -> runtime/...
    QStringList searchDirs;
    searchDirs << (appDir + "/../../../../../runtime/nport/node_modules/nport/bin");
    searchDirs << (appDir + "/../../../../runtime/nport/node_modules/nport/bin");
    searchDirs << (appDir + "/../../../runtime/nport/node_modules/nport/bin");
    searchDirs << (appDir + "/../../runtime/nport/node_modules/nport/bin");
    searchDirs << (appDir + "/../runtime/nport/node_modules/nport/bin");
    searchDirs << (appDir + "/runtime/nport/node_modules/nport/bin");

    for (const QString& dirPath : searchDirs) {
        QFileInfo fi(dirPath + "/" + binaryName);
        if (fi.exists() && fi.isFile()) {
            m_CloudflaredPath = QDir::toNativeSeparators(fi.absoluteFilePath());
            qInfo() << "[NportClient] Found cloudflared:" << m_CloudflaredPath;
            return true;
        }
    }

    // Smart fallback: walk up parent directories looking for runtime/nport/...
    {
        QDir dir(appDir);
        for (int i = 0; i < 8; ++i) {
            if (!dir.cdUp()) break;
            QString candidate = dir.absoluteFilePath(
                "runtime/nport/node_modules/nport/bin/" + binaryName);
            QFileInfo fi(candidate);
            if (fi.exists() && fi.isFile()) {
                m_CloudflaredPath = QDir::toNativeSeparators(fi.absoluteFilePath());
                qInfo() << "[NportClient] Found cloudflared (parent walk):" << m_CloudflaredPath;
                return true;
            }
        }
    }

    // Fallback: check PATH for cloudflared
    QProcess which;
#ifdef Q_OS_WIN
    which.start("where", QStringList() << "cloudflared.exe");
#else
    which.start("which", QStringList() << "cloudflared");
#endif

    if (which.waitForFinished(10000) && which.exitCode() == 0) {
        m_CloudflaredPath = QString::fromUtf8(which.readAllStandardOutput())
                                .trimmed().split('\n').first().trimmed();
        if (!m_CloudflaredPath.isEmpty()) {
            qInfo() << "[NportClient] Found cloudflared (via PATH):" << m_CloudflaredPath;
            return true;
        }
    }

    m_CloudflaredPath.clear();
    qWarning() << "[NportClient] cloudflared binary not found — tunnel unavailable";
    return false;
}

// ---------------------------------------------------------------------------
// Cloudflared launch
// ---------------------------------------------------------------------------

void NportClient::launchCloudflared()
{
    m_Process = new QProcess(this);
    m_Process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_Process, &QProcess::started,
            this, &NportClient::onProcessStarted);
    connect(m_Process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &NportClient::onProcessFinished);
    connect(m_Process, &QProcess::readyReadStandardOutput,
            this, &NportClient::onReadyReadStdout);
    connect(m_Process, &QProcess::readyReadStandardError,
            this, &NportClient::onReadyReadStderr);

    m_LastError.clear();

    // Build cloudflared command:
    //   cloudflared.exe tunnel run --token <tunnelToken>
    //
    // The tunnelToken is a JWT obtained from the nport API POST response.
    // cloudflared connects to Cloudflare's edge and proxies traffic to us.
    //
    // The tunnel URL is already known from the API response (m_PublicUrl).

    QStringList args;
    args << "tunnel" << "run" << "--token" << m_TunnelToken;

    qInfo() << "[NportClient] Launching cloudflared:" << m_CloudflaredPath;
    qInfo() << "[NportClient] Args: tunnel run --token <redacted>";

    m_Process->start(m_CloudflaredPath, args);

    if (!m_Process->waitForStarted(kCloudflaredStartTimeoutMs)) {
        QString err = "Failed to start cloudflared: " + m_Process->errorString();
        m_LastError = err;
        qWarning() << "[NportClient]" << err;
        emit tunnelError(err);

        m_Process->deleteLater();
        m_Process = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Process slots
// ---------------------------------------------------------------------------

void NportClient::onProcessStarted()
{
    qInfo() << "[NportClient] cloudflared started (PID"
            << m_Process->processId() << ")";
}

void NportClient::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    // Read any remaining output
    if (m_Process) {
        m_LastOutput += QString::fromUtf8(m_Process->readAllStandardOutput());
    }

    qInfo() << "[NportClient] cloudflared finished: exitCode=" << exitCode
            << "status=" << (status == QProcess::NormalExit ? "normal" : "crash");

    m_Active = false;

    if (status == QProcess::CrashExit) {
        m_LastError = "cloudflared crashed";
        emit tunnelError(m_LastError);
    } else if (exitCode != 0) {
        m_LastError = "cloudflared exited with code " + QString::number(exitCode);
        emit tunnelError(m_LastError);
    }

    emit tunnelStopped();

    m_Process->deleteLater();
    m_Process = nullptr;
}

void NportClient::onReadyReadStdout()
{
    QByteArray data = m_Process->readAllStandardOutput();
    m_LastOutput += QString::fromUtf8(data);

    QString text = QString::fromUtf8(data).trimmed();
    if (!text.isEmpty()) {
        qInfo() << "[NportClient] cloudflared:" << text;
    }

    // Detect tunnel readiness from cloudflared log output.
    // cloudflared prints lines like:
    //   "INF Registered tunnel connection connIndex=0 ..."
    //   "INF Connection registered connIndex=0 ..."
    if (!m_Active && (text.contains("Registered tunnel connection", Qt::CaseInsensitive)
                      || text.contains("Connection registered", Qt::CaseInsensitive))) {
        m_Active = true;
        m_LastError.clear();

        qInfo() << "[NportClient] Tunnel ready at" << m_PublicUrl;
        emit tunnelReady(m_PublicUrl);

        // Start auto-refresh timer now that the tunnel is connected
        m_RefreshTimer->start(kRefreshIntervalMs);
        qInfo() << "[NportClient] Auto-refresh scheduled in"
                << (kRefreshIntervalMs / 1000 / 60) << "minutes";
    }
}

void NportClient::onReadyReadStderr()
{
    // cloudflared may emit log or error info to stderr; log it for debugging
    QByteArray data = m_Process->readAllStandardError();
    m_LastOutput += QString::fromUtf8(data);  // keep for error analysis

    QString text = QString::fromUtf8(data).trimmed();
    if (!text.isEmpty()) {
        qInfo() << "[NportClient] cloudflared stderr:" << text;
    }

    // Also try detecting connection from stderr as a fallback
    if (!m_Active && (text.contains("Registered tunnel connection", Qt::CaseInsensitive)
                      || text.contains("Connection registered", Qt::CaseInsensitive))) {
        m_Active = true;
        m_LastError.clear();

        qInfo() << "[NportClient] Tunnel ready at" << m_PublicUrl;
        emit tunnelReady(m_PublicUrl);

        m_RefreshTimer->start(kRefreshIntervalMs);
        qInfo() << "[NportClient] Auto-refresh scheduled in"
                << (kRefreshIntervalMs / 1000 / 60) << "minutes";
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
