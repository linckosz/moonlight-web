/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

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
#include "server/ConnectionGuard.h"
#include "server/CertManager.h"

class RestRouter;
class StaticFileHandler;
class AuthManager;
class QTimer;

class HttpServer : public QObject
{
    Q_OBJECT

public:
    explicit HttpServer(quint16 httpPort = 80, quint16 httpsPort = 443, QObject* parent = nullptr);
    ~HttpServer() override;

    /// Start servers. preferredHttpsPort is tried first, then fallback ranges.
    bool start(quint16 preferredHttpsPort = 443);
    void stop();

    RestRouter* router() const { return m_Router; }
    QSslConfiguration sslConfiguration() const { return m_Certs.publicConfig(); }

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

    /// Add a second HTTPS listener on @p port that serves the same app, without
    /// touching the primary listener. Used for port parity: when the public
    /// domain must be reached on a non-default external port (another instance
    /// owns the default on the router), we listen on that port for the domain
    /// while the primary keeps serving localhost/LAN — so the admin page never
    /// loses the origin it is currently loaded on. Idempotent: returns true when
    /// a listener on @p port is already up. @p port 0 is a no-op (false).
    bool addSecondaryHttpsListener(quint16 port);

    /// Tear down the secondary HTTPS listener, if any.
    void removeSecondaryHttpsListener();

    /// The port of the secondary HTTPS listener (0 when none).
    quint16 secondaryHttpsPort() const { return m_SecondaryHttpsPort; }

    /// Set the cert_pem value from settings (env var name or file path).
    /// When non-empty, loadCert() tries to resolve it before auto-discovering.
    void setCertPem(const QString& value) { m_Certs.setCertPem(value); }

    /// Set the cert_key value from settings (env var name or file path).
    void setCertKey(const QString& value) { m_Certs.setCertKey(value); }

    /// Set the expected CN (Common Name) for certificate matching.
    /// When set, CertManager filters certificates whose CN matches this domain.
    void setDomain(const QString& domain) { m_Certs.setDomain(domain); }

    QString domain() const { return m_Certs.domain(); }

    /// Check whether a client address (from peerAddress()) is localhost
    /// or a loopback address (127.0.0.1, ::1, or any loopback).
    static bool isLocalRequest(const QString& addr);

    /// Set the AuthManager for PIN-based authentication of remote requests.
    void setAuthManager(AuthManager* am) { m_AuthManager = am; }
    AuthManager* authManager() const { return m_AuthManager; }

    /// Check whether the request carries a valid session cookie.
    /// Returns true if no AuthManager is set (auth disabled).
    bool isAuthenticated(const HttpRequest& req) const;

    /// Extract the raw mw_session cookie token from a request ("" if absent).
    static QString sessionTokenFromRequest(const HttpRequest& req);

signals:
    void started(quint16 port);
    void serverError(const QString& message);

private slots:
    void onHttpConnection();
    void onReadyRead();
    void onDisconnected();

private:
    /// Create (and listen on) an SslServer for @p port using the shared cert
    /// configs and socket-registration wiring. Returns nullptr if the bind
    /// fails. Used by both the primary start() path and the secondary listener.
    QTcpServer* createHttpsServer(quint16 port);

    void processRequest(QTcpSocket* socket, const QByteArray& requestData);
    void onReadyReadSocket(QTcpSocket* socket);
    void sendResponse(QTcpSocket* socket, const HttpResponse& response);
    void handleWebSocketUpgrade(QTcpSocket* clientSocket, const QByteArray& requestData);
    bool isLanHost(const QString& host) const;

    /// Push CertManager's public config onto the live SslServer as its public
    /// (SNI default) config. Called after a TLS reload so new connections use the
    /// freshly issued certificate without restarting the server.
    void applyPublicSslConfig();

    QTcpServer* m_HttpServer;
    QTcpServer* m_HttpsServer;

    /// Optional second HTTPS listener (port parity for the public domain). Serves
    /// the same app as the primary; the primary keeps serving localhost/LAN.
    QTcpServer* m_SecondaryHttpsServer = nullptr;
    quint16 m_SecondaryHttpsPort = 0;

    /// Owns all TLS certificate discovery / loading / renewal / self-signed
    /// generation. Produces the public + local (SNI) SSL configurations.
    CertManager m_Certs;

    RestRouter* m_Router;
    StaticFileHandler* m_StaticFiles;
    quint16 m_HttpPort;
    quint16 m_HttpsPort;
    quint16 m_ActiveHttpsPort = 0;

    quint16 m_SignalingPort = 48001;
    quint16 m_StreamRelayPort = 48002;

    QMap<QTcpSocket*, QByteArray> m_Buffers;
    QSet<QTcpSocket*> m_PendingAsyncSockets;

    /// PIN-based authentication manager (nullable — auth disabled when null).
    AuthManager* m_AuthManager = nullptr;

    /// Per-IP abuse mitigation (connection-flood + auth-failure ban). Checked at
    /// accept() time for both HTTP and HTTPS listeners.
    ConnectionGuard m_ConnGuard;
    QTimer* m_GuardPurgeTimer = nullptr;

    /// Reject a freshly accepted socket when its peer IP is banned or floods.
    /// Returns true when the connection was rejected (socket scheduled to close).
    bool rejectIfAbusive(QTcpSocket* socket);

    static constexpr int ASYNC_TIMEOUT_MS = 30000;
};
