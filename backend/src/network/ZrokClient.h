#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>

/**
 * @brief Manages a zrok tunnel for WebRTC signaling WS exposure.
 *
 * zrok provides a public HTTPS endpoint that tunnels to a local non-secure
 * WebSocket server.  The browser connects via wss://<public-url> and zrok
 * forwards the connection (plain WS) to localhost:<targetPort>.
 *
 * Binary is bundled at backend/tools/{windows,linux,macos}/zrok[.exe] and located
 * automatically by findZrokBinary().  If the binary is not found, isAvailable()
 * returns false and the application continues in LAN-only mode.
 *
 * Lifecycle:
 *   1. First run: enable(token) + reserve(uniqueName, targetPort)
 *   2. Subsequent runs: share(reservedName)
 *   3. Process runs in background, tunnel stays open
 *   4. On stop: terminate QProcess
 */
class ZrokClient : public QObject
{
    Q_OBJECT

public:
    explicit ZrokClient(QObject* parent = nullptr);
    ~ZrokClient();

    /// Port that zrok should tunnel to (default 48001 = signaling WS).
    void setTargetPort(quint16 port) { m_TargetPort = port; }

    /// Token + reserved name from persisted settings.
    void setToken(const QString& token) { m_Token = token; }
    void setReservedName(const QString& name) { m_ReservedName = name; }

    /// Start the zrok tunnel using the configured token and reserved name.
    /// If no reserved name is set yet, emits tunnelError().
    void start();

    /// Tear down the tunnel.
    void stop();

    bool isActive() const { return m_Active; }

    /// Returns true if the zrok binary was found and is usable.
    /// If false, the application runs in LAN-only mode.
    bool isAvailable() const { return !m_ZrokBinaryPath.isEmpty(); }

    /// Full HTTPS URL of the zrok tunnel, e.g. "https://moonlightweb-a1b2.share.zrok.io".
    QString publicUrl() const { return m_PublicUrl; }

    /// The reserved share name, e.g. "moonlightweb-a1b2".
    QString reservedName() const { return m_ReservedName; }

signals:
    /// Emitted when the tunnel is ready for connections.
    void tunnelReady(const QString& publicUrl);

    /// Emitted when the tunnel fails (process error, parse error, etc.).
    void tunnelError(const QString& errorMessage);

    /// Emitted when the tunnel process exits unexpectedly.
    void tunnelStopped();

private slots:
    void onProcessStarted();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onReadyReadStdout();
    void onReadyReadStderr();

private:
    /// Parse the public URL from zrok's stdout.
    void parseShareOutput(const QByteArray& data);

    /// Locate the zrok binary.  Search order (per-platform subdir first, then flat):
    ///   1. <exe_dir>/tools/{windows,linux,macos}/zrok[.exe]
    ///   2. <exe_dir>/tools/zrok[.exe]  (legacy flat)
    ///   3. <cwd>/backend/tools/{...}/zrok[.exe]
    ///   4. <cwd>/backend/tools/zrok[.exe]  (legacy flat)
    ///   5. <cwd>/tools/{...}/zrok[.exe]
    ///   6. <cwd>/tools/zrok[.exe]  (legacy flat)
    ///   7. PATH (`where` / `which`)
    /// Returns empty string if not found.
    QString findZrokBinary() const;

    QProcess* m_Process = nullptr;
    quint16 m_TargetPort = 48001;

    QString m_Token;
    QString m_ReservedName;
    QString m_PublicUrl;

    /// Full path to the zrok binary, resolved by findZrokBinary().
    /// If empty, the binary was not found and LAN-only mode is used.
    QString m_ZrokBinaryPath;

    bool m_Active = false;
};
