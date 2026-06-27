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

    // ── Stream aspect ratio ────────────────────────────────────────────────────
    //
    // Aspect ratio used to derive width from a fixed height. Stored as JSON
    // string "stream_aspect", default "auto" (derive from the host's reported
    // screen format). Manual overrides: "16:9", "21:9", "32:9".

    QString streamAspect() const;
    void setStreamAspect(const QString& aspect);

    // ── Stream frame rate ─────────────────────────────────────────────────────
    //
    // Target FPS. Stored as JSON int "stream_fps", default 60.
    // Common values: 30, 60, 75, 90, 120, 144, 165, 240.

    int streamFps() const;
    void setStreamFps(int fps);

    // ── HDR ───────────────────────────────────────────────────────────────────
    //
    // Whether HDR streaming is requested. Stored as JSON bool "hdr_enabled",
    // default false. Not yet applied to the video pipeline — UI preference only.

    bool hdrEnabled() const;
    void setHdrEnabled(bool enabled);

    // ── Mute host audio ───────────────────────────────────────────────────────
    //
    // Whether the host PC speakers are muted while streaming. Stored as JSON bool
    // "mute_host_audio", default true (GameStream localAudioPlayMode=0).

    bool muteHostAudio() const;
    void setMuteHostAudio(bool enabled);

    // ── Chroma 4:4:4 ───────────────────────────────────────────────────────────
    //
    // Whether full-resolution YUV 4:4:4 chroma is requested (vs the default
    // 4:2:0). Stored as JSON bool "chroma_444_enabled", default false. Requires
    // significantly higher bandwidth and a browser able to decode the 4:4:4
    // codec profile.

    bool chroma444Enabled() const;
    void setChroma444Enabled(bool enabled);

    // ── Video enhancement (WebGPU upscale/sharpen) ─────────────────────────────
    //
    // Client-side feature (the browser renders via WebGPU). Persisted here so the
    // setting syncs across browsers. Stored as JSON string "video_enhancement"
    // ("off" default | "on") and "video_enhancement_algo" ("auto" | "sgsr" | "fsr1" | "force2d").
    // When "on", the launch transport negotiation skips webrtc-media (the canvas
    // pipeline is required).

    QString videoEnhancement() const;
    void setVideoEnhancement(const QString& value);

    QString videoEnhancementAlgo() const;
    void setVideoEnhancementAlgo(const QString& algo);

    // ── Audio time-stretch (WSOLA) ─────────────────────────────────────────────
    //
    // Pitch-preserving accelerate/expand in the AudioWorklet. File-only setting
    // (no UI): stored as JSON bool "audio_time_stretch", DEFAULT true. When true,
    // smooths clock drift and jitter without added latency at a small CPU cost.
    // Documented in the README (the JSON holds no comment keys).

    bool audioTimeStretch() const;

    // Seed documented file-only default keys into settings.json if absent, so they
    // are discoverable/editable in the file. Idempotent.
    void seedDocumentedDefaults();

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

    /// Whether Internet Access via PowerDNS is enabled.
    /// Default: false.
    bool internetAccessEnabled() const;
    void setInternetAccessEnabled(bool enabled);

    /// Unique 8-char hex identifier for the PowerDNS subdomain.
    QString uniqueId() const;
    void setUniqueId(const QString& id);

    /// Last subdomain actually registered in DNS (the one whose _owner TXT we
    /// hold). Used to release the previous subdomain when unique_id changes,
    /// so an owner never holds more than one live subdomain.
    QString registeredUid() const;
    void setRegisteredUid(const QString& id);

    /// Full domain name: "{uniqueId}.{MW_DOMAIN}" or the stored FQDN.
    ///
    /// Logic:
    ///   1. Read "domain" from settings.json.
    ///   2. If it is a valid FQDN (contains at least one dot), return it as-is.
    ///   3. Otherwise construct "{uniqueId}.{MW_DOMAIN}" where MW_DOMAIN is
    ///      read from the MW_DOMAIN env var (fallback "moonlightweb.top").
    ///      If uniqueId is empty, returns just the base domain.
    QString domain() const;
    void setDomain(const QString& domain);

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

    // ── HMAC key for session tokens ──────────────────────────────────────────

    /// Persisted HMAC signing key. Generated once, reused across restarts.
    /// Stored as Base64 string. Empty if never set.
    QByteArray hmacKey() const;
    void setHmacKey(const QByteArray& key);

    /// Pending domain registration flag (set when no internet at install time).
    /// On each startup, if true, retry registration every 30s until success.
    bool pendingRegistration() const;
    void setPendingRegistration(bool pending);

    /// Certificate PEM source: env var name (e.g. "MW_CERT_PEM") or file path.
    /// Defaults to "MW_CERT_PEM" (reads the PEM from environment variable).
    /// After ACME issuance, set to a file path (e.g. letsencrypt/fullchain.pem).
    QString certPem() const;
    void setCertPem(const QString& value);

    /// Private key source: env var name (e.g. "MW_CERT_KEY") or file path.
    /// Defaults to "MW_CERT_KEY" (reads the PEM from environment variable).
    /// After ACME issuance, set to a file path (e.g. letsencrypt/domain_key.pem).
    QString certKey() const;
    void setCertKey(const QString& value);

    // ── Certificate Authentication (alternative to PIN) ──────────────────────
    //
    // Generates and persists a long random token used as a downloadable
    // certificate file. Remote users can upload this file instead of entering
    // a PIN. Generated once at first startup, never changes afterwards.

    /// The certificate token (64+ random bytes, Base64-encoded). Empty if never generated.
    QString certificateToken() const;

    /// Persist the certificate token.
    void setCertificateToken(const QString& token);

    /// Whether certificate authentication is enabled (alternative to PIN).
    /// Default: false.
    bool certAuthEnabled() const;

    /// Enable or disable certificate authentication.
    void setCertAuthEnabled(bool enabled);

    // ── DNS subdomain ownership ─────────────────────────────────────────────
    //
    // Per-instance random token written to a _owner.<uid> TXT record. Before
    // replacing its A record, an instance verifies this matches (or claims it
    // if absent), so two instances cannot clobber each other's subdomain.

    QString ownerToken() const;
    void setOwnerToken(const QString& token);

    // ── Low-level access (for other one-off settings) ───────────────────────

    /// Read the entire settings JSON object.
    QJsonObject readAll() const;

    /// Write back a full JSON object.
    void writeAll(const QJsonObject& obj);

    /// Full path to settings.json.
    QString filePath() const { return m_FilePath; }

    /// Validate that a string looks like a real FQDN.
    /// Criteria: non-empty, contains at least one dot, dot is not first/last,
    /// only alphanumeric chars, dots, and hyphens.
    static bool isValidFqdn(const QString& domain);

    QString m_FilePath;
};
