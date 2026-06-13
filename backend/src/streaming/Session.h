#pragma once

#include <QObject>
#include <QNetworkReply>
#include <QUrl>

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
    StreamSession(NvComputer* host, int appId,
                  NvHTTP* http, ResponseCallback respond,
                  quint16 wsPort = 48001,
                  const QString& serverHost = "localhost",
                  VideoCodec videoCodec = VideoCodec::Auto,
                  bool gamingMode = true,
                  bool upnpEnabled = true,
                  const QString& transport = "webrtc",
                  const QString& stunServer = "stun:stun.l.google.com:19302",
                  int streamHeight = 1080,
                  int streamFps = 60,
                  int streamBitrateKbps = 20000,
                  bool hdr = false,
                  QObject* parent = nullptr);
    ~StreamSession();

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

    /// Enable auto-mode behavior: WS fallback is disabled so that ICE timeout
    /// emits sessionEnded() instead of starting the internal WS fallback.
    /// This allows the auto fallback chain in main.cpp to try the next transport.
    void setAutoMode(bool autoMode) { m_AutoMode = autoMode; }

    /// Mark the video codec as overridden for transport compatibility.
    /// Example: MediaTrack forced but user selected HEVC → codec forced to H.264.
    /// When set, onShimConnectionStarted() includes codecOverridden + originalCodec
    /// in the JSON response so the frontend can adjust decoder expectations.
    void setCodecOverridden(bool overridden, VideoCodec originalCodec) {
        m_CodecOverridden = overridden;
        m_OriginalCodec = originalCodec;
    }

    void setExplicitWsUrl(const QString& url) { m_ExplicitWsUrl = url; }

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
    /// Enable ICE-TCP candidates (true for *-tcp modes, false for *-udp modes).
    bool m_EnableIceTcp = false;
    /// True when session is part of the auto fallback chain (main.cpp auto mode).
    /// Disables internal WS fallback so ICE timeout → sessionEnded → tryNext().
    bool m_AutoMode = false;
    /// True when the video codec was overridden for transport compatibility.
    bool m_CodecOverridden = false;
    /// The original codec selected by the user, before override.
    VideoCodec m_OriginalCodec = VideoCodec::Auto;
    QString m_StunServer = "stun:stun.l.google.com:19302";
    QNetworkReply* m_LaunchReply = nullptr;
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
