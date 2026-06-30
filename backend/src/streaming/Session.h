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
#include <QNetworkReply>
#include <QUrl>
#include <QStringList>
#include <QSet>

#include "StreamConfig.h"
#include "../common/Types.h"

class NvHTTP;
class NvComputer;
class DataChannelRelay;
class MediaTrackRelay;
class SignalingServer;
class StreamRelay;
class MoonlightShim;

class StreamSession : public QObject
{
    Q_OBJECT

public:
    StreamSession(NvComputer* host, int appId, NvHTTP* http, ResponseCallback respond,
                  quint16 wsPort = 48001, const QString& serverHost = "localhost",
                  VideoCodec videoCodec = VideoCodec::Auto, bool gamingMode = true,
                  bool upnpEnabled = true, const QString& transport = "webrtc",
                  const QString& stunServer = "stun:stun.l.google.com:19302",
                  int streamHeight = 1080,
                  int streamWidth = 0, // 0 = derive from height (16:9); >0 = explicit (ultrawide)
                  int streamFps = 60, int streamBitrateKbps = 20000, bool yuv444 = false,
                  bool hdrEnabled = false, QObject* parent = nullptr);
    ~StreamSession() override;

    void start();
    void quit();

    /// Override the signaling WS URL returned to the browser.
    /// When a public tunnel is active, this is the tunnel's WSS endpoint.
    /// Set the full transport mode string (e.g. "webrtc-media-udp", "webrtc-dc-tcp").
    /// Used for reporting back to the browser in the /start response.
    void setTransportMode(const QString& mode) { m_TransportMode = mode; }

    /// Enable/disable ICE-TCP candidates for WebRTC transport.
    /// Set before start() to control whether TCP fallback candidates are generated.
    void setEnableIceTcp(bool enable) { m_EnableIceTcp = enable; }

    /// Request lower-bandwidth audio (10ms Opus frames) for mobile clients.
    void setLowAudio(bool enable) { m_LowAudio = enable; }

    /// Mute the host PC speakers while streaming (GameStream localAudioPlayMode).
    /// true (default) → host muted; false → audio also plays on the host.
    void setMuteHostAudio(bool mute) { m_Config.muteHostAudio = mute; }

    /// Enable auto-mode behavior: WS fallback is disabled so that ICE timeout
    /// emits sessionEnded() instead of starting the internal WS fallback.
    /// This allows the auto fallback chain in main.cpp to try the next transport.
    void setAutoMode(bool autoMode) { m_AutoMode = autoMode; }

    /// Mark the video codec as overridden for transport compatibility.
    /// Example: MediaTrack forced but user selected HEVC → codec forced to H.264.
    /// When set, onShimConnectionStarted() includes codecOverridden + originalCodec
    /// in the JSON response so the frontend can adjust decoder expectations.
    void setCodecOverridden(bool overridden, VideoCodec originalCodec)
    {
        m_CodecOverridden = overridden;
        m_OriginalCodec = originalCodec;
    }

    void setExplicitWsUrl(const QString& url) { m_ExplicitWsUrl = url; }

    /// Set the ordered transport fallback chain and the index of the attempt
    /// this session represents. Echoed back to the browser so the frontend can
    /// walk the chain (relaunch with the next index) when a transport fails.
    void setTransportChain(const QStringList& chain, int index)
    {
        m_TransportChain = chain;
        m_TransportIndex = index;
    }

    /// Per-browser Sunshine unique ID (from the browser's localStorage).
    /// Isolates session management on Sunshine so one browser doesn't take
    /// over / cancel another's session. Empty → shared IdentityManager id.
    void setClientUniqueId(const QString& id) { m_ClientUniqueId = id; }

    /// Set the actual HTTPS port used by HttpServer (may differ from 443
    /// due to port fallback). Used to construct the correct wsUrl() for
    /// the browser when sharing the unified port.
    void setHttpsPort(quint16 port) { m_HttpsPort = port; }

    /// Set the port for the legacy WSS StreamRelay (separate from signaling WS port).
    void setStreamRelayPort(quint16 port) { m_StreamRelayPort = port; }

signals:
    void relayCreated(DataChannelRelay* relay);
    void mediaTrackRelayCreated(MediaTrackRelay* relay);
    void streamRelayCreated(StreamRelay* relay);
    void sessionStarted();
    void sessionFailed(const QString& error);

private slots:
    void onLaunchReplyFinished();
    void onShimConnectionStarted();
    void onShimConnectionFailed(const QString& error);

private:
    void doLaunchApp(const QByteArray& clientCert, const QByteArray& clientKey);
    void doResumeApp(const QByteArray& clientCert, const QByteArray& clientKey);

    /// Effective Sunshine uniqueid (m_ClientUniqueId or the shared id).
    QString effectiveUniqueId() const;

    /// Registry of uniqueids with a session we launched and haven't quit.
    /// Main-thread only (start/quit/reply slots all run on the Session thread).
    /// Lets a reload reconnect to its own session via /resume instead of /launch.
    static QSet<QString> s_ActiveUniqueIds;

    NvComputer* m_Host;
    int m_AppId;
    NvHTTP* m_Http;
    ResponseCallback m_Respond;
    quint16 m_WsPort = 48001;
    QString m_ServerHost;

    StreamConfig m_Config;
    bool m_GamingMode = true;

    // Streaming resolution, FPS and bitrate (read from AppSettings)
    int m_StreamWidth = 1920;
    int m_StreamHeight = 1080;
    int m_StreamFps = 60;
    int m_StreamBitrateKbps = 20000;

    bool m_UpnpEnabled = true;
    QString m_Transport = "webrtc";
    /// Full transport mode string (e.g. "webrtc-media-udp", "webrtc-dc-tcp", "wss").
    /// Echoed back to the browser in the /start response for display/debug.
    QString m_TransportMode;
    /// Ordered transport fallback chain + this attempt's index (echoed back so
    /// the frontend can relaunch with the next transport on connection failure).
    QStringList m_TransportChain;
    int m_TransportIndex = 0;
    /// Enable ICE-TCP candidates (true for *-tcp modes, false for *-udp modes).
    bool m_EnableIceTcp = false;
    bool m_LowAudio = false;
    /// True when session is part of the auto fallback chain (main.cpp auto mode).
    /// Disables internal WS fallback so ICE timeout → sessionEnded → tryNext().
    bool m_AutoMode = false;
    /// True when the video codec was overridden for transport compatibility.
    bool m_CodecOverridden = false;
    /// The original codec selected by the user, before override.
    VideoCodec m_OriginalCodec = VideoCodec::Auto;
    QString m_StunServer = "stun:stun.l.google.com:19302";
    /// Per-browser uniqueid for Sunshine launch/cancel; empty → shared id.
    QString m_ClientUniqueId;
    QNetworkReply* m_LaunchReply = nullptr;
    /// Track which start paths we've tried, so launch↔resume fallback (used when
    /// the registry hint is stale) terminates instead of looping.
    bool m_LaunchAttempted = false;
    bool m_ResumeAttempted = false;
    QString m_SessionUrl;

    /// If non-empty, overrides SignalingServer::wsUrl() in the /start response.
    QString m_ExplicitWsUrl;

    /// The HTTPS port HttpServer is actually listening on (may be != 443).
    quint16 m_HttpsPort = 443;

    /// Port for legacy WSS StreamRelay (separate from m_WsPort used for signaling).
    quint16 m_StreamRelayPort = 48002;

    /// Negotiated video format from drSetup (0=unknown, 0x0001=H.264, 0x0100=HEVC, 0x0200=AV1).
    /// Written by drSetup on the worker thread, read on the main thread during
    /// onShimConnectionStarted(). The atomic in MoonlightShim guarantees up-to-date reads.
    int m_NegotiatedVideoFormat = 0;

    MoonlightShim* m_Shim = nullptr;
    DataChannelRelay* m_Relay = nullptr;
    MediaTrackRelay* m_MediaTrackRelay = nullptr;
    SignalingServer* m_Signaling = nullptr;
    StreamRelay* m_StreamRelay = nullptr;
};
