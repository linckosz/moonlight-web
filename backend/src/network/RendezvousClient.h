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
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QString>
#include <QTimer>
#include <QWebSocket>

class AppSettings;

/**
 * @brief Holds this instance's line to the rendezvous server.
 *
 * What it is for
 * --------------
 * An instance used to be reachable because it published an A record pointing at
 * a home IP address. It no longer does. What makes it reachable now is this:
 * one outbound WebSocket, held open, over which the rendezvous server can reach
 * it at any moment. The line IS the reachability — there is nothing to publish
 * and nothing to poll.
 *
 * The reason it has to be a held connection rather than a periodic announcement
 * is that the address a browser actually needs — the reflexive candidate — is a
 * port that only exists while a connection is being made. It cannot be stored
 * and it cannot be re-used, so there is nothing an instance could usefully
 * announce in advance.
 *
 * What crosses it
 * ---------------
 * WebRTC signalling, and nothing else. No media, no input, no credentials. The
 * payloads are opaque to the server, which copies them between this line and
 * the browser that asked for this instance by identifier.
 *
 * Threading: Qt main thread only, like the rest of the network layer.
 */
class RendezvousClient : public QObject
{
    Q_OBJECT

public:
    explicit RendezvousClient(AppSettings* settings, QObject* parent = nullptr);
    ~RendezvousClient() override;

    /// Base URL of the rendezvous server: MW_RENDEZVOUS_URL, else
    /// "https://stream.{MW_DOMAIN}".
    static QString baseUrl();

    /// The address to hand a user: "{baseUrl}/{id}". Empty until claimed —
    /// an unclaimed id is an address that answers to nobody, and showing it
    /// would be worse than showing nothing.
    QString entryUrl() const;

    /// Draw an identity if needed, claim it, then hold the line. Safe to call
    /// repeatedly; a second call while running is a no-op.
    void start();

    /// Deliberate shutdown: drop the line and do NOT reconnect.
    void stop();

    bool isOnline() const { return m_Online; }

signals:
    /// A browser arrived and the server opened a session for it.
    void sessionOpened(const QString& sessionId);

    /// That session is finished — the browser left, or the server dropped it.
    void sessionClosed(const QString& sessionId);

    /// One signalling payload, verbatim as the browser sent it.
    void signalReceived(const QString& sessionId, const QJsonObject& payload);

    /// The held line came up or went down.
    void onlineChanged(bool online);

    /// The identifier changed (first claim, or a forced redraw). Entry points —
    /// tray menu, desktop shortcut, admin page — follow this.
    void identityChanged(const QString& id);

public slots:
    /// Send one signalling payload toward the browser on `sessionId`.
    void sendSignal(const QString& sessionId, const QJsonObject& payload);

    /// Tell the server this session is over.
    void closeSession(const QString& sessionId);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString& text);
    void onSocketError(QAbstractSocket::SocketError error);
    void onPong(quint64 elapsed, const QByteArray& payload);
    void onKeepAlive();
    void onWatchdog();

private:
    void ensureIdentity();
    void claim();
    void openLine();
    void scheduleRetry(const char* why);
    void setOnline(bool online);
    void sendFrame(const QJsonObject& frame);

    AppSettings* m_Settings = nullptr;
    QNetworkAccessManager* m_Net = nullptr;
    QWebSocket m_Socket;

    QTimer m_RetryTimer;     // reconnect / re-claim backoff
    QTimer m_KeepAliveTimer; // our own ping, so a dead line is noticed
    QTimer m_Watchdog;       // fires when a pong never came back

    bool m_Running = false;
    bool m_Online = false;
    bool m_Claiming = false;
    int m_RetryCount = 0;

    /// How many times this process has been allowed to redraw its identity.
    /// Bounded on purpose: a redraw loop would burn through the server's
    /// per-address claim budget and leave orphaned entries behind it.
    int m_IdentityRedraws = 0;
};
