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
#include <QTimer>
#include <QString>
#include <QByteArray>
#include <cstdint>

// ENet C API
extern "C" {
#include "enet/enet.h"
}

// Qt-friendly wrapper around the ENet reliable UDP protocol.
// Connects to Sunshine on the control port (e.g. 47999),
// performs the START_A/START_B handshake, then sends input
// packets on keyboard (ch 0x02) and mouse (ch 0x03) channels.
class EnetControlStream : public QObject
{
    Q_OBJECT

public:
    EnetControlStream(const QString& host, quint16 port, uint32_t connectData,
                      QObject* parent = nullptr);
    ~EnetControlStream() override;

    bool start();
    void stop();
    bool isConnected() const { return m_Connected; }

    // Raw GameStream input packet (NV_INPUT_HEADER + payload).
    // The NVCTL header (type 0x0206) is prepended automatically.
    void sendInput(const QByteArray& inputPacket, quint8 channel);

signals:
    void connected();
    void connectionFailed(const QString& error);
    void disconnected();

private slots:
    void service();

private:
    bool waitForConnect(int timeoutMs);
    bool sendMessage(const QByteArray& payload, uint16_t type, uint8_t channel,
                     uint32_t flags = ENET_PACKET_FLAG_RELIABLE);
    bool sendAndWaitReply(const QByteArray& payload, uint16_t type, uint8_t channel, int timeoutMs);

    static bool s_EnetInitialized;

    ENetHost* m_Host = nullptr;
    ENetPeer* m_Peer = nullptr;
    QString m_HostName;
    quint16 m_Port;
    uint32_t m_ConnectData;
    QTimer* m_ServiceTimer;
    bool m_Connected = false;
    int m_ServiceIntervalMs = 20;
};
