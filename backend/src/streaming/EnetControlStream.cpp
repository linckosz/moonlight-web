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

#include "EnetControlStream.h"

#include <QDebug>
#include <QRandomGenerator>
#include <QtEndian>

// Channel IDs
static const quint8 CTRL_CHANNEL_GENERIC = 0x00;
static const quint8 CTRL_CHANNEL_KEYBOARD = 0x02;
static const quint8 CTRL_CHANNEL_MOUSE = 0x03;
static const quint8 CTRL_CHANNEL_COUNT = 0x30;

// Packet types (Gen5+)
static const uint16_t PKT_TYPE_START_A = 0x0305;
static const uint16_t PKT_TYPE_START_B = 0x0307;
static const uint16_t PKT_TYPE_INPUT = 0x0206;

bool EnetControlStream::s_EnetInitialized = false;

EnetControlStream::EnetControlStream(const QString& host, quint16 port, uint32_t connectData,
                                     QObject* parent)
    : QObject(parent)
    , m_HostName(host)
    , m_Port(port)
    , m_ConnectData(connectData)
{
    if (!s_EnetInitialized) {
        if (enet_initialize() != 0) {
            qCritical() << "[ENet] enet_initialize() failed";
            return;
        }
        s_EnetInitialized = true;
    }

    m_ServiceTimer = new QTimer(this);
    m_ServiceTimer->setInterval(m_ServiceIntervalMs);
    connect(m_ServiceTimer, &QTimer::timeout, this, &EnetControlStream::service);
}

EnetControlStream::~EnetControlStream()
{
    stop();
}

bool EnetControlStream::start()
{
    // Create ENet host (AF_INET = IPv4, 1 peer, 48 channels)
    m_Host = enet_host_create(AF_INET, nullptr, 1, CTRL_CHANNEL_COUNT, 0, 0);
    if (!m_Host) {
        emit connectionFailed("enet_host_create failed");
        return false;
    }

    // Resolve remote address
    ENetAddress addr;
    enet_address_set_host(&addr, m_HostName.toUtf8().constData());
    enet_address_set_port(&addr, m_Port);

    qDebug() << "[ENet] Connecting to" << m_HostName << ":" << m_Port
             << "connectData=" << m_ConnectData;

    // Connect
    m_Peer = enet_host_connect(m_Host, &addr, CTRL_CHANNEL_COUNT, m_ConnectData);
    if (!m_Peer) {
        enet_host_destroy(m_Host);
        m_Host = nullptr;
        emit connectionFailed("enet_host_connect failed");
        return false;
    }

    // Wait for the CONNECT event
    if (!waitForConnect(10000)) {
        emit connectionFailed("ENet connect timeout (10s)");
        return false;
    }

    // Flush the connect ACK
    enet_host_flush(m_Host);

    // Limit backoff, 10s timeout (matching moonlight-qt)
    enet_peer_timeout(m_Peer, 2, 10000, 10000);

    // START_A/B — fire-and-forget (Sunshine does not send app-level replies for ENet).
    // This matches moonlight-qt's sendMessageAndDiscardReply() for Gen5+.
    qDebug() << "[ENet] Connected, sending START_A...";

    QByteArray startA(2, '\0'); // {0, 0}
    if (!sendMessage(startA, PKT_TYPE_START_A, CTRL_CHANNEL_GENERIC)) {
        emit connectionFailed("START_A send failed");
        return false;
    }

    qDebug() << "[ENet] START_A sent, sending START_B...";

    QByteArray startB(1, '\0'); // {0}
    if (!sendMessage(startB, PKT_TYPE_START_B, CTRL_CHANNEL_GENERIC)) {
        emit connectionFailed("START_B send failed");
        return false;
    }

    qDebug() << "[ENet] START_B sent — control channel ready";

    // Start periodic service timer
    m_ServiceTimer->start();
    m_Connected = true;
    emit connected();
    return true;
}

void EnetControlStream::stop()
{
    m_ServiceTimer->stop();

    if (m_Peer) {
        enet_peer_disconnect_now(m_Peer, 0);
        m_Peer = nullptr;
    }

    if (m_Host) {
        // Process the disconnect
        ENetEvent event;
        enet_host_service(m_Host, &event, 0);
        enet_host_flush(m_Host);
        enet_host_destroy(m_Host);
        m_Host = nullptr;
    }

    m_Connected = false;
}

void EnetControlStream::sendInput(const QByteArray& inputPacket, quint8 channel)
{
    if (!m_Connected || !m_Peer) return;
    // Use RELIABLE to match moonlight-qt — input events must not be dropped
    sendMessage(inputPacket, PKT_TYPE_INPUT, channel, ENET_PACKET_FLAG_RELIABLE);
}

void EnetControlStream::service()
{
    if (!m_Host) return;

    ENetEvent event;
    while (enet_host_service(m_Host, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE: {
            enet_packet_destroy(event.packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT:
            qWarning() << "[ENet] Peer disconnected, data=" << event.data;
            m_Connected = false;
            m_ServiceTimer->stop();
            emit disconnected();
            break;

        default: break;
        }
    }
}

// --- Private helpers --------------------------------------------------------

bool EnetControlStream::sendMessage(const QByteArray& payload, uint16_t type, uint8_t channel,
                                    uint32_t flags)
{
    if (!m_Peer) return false;

    QByteArray data;
    data.resize(2 + payload.size());
    qToLittleEndian(type, data.data());
    if (!payload.isEmpty()) std::memcpy(data.data() + 2, payload.constData(), payload.size());

    ENetPacket* pkt = enet_packet_create(data.constData(), data.size(), flags);
    if (!pkt) {
        qWarning() << "[ENet] Failed to create packet type=0x" << Qt::hex << type;
        return false;
    }

    if (enet_peer_send(m_Peer, channel, pkt) < 0) {
        enet_packet_destroy(pkt);
        qWarning() << "[ENet] Failed to send packet type=0x" << Qt::hex << type;
        return false;
    }

    // Service to send + flush
    ENetEvent event;
    int svc = enet_host_service(m_Host, &event, 0);
    enet_host_flush(m_Host);

    // If Sunshine disconnected during send, handle it here
    if (svc > 0 && event.type == ENET_EVENT_TYPE_DISCONNECT) {
        qWarning() << "[ENet] Peer disconnected during send, data=" << event.data;
        m_Connected = false;
        m_ServiceTimer->stop();
        emit disconnected();
        return false;
    }

    return true;
}

bool EnetControlStream::waitForConnect(int timeoutMs)
{
    ENetEvent event;
    int elapsed = 0;
    const int slice = 50; // ms per poll

    while (elapsed < timeoutMs) {
        int ret = enet_host_service(m_Host, &event, slice);
        if (ret > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
            return true;
        }
        elapsed += slice;
    }

    return false;
}

bool EnetControlStream::sendAndWaitReply(const QByteArray& payload, uint16_t type, uint8_t channel,
                                         int timeoutMs)
{
    // Build packet: type (LE16) + payload
    QByteArray data;
    data.resize(2 + payload.size());
    qToLittleEndian(type, data.data());
    if (!payload.isEmpty()) std::memcpy(data.data() + 2, payload.constData(), payload.size());

    ENetPacket* pkt = enet_packet_create(data.constData(), data.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(m_Peer, channel, pkt);
    enet_host_flush(m_Host);

    // Wait for reply
    ENetEvent event;
    int elapsed = 0;
    const int slice = 50;

    while (elapsed < timeoutMs) {
        int ret = enet_host_service(m_Host, &event, slice);
        elapsed += slice;

        if (ret <= 0) continue;

        switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE: {
            uint16_t replyType = 0;
            if (event.packet->dataLength >= 2)
                replyType = qFromLittleEndian<uint16_t>(event.packet->data);

            if (event.channelID == channel && replyType == type) {
                enet_packet_destroy(event.packet);
                return true;
            }
            enet_packet_destroy(event.packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            qWarning() << "[ENet] DISCONNECT at" << elapsed << "ms"
                       << "data=" << event.data;
            return false;
        }
        default: break;
        }
    }

    qWarning() << "[ENet] No reply for type 0x" << Qt::hex << type << "within" << elapsed << "ms";
    return false;
}
