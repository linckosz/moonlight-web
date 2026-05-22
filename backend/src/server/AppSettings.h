#pragma once

#include <QString>
#include <QJsonObject>

#include "../streaming/StreamConfig.h"

// Persistent settings stored in QStandardPaths::AppDataLocation/settings.json.
//
// This class provides a type-safe interface over the JSON settings file
// and handles read/write synchronisation (single-threaded, synchronous I/O).
class AppSettings
{
public:
    explicit AppSettings();

    // ── HTTP port ────────────────────────────────────────────────────────────

    quint16 httpPort(quint16 fallback = 80) const;
    void setHttpPort(quint16 port);

    // ── HTTPS port ──────────────────────────────────────────────────────────

    quint16 httpsPort(quint16 fallback = 443) const;
    void setHttpsPort(quint16 port);

    // ── Video codec preference ──────────────────────────────────────────────
    //
    // Values stored in JSON: "auto", "h264", "hevc", "av1"
    // Default: VideoCodec::Auto

    VideoCodec videoCodec() const;
    void setVideoCodec(VideoCodec codec);
    static QString videoCodecToString(VideoCodec codec);
    static VideoCodec videoCodecFromString(const QString& str);

    // ── Gaming mode ───────────────────────────────────────────────────────────
    //
    // When enabled, mouse pointer is locked for seamless camera control.
    // When disabled (default), absolute mouse position tracking is used instead.
    // Stored as JSON bool "gaming_mode", default false.

    bool gamingMode() const;
    void setGamingMode(bool enabled);

    // ── Performance stats overlay ──────────────────────────────────────────────
    //
    // When enabled, shows FPS, bitrate, frame loss, and latency stats during
    // streaming. Stored as JSON bool "show_performance_stats", default false.

    bool showPerformanceStats() const;
    void setShowPerformanceStats(bool enabled);

    // ── Stream bitrate ────────────────────────────────────────────────────────
    //
    // Target bitrate in kbps. Stored as JSON int "stream_bitrate", default 20000.
    // Range: 5000 – 150000 kbps (5 – 150 Mbps).

    int streamBitrate() const;
    void setStreamBitrate(int kbps);

    // ── Stream height (resolution) ────────────────────────────────────────────
    //
    // Target vertical resolution in pixels. Stored as JSON int "stream_height",
    // default 1080. A value of 0 means "Native Host" (use host's native resolution
    // from the EDID or display info). Width is auto-calculated from aspect ratio.
    // Supported: 720, 1080, 1440, 2160, 0 (Native).

    int streamHeight() const;
    void setStreamHeight(int height);

    // ── Stream frame rate ─────────────────────────────────────────────────────
    //
    // Target FPS. Stored as JSON int "stream_fps", default 60.
    // Common values: 30, 60, 75, 90, 120, 144, 165, 240.

    int streamFps() const;
    void setStreamFps(int fps);

    // ── UPnP NAT traversal ────────────────────────────────────────────────────

    /// Whether UPnP port mapping is enabled for WebRTC NAT traversal.
    /// Default: true (recommended — enables direct P2P connections from outside LAN).
    bool upnpEnabled() const;
    void setUpnpEnabled(bool enabled);

    // ── STUN server ─────────────────────────────────────────────────────────────
    //
    // STUN server URL for WebRTC ICE connectivity. Used by both the backend's
    // libdatachannel PeerConnection and forwarded to the browser for its
    // RTCPeerConnection. Default: "stun:stun.l.google.com:19302"

    QString stunServer() const;
    void setStunServer(const QString& url);

    // ── Transport preference ──────────────────────────────────────────────────
    //
    // Values stored in JSON: "webrtc" (default), "wss" (legacy StreamRelay)
    //
    // "webrtc": Uses WebRTC DataChannels via libdatachannel (default).
    // "wss":    Uses the legacy WebSocket StreamRelay (for testing/diagnostics).

    QString transport() const;
    void setTransport(const QString& transport);

    // ── Internet Access ───────────────────────────────────────────────────────

    /// Whether Internet Access via deSEC is enabled.
    /// Default: false.
    bool internetAccessEnabled() const;
    void setInternetAccessEnabled(bool enabled);

    /// Unique 8-char hex identifier for the deSEC domain.
    QString uniqueId() const;
    void setUniqueId(const QString& id);

    /// Full domain name: "moonlightweb-{uniqueId}.dedyn.io"
    QString domain() const;
    void setDomain(const QString& domain);

    /// deSEC API token. "auto" or empty means the compiled-in default token
    /// is used. The compiled token is never logged.
    QString desecToken() const;
    void setDesecToken(const QString& token);

    /// Resolved public IP (from STUN or manual entry).
    QString publicIp() const;
    void setPublicIp(const QString& ip);

    /// Whether to auto-detect public IP via STUN.
    /// Default: true.
    bool autoIpDetection() const;
    void setAutoIpDetection(bool enabled);

    /// Transport mode for Internet Access connections.
    /// "auto" | "webrtc-media-udp" | "webrtc-dc-udp" | "webrtc-media-tcp" |
    /// "webrtc-dc-tcp" | "wss"
    /// Default: "auto".
    QString transportMode() const;
    void setTransportMode(const QString& mode);

    /// Pending domain registration flag (set when no internet at install time).
    /// On each startup, if true, retry registration every 30s until success.
    bool pendingRegistration() const;
    void setPendingRegistration(bool pending);

    /// Path to the Let's Encrypt TLS certificate file.
    QString certPath() const;
    void setCertPath(const QString& path);

    /// Certificate expiry timestamp (ISO 8601).
    QString certExpiry() const;
    void setCertExpiry(const QString& expiry);

    // ── Low-level access (for other one-off settings) ───────────────────────

    /// Read the entire settings JSON object.
    QJsonObject readAll() const;

    /// Write back a full JSON object.
    void writeAll(const QJsonObject& obj);

    /// Full path to settings.json.
    QString filePath() const { return m_FilePath; }

    /// Compiled-in default deSEC token (for "auto" mode).
    /// Never log this value.
    static QString defaultDesecToken();

private:
    /// Encrypt a token via Windows DPAPI (machine-level, current user).
    static QString encryptToken(const QString& plain);
    /// Decrypt a DPAPI-encrypted token.
    static QString decryptToken(const QString& encoded);

    QString m_FilePath;
};
