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
#include <QSet>

class QWebSocketServer;
class QWebSocket;

// Persistent control channel between the backend and every open browser tab of
// the app (hosts/admin page), used to avoid duplicate browser tabs.
//
// The app is windowless: clicking the Desktop shortcut (or launching the exe
// again) spawns a throwaway process that detects the single-instance lock and
// asks the RUNNING instance — over POST /api/local/focus — to surface the admin
// page. If any tab holds a control-channel WebSocket open, the running instance
// broadcasts {"type":"focus-admin"} and that tab navigates to /admin (no new
// tab). If no tab is connected, the running instance opens a fresh browser tab.
//
// A WebSocket (not polling) is used on purpose: browsers keep it alive even for
// backgrounded tabs — the exact case here (the user clicks the shortcut because
// the existing tab is buried) — and throttle timers, not sockets.
//
// The HttpServer proxies GET /ws/control (same HTTPS port as the page) to this
// local WebSocket server, mirroring the /ws (signaling) and /ws/stream routes.
class ControlChannel : public QObject
{
    Q_OBJECT

public:
    explicit ControlChannel(quint16 port, QObject* parent = nullptr);
    ~ControlChannel() override;

    /// Start listening on 127.0.0.1:port. Returns false if the bind fails.
    bool start();
    void stop();

    /// The local port the WebSocket server bound to (0 when not started).
    quint16 port() const { return m_Port; }

    /// True when at least one browser tab holds the control channel open.
    bool hasClients() const { return !m_Clients.isEmpty(); }

    /// Broadcast {"type":"focus-admin"} to every connected tab so it opens the
    /// admin overlay and (best-effort) brings itself to the foreground.
    void broadcastFocusAdmin();

private slots:
    void onNewConnection();
    void onDisconnected();

private:
    QWebSocketServer* m_Server = nullptr;
    QSet<QWebSocket*> m_Clients;
    quint16 m_Port = 0;
};
