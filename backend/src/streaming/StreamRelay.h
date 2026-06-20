/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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
#include <QWebSocketServer>
#include <QWebSocket>
#include <QSslConfiguration>
#include <QElapsedTimer>
#include <QTimer>

class MoonlightShim;

class StreamRelay : public QObject
{
    Q_OBJECT

public:
    StreamRelay(MoonlightShim* shim,
                quint16 wsPort,
                const QSslConfiguration& sslConfig = {},
                QObject* parent = nullptr);
    ~StreamRelay();

    bool start();
    void stop();

    /// Notify the browser (over the WS) that its session was taken over by
    /// another device, just before stop() closes the socket. Best-effort.
    void notifyClientTakenOver();

    void setServerHost(const QString& host) { m_ServerHost = host; }
    void setHttpsPort(quint16 port) { m_HttpsPort = port; }
    quint16 wsPort() const { return m_WsPort; }
    QString wsUrl() const;

    /// A WS client is attached (wss connects immediately, no ICE) — an active
    /// StreamRelay therefore means a live stream, used for take-over detection.
    bool isClientConnected() const { return m_WsClient != nullptr; }

    /// Access the MoonlightShim for explicit stopConnection() before cleanup.
    MoonlightShim* moonlightShim() const { return m_Shim; }

signals:
    void sessionEnded();
    void clientConnected();
    void clientDisconnected();

private slots:
    void onVideoFrame(const QByteArray& data, int frameType, int frameNumber);
    void onAudioSample(const QByteArray& data);
    void onNewWsConnection();
    void onWsTextMessage(const QString& message);
    void onWsDisconnected();

    void onShimConnectionStarted();
    void onShimConnectionFailed(const QString& error);
    void onShimConnectionTerminated(int errorCode);

private:
    /// Enable/disable WSS video fragmentation (same format as DataChannelRelay).
    /// When true, video frames are split into chunks with a 17-byte header
    /// matching the DataChannelRelay protocol, sent over the WebSocket with
    /// a 1-byte channel prefix.  This allows testing whether the green image
    /// bug is in SCTP transport or in the fragmentation/reassembly itself.
    /// Toggle this and the frontend WSS_FRAGMENTED flag together for comparison.
    void setVideoFragmentationEnabled(bool enabled) { m_UseVideoFragmentation = enabled; }
    bool isVideoFragmentationEnabled() const { return m_UseVideoFragmentation; }

    // Fragmentation constants — same values as DataChannelRelay
    static constexpr int kFragHeaderSize = 17;
    static constexpr int kMaxPayloadSize = 14000;

    /// Send a video frame using the DataChannelRelay fragmentation protocol
    /// but over the WebSocket.  Header: [frame_id:4][chunk_index:2][total_chunks:2]
    /// [is_keyframe:1][payload_size:4][backend_ts:4], then the chunk payload.
    /// Each WS binary message: [channel:1][frag_header:17][chunk_payload...].
    void sendVideoFragmentedWss(const QByteArray& data, bool isKeyframe);

    /// Send periodic stats (hostRtt + steady-clock reference) to the browser
    /// over the WS as text JSON, so the latency overlay works in WSS mode.
    void sendStats();

    MoonlightShim* m_Shim;
    QTimer* m_StatsTimer = nullptr;    // Periodic stats to the browser (500ms)
    QElapsedTimer m_IdrCooldownTimer;  // Throttle browser IDR requests (300ms)
    QWebSocketServer* m_WsServer = nullptr;
    QWebSocket* m_WsClient = nullptr;
    quint16 m_WsPort = 48001;
    QString m_ServerHost = "localhost";
    quint16 m_HttpsPort = 443;
    bool m_Running = false;
    bool m_Stopping = false;
    bool m_StreamStarted = false;
    bool m_UseVideoFragmentation = false;  // WSS fragmentation OFF — full frames sent as single WS messages
    uint32_t m_FrameId = 0;              // Monotonic frame ID for fragmentation
    QList<QByteArray> m_PendingVideoFrames;
    QList<QByteArray> m_PendingAudioFrames;
    int m_FrameCount = 0;
};
