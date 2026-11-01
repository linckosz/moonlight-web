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

#include "ControlChannel.h"

#include <QHostAddress>
#include <QWebSocket>
#include "common/LoopbackKey.h"

#include <QWebSocketServer>

#include "common/Logger.h"

ControlChannel::ControlChannel(quint16 port, QObject* parent)
    : QObject(parent)
    , m_Port(port)
{}

ControlChannel::~ControlChannel()
{
    stop();
}

bool ControlChannel::start()
{
    if (m_Server) return true;

    m_Server = new QWebSocketServer(QStringLiteral("Moonlight-Control"),
                                    QWebSocketServer::NonSecureMode, this);
    // Loopback only: the channel is always reached through the HttpServer proxy,
    // which connects to 127.0.0.1. Never expose it directly on the network.
    if (!m_Server->listen(QHostAddress::LocalHost, m_Port)) {
        // Taken, typically by another edition on this PC (a --dev instance
        // beside an installed one derives the same port). Any free port does:
        // the proxy reads port() after start(). Keeping the taken number
        // instead handed this instance's tabs to the other instance's channel.
        Logger::warning(QStringLiteral("[ControlChannel] 127.0.0.1:%1 unavailable (%2), "
                                       "taking a free port")
                            .arg(m_Port)
                            .arg(m_Server->errorString()));
        if (!m_Server->listen(QHostAddress::LocalHost, 0)) {
            Logger::warning(QStringLiteral("[ControlChannel] Failed to listen — %1")
                                .arg(m_Server->errorString()));
            m_Server->deleteLater();
            m_Server = nullptr;
            m_Port = 0;
            return false;
        }
    }
    m_Port = m_Server->serverPort();
    connect(m_Server, &QWebSocketServer::newConnection, this, &ControlChannel::onNewConnection);
    Logger::info(QStringLiteral("[ControlChannel] Listening on 127.0.0.1:%1").arg(m_Port));
    return true;
}

void ControlChannel::stop()
{
    for (QWebSocket* client : m_Clients) {
        client->close();
        client->deleteLater();
    }
    m_Clients.clear();
    if (m_Server) {
        m_Server->close();
        m_Server->deleteLater();
        m_Server = nullptr;
    }
}

void ControlChannel::onNewConnection()
{
    if (!m_Server) return;
    while (QWebSocket* client = m_Server->nextPendingConnection()) {
        if (!mw::loopback::accepts(*client, mw::loopback::key())) {
            client->close(QWebSocketProtocol::CloseCodePolicyViolated);
            client->deleteLater();
            continue;
        }
        // Bounded: Qt takes a message of up to 2 GB by default, and buffers all of
        // it before anyone sees a byte. 16 MiB is far past anything this socket
        // legitimately carries.
        client->setMaxAllowedIncomingMessageSize(16 * 1024 * 1024);
        client->setMaxAllowedIncomingFrameSize(16 * 1024 * 1024);
        connect(client, &QWebSocket::disconnected, this, &ControlChannel::onDisconnected);
        m_Clients.insert(client);
    }
}

void ControlChannel::onDisconnected()
{
    QWebSocket* client = qobject_cast<QWebSocket*>(sender());
    if (!client) return;
    m_Clients.remove(client);
    client->deleteLater();
}

void ControlChannel::broadcastFocusAdmin()
{
    const QString msg = QStringLiteral("{\"type\":\"focus-admin\"}");
    for (QWebSocket* client : m_Clients)
        client->sendTextMessage(msg);
    Logger::info(QStringLiteral("[ControlChannel] focus-admin broadcast to %1 tab(s)")
                     .arg(m_Clients.size()));
}
