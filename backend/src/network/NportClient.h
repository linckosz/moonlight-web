#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>

/**
 * @brief Manages an nport.link tunnel via the nport CLI binary.
 *
 * Workflow:
 *   1. Pre-create the tunnel via REST API (POST) to obtain tunnelId + public URL
 *   2. Launch nport.exe <targetPort> -s moonlightweb-<subdomain>
 *   3. If nport fails with "already in use", reset via API and retry (max 1)
 *   4. On stop: send Ctrl+C to nport so its cleanup() runs (proper DELETE with tunnelId)
 *   5. Fallback to taskkill /T /F if nport doesn't exit within 10s
 *
 * LAN-only mode: if nport binary is not found, isAvailable() returns false
 * and tunneling is skipped.
 */
class NportClient : public QObject
{
    Q_OBJECT

public:
    explicit NportClient(QObject* parent = nullptr);
    ~NportClient();

    /// Port that the tunnel should forward to (default 80 = HTTP server).
    void setTargetPort(quint16 port) { m_TargetPort = port; }

    /// The subdomain hex id (8 chars), e.g. "a1b2c3d4".
    /// The "moonlightweb-" prefix is prepended automatically.
    void setSubdomain(const QString& subdomain) { m_Subdomain = subdomain.trimmed().toLower(); }

    /// Start the tunnel: pre-create via API, then launch nport.
    void start();

    /// Tear down the tunnel: send Ctrl+C to nport, wait 10s, fallback to kill.
    void stop();

    bool isActive() const { return m_Active; }

    /// Returns true if nport binary was found and subdomain is set.
    bool isAvailable() const;

    /// Full URL of the tunnel, e.g. "https://moonlightweb-a1b2c3d4.nport.link".
    QString publicUrl() const { return m_PublicUrl; }

    /// The configured subdomain name.
    QString subdomain() const { return m_Subdomain; }

    /// Tunnel ID from the nport API (for reference/monitoring).
    QString tunnelId() const { return m_TunnelId; }

    /// Last error message (empty if none).
    QString lastError() const { return m_LastError; }

    /// Pause auto-refresh during signaling to avoid breaking the WS connection.
    void pauseRefresh() { m_RefreshPaused = true; }

    /// Resume auto-refresh. If a refresh was deferred, executes it now.
    void resumeRefresh();

signals:
    /// Emitted when the tunnel is ready for connections.
    void tunnelReady(const QString& publicUrl);

    /// Emitted when the tunnel fails (process error, API error, etc.).
    void tunnelError(const QString& errorMessage);

    /// Emitted when the tunnel process exits unexpectedly.
    void tunnelStopped();

private slots:
    void onNportStdout();
    void onNportStderr();
    void onNportError(QProcess::ProcessError error);
    void onRefreshTimeout();

private:
    /// Internal start — called by start() and deferred refresh.
    void doStart();

    /// Locate the nport binary bundled with the nport npm package.
    bool findNportBinary();

    /// POST to api.nport.link to create/reset the tunnel for our subdomain.
    /// Returns true on success, populates outTunnelId and outUrl with API response.
    bool createTunnelViaApi(QString& outTunnelId, QString& outUrl);

    /// Launch nport.exe <port> -s <subdomain>.
    void launchNport();

    /// Called after each stdout/stderr chunk to detect ready or error state.
    void checkNportOutput();

    /// Send Ctrl+C to the nport process (graceful shutdown on Windows).
    /// Returns true if the process exited gracefully within the timeout.
    bool sendCtrlC();

    /// Kill the nport process tree forcibly (taskkill /T /F on Windows, SIGKILL on Unix).
    void forceKill();

    /// Build the subdomain string: "moonlightweb-<hex>".
    QString buildSubdomain() const;

    QProcess* m_Process = nullptr;
    QTimer* m_RefreshTimer = nullptr;
    QTimer* m_StartTimeoutTimer = nullptr;
    quint16 m_TargetPort = 80;

    QString m_Subdomain;
    QString m_PublicUrl;
    QString m_TunnelId;
    QString m_NportPath;
    bool m_Active = false;
    bool m_RefreshPaused = false;
    bool m_PendingRefresh = false;
    bool m_Retried = false;
    QString m_LastOutput;
    QString m_LastError;

    QNetworkAccessManager* m_NetworkManager = nullptr;
};
