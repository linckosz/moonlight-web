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

#include "common/Types.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

class AppSettings;
class AuthManager;
class HttpServer;
class RendezvousClient;
class QWebSocket;

namespace rtc {
class PeerConnection;
class DataChannel;
} // namespace rtc

/**
 * @brief The application, delivered to a browser that has no way to reach it.
 *
 * The problem this solves
 * ----------------------
 * An instance no longer publishes a name in DNS and no longer opens 80 or 443,
 * so a browser cannot make an HTTP request to it. But the interface still has
 * to come FROM this machine — serving it from the introduction server would put
 * the author's code between the user and their own desktop, which is the whole
 * thing the architecture refuses.
 *
 * So the browser loads a few kilobytes of bootstrap from the introduction
 * server, that bootstrap opens a WebRTC connection straight to this machine, and
 * everything after the first byte — the interface, the API, the streaming
 * signalling — travels over that connection. This class is the host end of it.
 *
 * What it is NOT
 * --------------
 * It is not a second web server. Requests coming off the tunnel go through
 * HttpServer::serveRequest(), the same access decision and the same router a
 * socket request goes through. A tunnel with its own copy of the pipeline would
 * drift, and the half that drifts is always the guard.
 *
 * The one deliberate difference is the arrival flag: §4.3 of the architecture
 * says a request that came through the tunnel never obtains the local exemption.
 * That rule is enforced inside serveRequest(), not here, so it cannot be
 * forgotten by a future caller.
 *
 * Binding the channel
 * -------------------
 * The signalling for this connection is relayed by the introduction server,
 * which is exactly the party MW-BIND-v1 exists to keep out of the middle. The
 * exchange here is the one from docs/design/pairing-signature.md, with one
 * difference forced by the situation: a browser arriving at the login screen has
 * no session yet, so the host cannot look its key up. It presents it instead,
 * and the host signs against the key it was shown.
 *
 * Be precise about what that buys. The signature proves the browser holds the
 * private half of the key it named, and it binds both DTLS fingerprints so the
 * relay cannot substitute either. It proves NOTHING about who that browser is —
 * authorisation is still the session cookie, established with the PIN, over this
 * channel. What the binding removes is the man in the middle, not the login.
 *
 * The first connection from a given browser trusts the host key on sight, as SSH
 * does. From then on a changed host key is refused. The host's key fingerprint
 * is printed in the log and shown on the local admin page, so the pairing can be
 * compared out of band by anyone who wants to.
 *
 * Threading: libdatachannel calls back on its own threads. Everything here hops
 * to the Qt main thread before touching state, and peers are looked up by id at
 * that point rather than captured, so a session that ended in between is a
 * missing key rather than a dangling pointer.
 */
class ControlTunnel : public QObject
{
    Q_OBJECT

public:
    ControlTunnel(HttpServer* http, AuthManager* auth, AppSettings* settings,
                  RendezvousClient* rendezvous, QObject* parent = nullptr);
    ~ControlTunnel() override;

    /// How many browsers are connected through the tunnel right now.
    int peerCount() const { return static_cast<int>(m_Peers.size()); }

    /// Colon-separated SHA-256 of this host's signing key, as a browser sees it.
    /// Shown on the admin page so a user can compare what their browser trusted
    /// against what this machine actually holds — the out-of-band check that
    /// trust-on-first-use otherwise leaves them without.
    QString hostKeyFingerprint() const;

    // The wire format lives in server/TunnelFrame.h, on its own and free of
    // libdatachannel — its other half is JavaScript on a machine we will never
    // see, so it is the one part of this that has to be testable in isolation.

private slots:
    void onSessionOpened(const QString& sessionId);
    void onSessionClosed(const QString& sessionId);
    void onSignal(const QString& sessionId, const QJsonObject& payload);

private:
    /// One browser, from the moment the introduction server announces it to the
    /// moment its line drops.
    struct Peer
    {
        QString sessionId;

        std::shared_ptr<rtc::PeerConnection> pc;
        std::shared_ptr<rtc::DataChannel> dc;

        // ── MW-BIND-v1, per connection ──────────────────────────────────────
        bool helloSeen = false;
        bool answerVerified = false;
        QByteArray browserSpki; ///< as presented in the hello, never assumed paired
        QString browserKeyId;
        QByteArray nonceBrowser;
        QByteArray nonceHost;
        QString localFingerprint;
        std::string pendingOffer;

        /// Requests handed to the router and not yet answered. Bounded so one
        /// browser cannot make the host queue work without limit.
        int inFlight = 0;

        /// Streaming signalling sockets this browser opened through us, by the
        /// id it gave them.
        QHash<quint32, QPointer<QWebSocket>> sockets;

        /// Queued outbound frames, drained as the channel makes room.
        QList<QByteArray> outbox;
        bool draining = false;
    };

    Peer* peer(const QString& sessionId);
    void dropPeer(const QString& sessionId, const QString& why);

    /// Where this browser actually is, as ICE settled it — or a label while
    /// there is no selected pair yet. Never a claim the browser made.
    QString peerAddress(const Peer& p) const;

    void handleHello(Peer& p, const QJsonObject& msg);
    void handleAnswer(Peer& p, const QJsonObject& msg);
    void handleIce(Peer& p, const QJsonObject& msg);
    void sendSignedOffer(Peer& p);

    /// Refuse this connection and take it down. The peer is DESTROYED — every
    /// caller must return immediately afterwards.
    void abort(Peer& p, const QString& why);

    void onChannelMessage(const QString& sessionId, const QByteArray& message);
    void handleRequestFrame(Peer& p, quint32 id, const QByteArray& payload);
    void handleWsOpen(Peer& p, quint32 id, const QByteArray& payload);
    void handleWsText(Peer& p, quint32 id, const QString& text);
    void closeWs(Peer& p, quint32 id, const QString& reason);

    void sendFrame(Peer& p, const QByteArray& frame);
    void sendResponse(const QString& sessionId, quint32 id, const HttpResponse& response,
                      const QString& hostHeader);
    void drain(Peer& p);

    HttpServer* m_Http = nullptr;
    AuthManager* m_Auth = nullptr;
    AppSettings* m_Settings = nullptr;
    RendezvousClient* m_Rendezvous = nullptr;

    /// Peers are held behind pointers so a reference into one stays valid while
    /// another connects: Qt 6's QHash moves its values when it grows.
    QHash<QString, std::shared_ptr<Peer>> m_Peers;
};
