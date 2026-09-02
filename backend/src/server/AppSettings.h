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

    // ── Click-to-photon latency flag (debug builds on Windows only) ───────────
    //
    // When enabled, the host shows a French flag at the top of its screen for
    // 100 ms on every injected click, and the browser measures the time from
    // its click to the flag's appearance in the decoded picture — see
    // LatencyFlag.h and frontend/js/stream/LatencyProbe.js. Stored as JSON bool
    // "latency_flag_enabled", default false. Ignored outside supported builds.

    bool latencyFlagEnabled() const;
    void setLatencyFlagEnabled(bool enabled);

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
    // string "stream_aspect", default "auto" (the browser measures the host's
    // real format at stream start and relaunches with it). Manual overrides:
    // "16:9", "16:10", "21:9", "4:3", "3:2", "32:9", "5:4".

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
    // ("on" default | "off") and "video_enhancement_algo" ("auto" default | "sgsr" | "fsr1" |
    // "force2d"). When "on", the launch transport negotiation skips webrtc-media (the canvas
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

    /// Host each streaming session in a --stream-worker subprocess (enables two
    /// concurrent sessions). File-only kill switch, default true.
    bool streamWorkerEnabled() const;

    // ── Update relay ──────────────────────────────────────────────────────────
    //
    // Whether the periodic update check goes through the project's relay
    // (https://updates.{MW_DOMAIN}) rather than straight to api.github.com.
    /// Whether a session's city and country may be resolved for the admin
    /// sessions list. DEFAULT false, and deliberately so: the address looked up
    /// belongs to the *visitor*, not to the owner who would be switching this
    /// on, and it goes to a third party neither of them has heard of. Stored as
    /// JSON bool "session_location_enabled", file-only.
    bool sessionLocationEnabled() const;
    void setSessionLocationEnabled(bool enabled);

    // Stored as JSON bool "update_relay_enabled", DEFAULT true, file-only.
    //
    // The relay returns the same GitHub release payload from a shared cache, and
    // records version/OS/arch in aggregate so migrations can be planned on real
    // numbers instead of download counts. Set it to false (or set MW_NO_TELEMETRY
    // in the environment) to check GitHub directly and report nothing; updates
    // keep working either way. Self-built binaries never use the relay at all —
    // it needs the MW_PDNS_TOKEN only official builds carry. Documented in the
    // README (the JSON holds no comment keys).

    bool updateRelayEnabled() const;

    // ── Session metrics ────────────────────────────────────────────────────────
    //
    // Whether finished streaming sessions are counted in aggregate by the
    // project (https://metrics.{MW_DOMAIN}). Stored as JSON bool
    // "session_metrics_enabled", DEFAULT true, file-only.
    //
    // What travels is the SHAPE of a session — resolution, frame rate, codec,
    // HDR, a bitrate band, backend, transport, LAN-or-internet, device class,
    // duration — and never a host name, an account or an application. Set it to
    // false (or set MW_NO_TELEMETRY in the environment) to report nothing;
    // streaming is identical either way, and self-built binaries never report
    // at all. See backend/src/network/SessionMetrics.h and the README.

    bool sessionMetricsEnabled() const;

    // ── Statistics consent (GDPR) ──────────────────────────────────────────────
    //
    // Neither census above reports ANYTHING until the person running this
    // machine has been asked and has said yes. The two booleans above are
    // permanent kill switches for someone editing the file; this is the answer
    // to the question the app puts on screen at first launch.
    //
    // Stored as JSON object "metrics_consent", mirroring "internet_consent":
    // the exact wording that was displayed, when it was answered, through which
    // entry point ("banner" | "admin"), the decision, and the version of the
    // wording — so a later wording that describes MORE than this one can ask
    // again instead of inheriting an answer given to a different question.
    //
    // Absent = never asked = nothing is reported. Refusing changes nothing else
    // about the application: it is not a cookie wall, it does not touch the
    // session cookie the app needs to work, and it is unrelated to the separate
    // Internet Access consent.

    /// Version of the wording currently shown. Bump only when the new text
    /// covers something the old one did not — an answer to the old question
    /// does not carry over.
    static constexpr int kMetricsConsentVersion = 1;

    /// {"decision" ("granted"|"denied"), "message", "at" (ISO-8601 UTC),
    /// "source", "version"} — empty object if never asked.
    QJsonObject metricsConsent() const;
    void setMetricsConsent(bool granted, const QString& message, const QString& source);

    /// "" (never asked / stale wording), "granted" or "denied".
    QString metricsConsentDecision() const;

    /// The two questions every reporting path actually asks: consent given AND
    /// the file-only switch left alone. Keeping the rule here means a new
    /// caller cannot forget half of it.
    bool updateRelayAllowed() const;
    bool sessionMetricsAllowed() const;

    // Seed documented file-only default keys into settings.json if absent, so they
    // are discoverable/editable in the file. Idempotent.
    void seedDocumentedDefaults();

    // ── First-run setup ────────────────────────────────────────────────────────
    //
    // Whether the first-run setup wizard has completed. Stored as JSON bool
    // "setup_completed", default false. While false (and on a GUI, non-service
    // launch), the server opens the browser to /setup so the user can authorize
    // Internet Access, install and pair the local Sunshine. Windows sets this via
    // the Inno Setup installer's provisioning.json flow; macOS/Linux use the
    // in-app wizard because there is no native installer.

    bool setupCompleted() const;
    void setSetupCompleted(bool completed);

    // ── UPnP NAT traversal ────────────────────────────────────────────────────

    /// Whether UPnP port mapping is enabled for WebRTC NAT traversal.
    /// Default: true (recommended — enables direct P2P connections from outside LAN).
    bool upnpEnabled() const;
    void setUpnpEnabled(bool enabled);

    // ── STUN server ─────────────────────────────────────────────────────────────
    //
    // STUN server URL for WebRTC ICE connectivity. Used by both the backend's
    // libdatachannel PeerConnection and forwarded to the browser for its
    // RTCPeerConnection. Default when the key is absent: "stun:stream.{MW_DOMAIN}:3478"
    // — our own server, the one the Internet-access consent names.

    QString stunServer() const;
    void setStunServer(const QString& url);

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

    // ── Remote admin password ───────────────────────────────────────────────
    //
    // Admin access is normally granted by address: whoever browses from the
    // host machine itself gets it, everyone else never does. This password is
    // the second door — it lets a LAN machine that is NOT the host unlock the
    // same admin access (see AuthManager::validateAdminPassword). Stored only
    // as a PBKDF2 digest; the plaintext never touches disk and cannot be read
    // back, so a forgotten password is reset from the host, not recovered.
    //
    // Three states, because setting the password from the host is exactly what
    // an unreachable host makes impossible:
    //   enabled + no digest → the built-in default is in force (out of the box)
    //   enabled + digest    → the operator chose their own
    //   disabled            → no password is accepted, host-only admin

    /// Encoded digest ("pbkdf2-sha256$<iters>$<salt>$<key>"), empty when the
    /// built-in default still applies.
    QString adminPasswordDigest() const;

    /// Persist an encoded digest. An empty value falls back to the default.
    void setAdminPasswordDigest(const QString& digest);

    /// Whether the LAN may unlock admin access at all. Default: true.
    bool remoteAdminEnabled() const;

    /// Turn remote administration on or off.
    void setRemoteAdminEnabled(bool enabled);

    // ── DNS subdomain ownership ─────────────────────────────────────────────
    //
    // Per-instance random token written to a _owner.<uid> TXT record. Before
    // replacing its A record, an instance verifies this matches (or claims it
    // if absent), so two instances cannot clobber each other's subdomain.

    QString ownerToken() const;
    void setOwnerToken(const QString& token);

    // ── Rendezvous identity (0.3.0+) ────────────────────────────────────────
    //
    // What replaces the sub-domain: the instance is reached at
    // https://stream.{MW_DOMAIN}/{rendezvous_id}. Two values, deliberately
    // distinct from the DNS pair above.
    //
    // rendezvous_id is a LOCATOR, not a secret — it identifies, it does not
    // authenticate (that is the pairing signature's job). It is drawn once and
    // never rotates: an address that changes cannot be bookmarked.
    //
    // rendezvous_token is the credential that proves ownership of that id to
    // the rendezvous server, which stores only HMAC(token). It is NOT
    // owner_token: that one authorises a DNS zone claim and retires with the
    // sub-domain mechanism in February 2027, while this one authorises a
    // rendezvous line and outlives it. Sharing one value between the two would
    // merge two unrelated authorisations, and retiring the first would either
    // strand the second or keep alive a credential that should have died.

    /// 26 Crockford base32 characters, empty until first claimed.
    QString rendezvousId() const;
    void setRendezvousId(const QString& id);

    /// Ownership credential presented as X-MW-Owner. Empty until generated.
    QString rendezvousToken() const;
    void setRendezvousToken(const QString& token);

    /// Whether the id above has been accepted by the rendezvous server. Until
    /// it has, the instance holds an id nothing knows about, and must keep
    /// retrying rather than advertise an address that answers to nobody.
    bool rendezvousClaimed() const;
    void setRendezvousClaimed(bool claimed);

    // ── Internet Access consent (legal traceability) ─────────────────────────
    //
    // Record of the user's explicit opt-in to Internet Access: the exact
    // agreement text displayed, when it was accepted, through which entry
    // point ("admin" | "setup" | "installer"), and for which mechanism.
    // Referenced by every A-record registration entry in the audit log.
    //
    // A consent only covers the mechanism its wording described. Records
    // written before the "version" field existed (version 1, implicit) were
    // obtained for the retiring DNS mechanism — an instance that never
    // registered a subdomain must obtain a fresh consent (version ≥ 2)
    // before opening anything.

    /// {"message", "at" (ISO-8601 UTC), "source", "version", "mechanism"} —
    /// empty if never given; no "version" field means version 1 (DNS wording).
    QJsonObject internetConsent() const;
    void setInternetConsent(const QString& message, const QString& source,
                            const QString& mechanism);

    /// Version of the stored consent record: 0 when none, 1 when the record
    /// predates versioning (DNS-mechanism wording), 2+ otherwise.
    int internetConsentVersion() const;

    // ── Host key (host-machine recognition over the public domain) ──────────
    //
    // Long random token embedded in the host machine's own entry-point URLs
    // (Desktop shortcut, tray, startup open) as ?mwk=..., so a browser opened
    // on the host via the public domain can prove it runs on the host and be
    // granted a localhost-equivalent session. Single-use: each successful
    // redemption rotates the key (and the entry points are rewritten), so a
    // leaked URL cannot be replayed.

    /// Persistent random key; generated on first call.
    QString localKey();

    /// Replace the host key with a fresh random one (after a redemption).
    /// Returns the new key.
    QString rotateLocalKey();

    // ── MW-BIND-v1 host identity key ────────────────────────────────────────
    //
    // ECDSA P-256 key pair proving to a paired browser that the DTLS
    // fingerprint in an SDP offer really is this host's, so that an
    // introduction server relaying the signaling cannot substitute its own and
    // sit in the middle of the session. See docs/design/pairing-signature.md.
    //
    // Deliberately NOT IdentityManager's key: that one is our client identity
    // towards Sunshine, a different role with a different lifetime. Conflating
    // them would mean re-pairing with Sunshine and re-pairing every browser for
    // the same reasons.
    //
    // It must survive updates: losing it invalidates every browser pairing at
    // once, since a browser has no way to tell a new key from a substituted one.

    /// PEM-encoded P-256 private key; generated on first call and persisted.
    QByteArray hostSigningKeyPem();

    /// SPKI (DER) public half of hostSigningKeyPem(), for handing to a browser
    /// at pairing time. Generates the key if it does not exist yet.
    QByteArray hostSigningPublicKey();

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
