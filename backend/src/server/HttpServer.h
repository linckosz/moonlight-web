#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QNetworkInterface>
#include <QList>
#include <functional>
#include "common/Types.h"

class RestRouter;
class StaticFileHandler;
class AuthManager;

class HttpServer : public QObject
{
    Q_OBJECT

public:
    explicit HttpServer(quint16 httpPort = 80, quint16 httpsPort = 443,
                        QObject* parent = nullptr);
    ~HttpServer();

    /// Start servers. preferredHttpsPort is tried first, then fallback ranges.
    bool start(quint16 preferredHttpsPort = 443);
    void stop();

    RestRouter* router() const { return m_Router; }
    QSslConfiguration sslConfiguration() const { return m_SslConfig; }

    /// Port of the local signaling WebSocket server (for WS→proxy routing).
    void setSignalingPort(quint16 port) { m_SignalingPort = port; }

    /// Port of the local stream relay WebSocket server (legacy WSS mode).
    /// The HttpServer proxies wss://host/ws/stream to this local port.
    void setStreamRelayPort(quint16 port) { m_StreamRelayPort = port; }

    /// The port the HTTPS server actually bound to (0 if not started).
    quint16 activeHttpsPort() const { return m_ActiveHttpsPort; }

    /// The HTTP redirect server port.
    quint16 httpPort() const { return m_HttpPort; }

    /// Reload TLS configuration from cert/ directory.
    /// Called after Let's Encrypt certificate acquisition.
    bool reloadTls();

    /// Change the HTTPS port at runtime. Stops current listeners and
    /// re-binds to the new port. Returns true if the new port binds OK.
    /// All existing connections are closed during the transition.
    bool changeHttpsPort(quint16 newPort);

    /// Set the cert_pem value from settings (env var name or file path).
    /// When non-empty, loadCert() tries to resolve it before auto-discovering.
    void setCertPem(const QString& value) { m_CertPem = value; }

    /// Set the cert_key value from settings (env var name or file path).
    void setCertKey(const QString& value) { m_CertKey = value; }

    /// Set the expected CN (Common Name) for certificate matching.
    /// When set, findCertDir() filters certificates whose CN matches this domain.
    void setDomain(const QString& domain) { m_Domain = domain; }

    QString domain() const { return m_Domain; }

    /// Check whether a client address (from peerAddress()) is localhost
    /// or a loopback address (127.0.0.1, ::1, or any loopback).
    static bool isLocalRequest(const QString& addr);

    /// Set the AuthManager for PIN-based authentication of remote requests.
    void setAuthManager(AuthManager* am) { m_AuthManager = am; }
    AuthManager* authManager() const { return m_AuthManager; }

    /// Check whether the request carries a valid session cookie.
    /// Returns true if no AuthManager is set (auth disabled).
    bool isAuthenticated(const HttpRequest& req) const;

signals:
    void started(quint16 port);
    void serverError(const QString& message);

private slots:
    void onHttpConnection();
    void onReadyRead();
    void onDisconnected();

private:
    void processRequest(QTcpSocket* socket, const QByteArray& requestData);
    void onReadyReadSocket(QTcpSocket* socket);
    void sendResponse(QTcpSocket* socket, const HttpResponse& response);
    void handleWebSocketUpgrade(QTcpSocket* clientSocket, const QByteArray& requestData);
    HttpRequest parseRequest(const QByteArray& raw) const;
    bool isLanHost(const QString& host) const;
    bool loadCert();
    QString findCertDir();
    bool loadCertFiles(const QString& certDir);
    bool loadCertFilesExplicit(const QString& certFilePath);

    /// Recursively scan cert directories for a PEM file whose CN matches domain.
    /// Returns the directory containing the matching cert + key.pem, or empty.
    QString findCertByDomain(const QString& domain);

    /// Extract the Common Name from a PEM certificate file. Returns empty if unparseable.
    QString extractCertCN(const QString& pemPath);

    /// Scan directory recursively for a *.pem file containing a valid private key.
    /// Returns the file path or QString() if none found.
    QString scanKeyInDir(const QString& dir) const;

    /// Load a private key from non-file sources.
    /// Priority: MW_CERT_KEY env var, then built-in key (MW_HAS_BUILTIN_KEY).
    /// Returns a null QSslKey if neither is available.
    QSslKey loadKeyFromEnv() const;

    /// Scan directory recursively for a *.pem file containing a valid certificate.
    /// If domain is non-empty, only certificates whose CN matches are considered.
    /// Returns the file path or QString() if none found.
    QString scanCertInDir(const QString& dir, const QString& domain = QString()) const;

    bool renewWithLego();
    bool generateSelfSignedCert();

    /// Ensure the local self-signed certificate exists in AppData/cert/ with
    /// proper SANs (localhost, LAN IPs) and load it into m_LocalSslConfig.
    /// Called after loadCert() to set up the second SSL configuration for SNI.
    void ensureLocalSslConfig();

    QTcpServer* m_HttpServer;
    QTcpServer* m_HttpsServer;

    /// Default SSL configuration (served to public-domain clients via PositiveSSL/LE).
    QSslConfiguration m_SslConfig;

    /// Self-signed SSL configuration with LAN SANs (served to localhost / LAN IP clients).
    QSslConfiguration m_LocalSslConfig;

    RestRouter* m_Router;
    StaticFileHandler* m_StaticFiles;
    quint16 m_HttpPort;
    quint16 m_HttpsPort;
    quint16 m_ActiveHttpsPort = 0;

    quint16 m_SignalingPort = 48001;
    quint16 m_StreamRelayPort = 48002;

    QMap<QTcpSocket*, QByteArray> m_Buffers;
    QSet<QTcpSocket*> m_PendingAsyncSockets;

    /// cert_pem value from settings: env var name or file path.
    /// Set via setCertPem() before loadCert() or reloadTls().
    QString m_CertPem;

    /// cert_key value from settings: env var name or file path.
    QString m_CertKey;

    /// Expected Common Name for certificate matching (e.g. "brunoocto.moonlightweb.top").
    /// Set via setDomain() before loadCert() or reloadTls().
    QString m_Domain;

    /// PIN-based authentication manager (nullable — auth disabled when null).
    AuthManager* m_AuthManager = nullptr;

    /// Resolve a cert_pem/cert_key value to PEM data.
    /// Tries qgetenv(value) first, then QFile(value).
    static QByteArray resolvePemValue(const QString& value);

    static constexpr int ASYNC_TIMEOUT_MS = 30000;
};
