#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>

/**
 * @brief Manages an nport.link tunnel via direct API calls + cloudflared.
 *
 * Replaces the previous nport CLI approach:
 *   1. POST https://api.nport.link to create a tunnel -> get tunnelToken + tunnelId + url
 *   2. Launch cloudflared tunnel run --token <tunnelToken>
 *   3. DELETE https://api.nport.link in stop() with subdomain + tunnelId
 *
 * The nport npm package is still required for its bundled cloudflared binary at
 * runtime/nport/node_modules/nport/bin/cloudflared.exe.
 *
 * Requirements:
 *   - cloudflared binary bundled with the nport npm package
 *   - Internet access to api.nport.link
 *
 * LAN-only mode: if cloudflared is not found, isAvailable() returns false
 * and tunneling is skipped.
 */
class NportClient : public QObject
{
    Q_OBJECT

public:
    explicit NportClient(QObject* parent = nullptr);
    ~NportClient();

    /// Port that the tunnel should forward to (default 443 = unified HTTPS/WS).
    void setTargetPort(quint16 port) { m_TargetPort = port; }

    /// The subdomain hex id (8 chars), e.g. "a1b2c3d4".
    /// The "moonlightweb-" prefix is prepended automatically.
    void setSubdomain(const QString& subdomain) { m_Subdomain = subdomain.trimmed().toLower(); }

    /// Start the tunnel: POST API + launch cloudflared with the token.
    /// If subdomain is not configured, emits tunnelError().
    void start();

    /// Tear down the tunnel: DELETE API + kill cloudflared process.
    void stop();

    /// Release the subdomain via nport API DELETE call (non-fatal if it fails).
    void releaseSubdomain();

    bool isActive() const { return m_Active; }

    /// Returns true if cloudflared binary was found and subdomain is set.
    bool isAvailable() const;

    /// Full URL of the tunnel, e.g. "https://moonlightweb-a1b2c3.nport.link".
    QString publicUrl() const { return m_PublicUrl; }

    /// The configured subdomain name.
    QString subdomain() const { return m_Subdomain; }

    /// Last error message (empty if none).
    QString lastError() const { return m_LastError; }

    /// Pause auto-refresh during signaling to avoid breaking the WS connection.
    /// If the timer fires while paused, refresh is deferred to resumeRefresh().
    void pauseRefresh() { m_RefreshPaused = true; }

    /// Resume auto-refresh. If a refresh was deferred, executes it now.
    void resumeRefresh();

signals:
    /// Emitted when the tunnel is ready for connections.
    void tunnelReady(const QString& publicUrl);

    /// Emitted when the tunnel fails (API error, process error, etc.).
    void tunnelError(const QString& errorMessage);

    /// Emitted when the tunnel process exits unexpectedly.
    void tunnelStopped();

private slots:
    void onProcessStarted();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onReadyReadStdout();
    void onReadyReadStderr();
    void onRefreshTimeout();

private:
    /// Internal start — called by start() and deferred refresh.
    void doStart();

    /// Locate the cloudflared binary bundled with the nport package.
    bool findCloudflaredBinary();

    /// Launch cloudflared tunnel run --token <m_TunnelToken>.
    void launchCloudflared();

    /// Build the subdomain string: "moonlightweb-<hex>".
    QString buildSubdomain() const;

    QProcess* m_Process = nullptr;
    QTimer* m_RefreshTimer = nullptr;
    quint16 m_TargetPort = 443;

    QString m_Subdomain;
    QString m_PublicUrl;
    QString m_TunnelId;
    QString m_TunnelToken;
    QString m_CloudflaredPath;
    bool m_Active = false;
    bool m_RefreshPaused = false;
    bool m_PendingRefresh = false;
    QString m_LastOutput;
    QString m_LastError;

    QNetworkAccessManager* m_NetworkManager = nullptr;
};
