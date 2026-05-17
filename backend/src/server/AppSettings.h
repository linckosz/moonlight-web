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
    // When enabled (default), mouse pointer is locked for seamless camera
    // control. When disabled, absolute mouse position tracking is used instead.
    // Stored as JSON bool "gaming_mode", default true.

    bool gamingMode() const;
    void setGamingMode(bool enabled);

    // ── nport tunnel ──────────────────────────────────────────────────────────

    /// Persistent subdomain for remote tunnel access (e.g. "moonlightweb-a1b2").
    QString nportSubdomain() const;
    void setNportSubdomain(const QString& subdomain);

    // ── UPnP NAT traversal ────────────────────────────────────────────────────

    /// Whether UPnP port mapping is enabled for WebRTC NAT traversal.
    /// Default: true (recommended — enables direct P2P connections from outside LAN).
    bool upnpEnabled() const;
    void setUpnpEnabled(bool enabled);

    // ── Transport preference ──────────────────────────────────────────────────
    //
    // Values stored in JSON: "webrtc" (default), "wss" (legacy StreamRelay)
    //
    // "webrtc": Uses WebRTC DataChannels via libdatachannel (default).
    // "wss":    Uses the legacy WebSocket StreamRelay (for testing/diagnostics).

    QString transport() const;
    void setTransport(const QString& transport);

    // ── Low-level access (for other one-off settings) ───────────────────────

    /// Read the entire settings JSON object.
    QJsonObject readAll() const;

    /// Write back a full JSON object.
    void writeAll(const QJsonObject& obj);

    /// Full path to settings.json.
    QString filePath() const { return m_FilePath; }

private:
    QString m_FilePath;
};
