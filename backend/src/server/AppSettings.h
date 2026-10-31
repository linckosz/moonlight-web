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

#include <QList>
#include <QString>
#include <QStringList>
#include <QJsonArray>
#include <QJsonObject>

#include "../streaming/StreamConfig.h"

// Persistent settings stored in QStandardPaths::AppDataLocation/settings.json,
// or in the file named by --config.
//
// This class provides a type-safe interface over the JSON settings file
// and handles read/write synchronisation (single-threaded, synchronous I/O).
class AppSettings
{
public:
    explicit AppSettings();

    // ── Settings file location (--config) ───────────────────────────────────

    /// Point every AppSettings built from now on at this file instead of the
    /// one in the data directory. Set once, from the command line, before the
    /// first instance is built; an empty path restores the default. The path
    /// is made absolute here: the children and the relaunches it is handed to
    /// do not all start in this process's working directory.
    static void setFileOverride(const QString& path);
    /// The --config file, absolute, or empty when the default one is in use.
    static QString fileOverride();

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

    // ── Click-to-photon latency flag ──────────────────────────────
    //
    // When enabled, the host shows a French flag at the top of its screen for
    // 100 ms on every injected click, and the browser measures the time from
    // its click to the flag's appearance in the decoded picture — see
    // LatencyFlag.h and frontend/js/stream/LatencyProbe.js.
    //
    // File-only setting (no UI, no write route): stored as JSON bool
    // "latency_flag_enabled", DEFAULT false, read at startup. It is a measuring
    // instrument and not a preference — it puts a tricolour band on the host's
    // screen at every click, which is the last thing a player wants. Edit the
    // file and restart the server; documented in the README.

    bool latencyFlagEnabled() const;

    // ── Packet priority (DSCP) ────────────────────────────────────────────────
    // Stored as JSON bool "packet_marking", DEFAULT true, set from the admin
    // page (« Network priority »): the stream's packets leave in AF41, the
    // Wi-Fi's video queue (streaming/PacketMarking.h). false marks nothing —
    // for a house whose other devices suffer while a stream runs, or a network
    // that mishandles marked packets. Read before each stream.
    bool packetMarking() const;
    void setPacketMarking(bool enabled);

    // ── Passive ICE-TCP ───────────────────────────────────────────────────────
    // Stored as JSON bool "ice_tcp_passive", DEFAULT true (since 0.4.1), file-only
    // and seeded. With it, the TCP rungs of a stream and the entry link's tunnel
    // also listen on their TCP port while their ICE checks run, so that a
    // browser on a network that lets no UDP out has a pair to form: a browser
    // only ever dials (docs/design/ice-tcp-passive.md). The listener lives only
    // while the checks run, keeps nothing that has not authenticated with the
    // session's ICE credentials, and bounds what an unknown peer can hold. On by
    // default once a test from an office network streamed over it (26/10/2026);
    // false keeps every TCP port closed. Read when each connection is built.
    bool iceTcpPassive() const;
    // A DEBUG switch for a bench, never for a user: JSON bool "debug_ice_drop_udp",
    // DEFAULT false, typed into the file by hand. Nothing writes it, not even its
    // default, and no API reads or sets it. With it, no UDP candidate crosses in
    // either direction, on a stream or on the entry link's tunnel, and the UDP
    // rungs leave every chain: passive ICE-TCP then carries everything from a
    // network that lets UDP out (mw::ice::stripUdpCandidates). Read when each
    // connection is built.
    bool debugIceDropUdp() const;

    // ── The guests' shared feed ───────────────────────────────────────────────
    //
    // Whether the guests of this machine's native host watch ONE stream encoded
    // once for all of them (plan « flux commun des invités »), rather than each
    // a stream of their own. File-only (no UI, no write route): stored as JSON
    // bool "shared_feed_enabled", DEFAULT true, read at each guest's join. The
    // safety net beside the MW_SHARED_FEED=0 variable: false puts back the
    // encoder per guest exactly as it was. Windows only for now, whatever it
    // says (SharedFeed::enabled).

    bool sharedFeedEnabled() const;

    // ── Invited players and administrator windows ─────────────────────────────
    //
    // Whether an invited player (a share link) may drive the host's windows that
    // run as administrator. Off, the native host drops their keyboard and mouse
    // presses on such a window (the "policy" gate, InputGate in NativeHost.h);
    // the owner's own sessions are never held back. On, the guest is let in like
    // the owner — and since a SYSTEM worker reaches the secure desktop, that
    // includes answering a UAC prompt and typing on the lock screen: whoever
    // holds the link and its PIN, with keyboard or mouse ticked, is then an
    // administrator of this PC. One switch, no levels: a guest who can drive an
    // administrator console already is one, so nothing in between would protect
    // anything. Only guests with keyboard or mouse are concerned; a gamepad is
    // never gated.
    //
    // File-only setting (no UI, no write route): stored as JSON bool
    // "share_guests_admin_windows", DEFAULT false, seeded so it can be found,
    // read when each guest's worker is launched. Explained to owners in the
    // wiki, Security §6.4c.

    bool shareGuestsAdminWindows() const;

    // ── Sharing from a booth ──────────────────────────────────────────────────
    //
    // Whether booth mode (--noadmin) still lets the one streaming invite guests
    // (issue #40). Off, the booth lets no new guest in. On, the share slots and
    // a guest's PIN are let through again, and nothing else: no PIN login for a
    // new device, no admin, the host list stays frozen.
    //
    // File-only setting (no UI, no write route): stored as JSON bool
    // "share_in_booth_mode", DEFAULT false, seeded so it can be found, read once
    // at startup. Explained to owners in the wiki, Security §6.2.2.

    bool shareInBoothMode() const;

    // ── Keyboard layout fidelity ──────────────────────────────────────────────
    //
    // When the host types the character the CLIENT's keyboard layout produced,
    // whatever layout the host itself runs — an AZERTY viewer types "azerty" on
    // a QWERTY host and reads "azerty" — and when it gets the key's physical
    // POSITION instead, the host's own layout deciding the character, as every
    // Moonlight client does. The mechanism, and what each kind of host can
    // honour, is documented in InputMessageCodec.h.
    //
    //   "auto"   (default) the desktop mouse mode types the client's characters,
    //            gaming mode plays by positions: movement keys and weapon slots
    //            stay where they are, whatever the keyboard. Bruno's rule of
    //            22/10/2026.
    //   "always" the client's characters, gaming mode included.
    //   "never"  positions everywhere.
    //
    // File-only setting (no UI, no write route), read at each stream start and
    // handed to the stream worker in its config (a SYSTEM worker reads another
    // profile's settings.json). It was a boolean until 0.4.0, seeded false
    // since 19/09/2026 for a reason gone by then (Sunshine no longer types a
    // non-letter as text), so a stored value cannot tell a choice from the
    // seed: seedDocumentedDefaults() moves true to "always" and false to
    // "auto". Anything else reads as "auto".

    QString keyboardLayoutFidelity() const;

    // ── This PC's keyboard layout for the touch keyboard ──────────────────────
    // Stored as JSON bool "keyboard_host_layout", DEFAULT false, set from the
    // admin page (« Use this PC's keyboard layout »), offered only when this
    // machine names a layout and a host other than the native one runs here.
    // On: a Sunshine or a Wolf on this machine, which can only type a phone's
    // text by pressing keys, gets the keys this machine's layout puts each
    // character on (issue #44). Read before each stream.
    bool keyboardHostLayout() const;
    void setKeyboardHostLayout(bool enabled);

    // ── Keyboard diagnostics ──────────────────────────────────────────────────
    //
    // On: every printable keystroke writes one line saying how it was resolved
    // and what the host will make of it — the character that reaches a text
    // editor ("Note"), and the physical key a game reading raw scancodes sees
    // ("CS"), each with a verdict. A key whose character cannot be guaranteed is
    // a warning, so the failures stand out in a log full of successes.
    //
    // File-only setting, and DELIBERATELY NOT SEEDED into settings.json: it is
    // an instrument for a bench session, not a preference, and a knob nobody
    // needs is a knob that invites being turned. Add "keyboard_debug": true by
    // hand, restart, type — the README says so. Default false.
    //
    // Never on the secure desktop, whatever the file says: the lock screen's PIN
    // and a UAC prompt's password are typed there, and since the SYSTEM worker
    // reaches that desktop they would have been written down key by key
    // (NativeHost::secureDesktopHasInput, InputMsg::setQuietProbe).
    //
    // Read once at startup rather than per keystroke: a settings file re-read on
    // every key press would put disk I/O on the input path.

    bool keyboardDebug() const;

    // ── The bench's engine knobs on a real session ────────────────────────────
    //
    // "native_tuning": the same `key=value,…` string as MW_NATIVE_TUNING (see
    // NativeBench.h), read by the server at each stream start and handed to the
    // session (a worker's through its config); the native engine takes it when
    // the variable is absent. It is the way to give them to a SYSTEM worker,
    // which would find neither: the launcher service builds its environment
    // from SYSTEM's, and its own AppData is systemprofile's. File-only and never
    // seeded, like keyboard_debug: an instrument for an A/B on an installed
    // edition, logged loudly whenever it is in effect. Empty by default.

    QString nativeTuning() const;

    // "rtp_video": which codecs ride an RTP video track instead of the video
    // DataChannel, per host type — `native:h264+hevc+av1+ultra;other:h264+hevc+av1`
    // (POC Ultra U1.4; "ultra" is the bench's synthetic intra stream). The
    // browser takes the frames off the track by Encoded Transform, before its
    // own decoder, into the same decode path as the DataChannel's. Empty, or a
    // codec not named: SCTP, as before. MW_RTP_VIDEO overrides it. File-only,
    // read at each stream start.

    QString rtpVideo() const;

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

    // ── Stream resolution mode ────────────────────────────────────────────────
    //
    // How the streamed size is chosen. Stored as JSON string "stream_resolution":
    // "auto" (default: the native host's display, brought down to fit the
    // client's screen; 1080p from every other host), "device" (the client's
    // own screen, pixel for pixel), "custom" (the pair below) or "fixed" (the
    // rung in stream_height). The browser resolves the choice into an explicit
    // width and height at launch; these are only the defaults it seeds from.

    QString streamResolution() const;
    void setStreamResolution(const QString& mode);

    // Whether the bitrate is the estimate's (resolution × frame rate × HDR),
    // following the streamed frame, or a value the user set. Stored as JSON
    // bool "stream_bitrate_auto", default true.
    bool streamBitrateAuto() const;
    void setStreamBitrateAuto(bool automatic);

    // The "custom" pair, each bounded to [360, 4096]. Stored as JSON ints
    // "stream_custom_width" / "stream_custom_height", default 1920x1080.
    static constexpr int kCustomSizeMin = 360;
    static constexpr int kCustomSizeMax = 4096;
    int streamCustomWidth() const;
    int streamCustomHeight() const;
    void setStreamCustomSize(int width, int height);

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
    // Target FPS. Stored as JSON int "stream_fps", default 0 — "Auto", the
    // rate the client's own screen refreshes at, resolved per launch from
    // `client_refresh_mhz` (and 60 when the client measured none).
    // Common fixed values: 30, 60, 75, 90, 120, 144, 165, 240.

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

    // ── Streaming notifications (tray) ────────────────────────────────────────
    //
    // Whether the desktop gets a notification when someone starts — or stops —
    // streaming THIS machine through the native host. Stored as JSON bool
    // "stream_notifications", default true.
    //
    // It is our own switch rather than the operating system's on purpose. Both
    // Windows and macOS do let a user silence an application, but only one it
    // has already met, buried in a settings page they would have to be told
    // about; the person who wants the toasts gone wants them gone from the app
    // that sends them. The OS switch keeps working on top of this one.

    bool streamNotifications() const;
    void setStreamNotifications(bool enabled);

    // ── Diagnostic modes (admin page → Advanced) ────────────────────────────
    //
    // "debug_mode" and "verbose_logs", JSON bools, default false. What they do
    // is described in common/RunFlags.h; --debug and --verbose switch the same
    // modes on for one run without writing here.

    bool debugMode() const;
    void setDebugMode(bool enabled);
    bool verboseLogs() const;
    void setVerboseLogs(bool enabled);

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

    // ── The viewer's microphone ────────────────────────────────────────────────
    //
    // Which device it plays into (src/streaming/MicOutput.h). File-only, JSON
    // string "mic_device": part of a playback device's name on Windows (say
    // "CABLE-A Input"); empty or "auto", the default, takes the first virtual
    // cable found. Ignored on Linux, which makes its own virtual source.
    QString micDevice() const;

    /// Host each streaming session in a --stream-worker subprocess (enables two
    /// concurrent sessions). File-only kill switch, default true.
    bool streamWorkerEnabled() const;

    // ── Native host ───────────────────────────────────────────────────────────
    //
    // Whether this machine offers ITSELF as a host — the
    // "<hostname> — MoonlightWeb Host" card. Stored as JSON bool
    // "native_host_enabled", DEFAULT true, file-only and seeded at startup.
    //
    // It gates visibility, not installation: the engine stays installed and
    // intact, it is simply not offered. Set it to false to run MoonlightWeb as
    // a pure front end for other hosts — Sunshine, Wolf, a private cloud-gaming
    // rig — where a card pointing at the machine that merely serves the page
    // would be an invitation to stream the wrong thing.
    //
    // Reading it as a positive ("enabled") with a true default means an absent
    // key, an unreadable file and a fresh install all mean the same thing: the
    // host is offered, exactly as before this setting existed.

    bool nativeHostEnabled() const;
    void setNativeHostEnabled(bool enabled);

    // ── Virtual display ───────────────────────────────────────────────────────
    //
    // What "Add Virtual Display" put on this machine, so the hosts page can
    // offer "Remove" and the requested mode can be told apart from whatever
    // the display woke up in. Stored as JSON object "virtual_display",
    // file-only and NOT seeded — state the job writes, not a preference:
    //   {"added": true, "width": 2560, "height": 1440, "refresh": 120,
    //    "hdr": false, "gpu": "", "added_at": "2026-09-17T10:00:00Z"}
    // Absent (or added:false) means nothing of ours is there.

    QJsonObject virtualDisplay() const;
    void setVirtualDisplay(const QJsonObject& record);
    void clearVirtualDisplay();

    // Stored as JSON bool "virtual_display_exclusive", DEFAULT false, set from
    // the admin page (Windows, the virtual display installed): every other
    // display goes off while a stream runs on ours, and comes back on at its
    // end (issue #39, VirtualDisplay::Request::exclusive). Read as each
    // activation starts.
    bool virtualDisplayExclusive() const;
    void setVirtualDisplayExclusive(bool enabled);

    // ── The native host's display cards ─────────────────────────────────────
    //
    // A card per display, whose id a grid fetched earlier sends back: it has
    // to keep naming the same screen. An enumeration rank does not — on
    // Windows our virtual display, made primary, goes first on its GPU and
    // moves the others down one. So each screen keeps the id it got the
    // first time it was seen. Key "display_app_ids": {"<DisplayInfo::key, in
    // lower case>": card id}, file-only, state the server writes.

    /// The card id of each display of @p keys (DisplayInfo::key): the one it
    /// keeps. A screen seen for the first time gets @p firstIds' entry for
    /// it — what its card was numbered before ids were kept — when that is
    /// free, else the lowest free id below kDisplayAppIdEnd; with every id
    /// taken, a screen not among @p keys gives its own up. Written down at
    /// once. An empty key gets 0.
    QList<int> displayAppIds(const QStringList& keys, const QList<int>& firstIds);
    /// Display cards are numbered 1 to kDisplayAppIdEnd - 1; the virtual
    /// display's card is kDisplayAppIdEnd (NativeHostBackend).
    static constexpr int kDisplayAppIdEnd = 1000;

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

    // ── Statistics consent (GDPR) — dormant, deliberately kept ────────────────
    //
    // Nothing gates on this. It is the machinery for a field that would one day
    // need an answer before it may travel, kept whole rather than rebuilt in a
    // hurry: the stored wording, the timestamp, the entry point, and the
    // version rule that refuses to let an answer given to a narrower question
    // cover a wider one.
    //
    // Why nothing gates on it today: neither census carries a field that
    // identifies anyone. Every value is a number or a token chosen in the
    // source, with no free-form field anywhere; the payload leaves this process
    // and never a browser; no address is stored; and nothing whatever is
    // written to a viewer's device. What is owed for that is the disclosure and
    // a way to say no, both of which live on the Settings page — not a consent.
    // Adding a field that changes this means putting this back in the path, and
    // bumping kMetricsConsentVersion so an old answer does not cover it.
    //
    // Stored as JSON object "metrics_consent", mirroring "internet_consent":
    // the exact wording that was displayed, when it was answered, through which
    // entry point ("banner" | "admin"), the decision, and the version of the
    // wording — so a later wording that describes MORE than this one can ask
    // again instead of inheriting an answer given to a different question.
    //
    // A record left over from the version that did ask is read by nobody, in
    // either direction: a machine that once refused is not silenced by it, and
    // a machine that once agreed is not counted because of it. Only the switch
    // below decides. Whichever way it was answered, it was answered to a
    // question about reporting, and reporting is now on unless someone says
    // otherwise.

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

    /// The one question every reporting path asks: has this machine opted out?
    /// Keeping the rule here means a new caller cannot get it wrong, and means
    /// putting a consent back in the path later is an edit to two functions.
    bool updateRelayAllowed() const;
    bool sessionMetricsAllowed() const;

    /// Turn both censuses on or off from the UI, together — the disclosure the
    /// user reads covers them as one thing. Writes the same two file-only keys
    /// documented above, so a hand-edited settings.json and the switch on the
    /// page are the same control and cannot disagree.
    void setMetricsReporting(bool enabled);

    // Seed documented file-only default keys into settings.json if absent, so they
    // are discoverable/editable in the file, and drop the keys nothing reads any
    // more (native_video_codec: PyroWave is each browser's choice since 0.4.0).
    // Idempotent.
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

    // ── Router ports (sticky UPnP allocation) ──────────────────────────────────
    //
    // The router-side ports this host obtained last time, so it asks for the
    // same ones again and keeps its place among the other MoonlightWeb hosts on
    // the LAN: the first installed keeps the corporate-friendly 3478, whatever
    // order the machines boot in. See network/RouterPortAllocator.h.
    //
    // Stored as JSON object "router_ports", file-only and NOT seeded — it is
    // state the allocator writes, not a preference:
    //   {"lan_ip": "192.168.1.20", "tunnel": [3478, 3479], "media": {"0": 48550}}
    // A remembered port is retried first and forgotten the moment the router
    // says it belongs to another machine. The numbers survive a shutdown — the
    // mappings are removed, the memory is what makes the next start land on
    // the same ones.

    /// Router ports remembered for the control tunnel, in the order obtained.
    QList<quint16> rememberedTunnelPorts() const;
    void rememberTunnelPort(quint16 port);
    void forgetTunnelPort(quint16 port);

    /// Router-side (external) media port remembered for a stream slot, 0 if none.
    quint16 rememberedMediaPort(int slot) const;
    void rememberMediaPort(int slot, quint16 port);
    void forgetMediaPort(int slot);

    /// The LAN address this host had when it obtained those ports. An entry
    /// the router still points at it after a DHCP renumbering is ours to take
    /// back, not a neighbour's to leave alone.
    QString routerLanIp() const;
    void setRouterLanIp(const QString& ip);

    // ── STUN server ─────────────────────────────────────────────────────────────
    //
    // STUN server URL for WebRTC ICE connectivity. Used by both the backend's
    // libdatachannel PeerConnection and forwarded to the browser for its
    // RTCPeerConnection. Default when the key is absent: "stun:stream.{MW_DOMAIN}:3478"
    // — our own server, the one the Internet-access consent names.

    QString stunServer() const;
    void setStunServer(const QString& url);

    // ── Internet Access ───────────────────────────────────────────────────────

    /// Whether the machine may reach out to the internet on the user's behalf
    /// (public-IP detection, router port mapping, the rendezvous line).
    /// Default: false.
    bool internetAccessEnabled() const;
    void setInternetAccessEnabled(bool enabled);

    /// Unique 8-char hex identifier of this instance. Local only: it names the
    /// machine in the admin page and is published nowhere.
    QString uniqueId() const;
    void setUniqueId(const QString& id);

    // ── Desktop portal consent ───────────────────────────────────────────────
    //
    // The token xdg-desktop-portal handed back the one time it asked the user
    // whether MoonlightWeb may record the screen. Replaying it is what turns
    // that dialog into a once-per-installation event instead of a once-per-
    // session one, so it is worth persisting even though nothing breaks
    // without it — the fallback for a lost token is simply being asked again.
    //
    // Stored as JSON string "portal_restore_token", file-only: it is never
    // read by any route and never leaves this machine. Opaque by contract —
    // it means something to the portal that issued it and to nothing else, so
    // it is written and handed back verbatim, never parsed.
    //
    // Empty on every machine but a Linux host that captures through the portal
    // (no capability to read the scanout: an AppImage). Cleared on its own the
    // day the portal refuses it — a stale token is answered with a dialog, and
    // the fresh grant overwrites this one.
    //
    // @p virtualDisplay: the grant for the portal's VIRTUAL source (the
    // "virtual display" card on Linux) — a different consent from a monitor's,
    // kept apart so neither overwrites the other.
    QString portalRestoreToken(bool virtualDisplay = false) const;
    void setPortalRestoreToken(const QString& token, bool virtualDisplay = false);

    // ── Display rates the capture did not hold (vddmatch) ────────────────────
    //
    // The virtual display rates above 240 Hz this host's capture did not hold
    // over a stream's dense content (the native engine's CaptureHold, plan
    // glissade V): `vddmatch` keeps the display under them from the next
    // stream on (VirtualDisplay::matchCapAfterRefusals) — the client's own
    // rate where twice it was refused. Never a mode changed under a stream.
    //
    // Stored as "vdd_match_refused", an array of {gpu, milli_hz, version, at,
    // count}, file-only. One entry per GPU and rate; an entry of another app
    // version is dropped on the next refusal, and ignored before: an update
    // tries again. `at` is when it was last refused (ms since the epoch) and
    // `count` how many times in a row: the cap lasts
    // VirtualDisplay::refusalHoldMs(count) from `at`, and a retry that holds
    // removes the entry (D27, option B of 28/10/2026).
    QJsonArray vddMatchRefused() const;
    /// Returns the entry's count after this refusal, 0 when nothing was kept.
    int addVddMatchRefusal(const QString& gpu, int milliHz, const QString& version, qint64 nowMs);
    void clearVddMatchRefusal(const QString& gpu, int milliHz);

    // ── Instance name ────────────────────────────────────────────────────────
    //
    // What this install calls itself in the header of every browser that has
    // paired with it, and in the switcher listing the other ones. Stored as JSON
    // string "instance_name"; EMPTY is the normal value and means "the name of
    // this PC" — resolved at the route, not here, because the machine name is a
    // platform question and this class holds a file.
    //
    // Editable because the PC name is frequently not a name: "DESKTOP-4K7N2QA"
    // tells nobody which machine they are about to stream from, and the whole
    // point of putting it in the header is that the answer is legible at a
    // glance.

    QString instanceName() const;
    void setInstanceName(const QString& name);

    /// This machine's own name, as the operating system knows it — the value
    /// the setting above falls back to, and the placeholder the admin field
    /// shows. Static because it asks the OS, not the file.
    static QString machineName();

    /// What to actually put on screen: the chosen name, or the machine's own.
    /// The one function every caller should use, so no surface can accidentally
    /// display an empty header.
    QString displayName() const;

    /// Longest name kept. A header is one line; anything past this is a
    /// sentence, and a sentence pushes everything else out of the bar.
    static constexpr int kInstanceNameMaxLength = 32;

    /// The FQDN this host serves, or empty — which is the normal case, since
    /// nothing registers a name any more.
    ///
    /// Returns the stored "domain" when it is a valid FQDN the user owns.
    /// A "{uniqueId}.{MW_DOMAIN}" left there by a pre-0.3 install is NOT
    /// returned: nothing points at that name, so serving it would hand every
    /// entry point a URL that does not resolve.
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

    /// The chain a native Windows session carries its pictures on, from the
    /// capture to the encoder (plan pipeline-video-d3d12-v2, §3.3): "auto" —
    /// the engine's table, measured GPU by GPU — "d3d11" or "d3d12". The
    /// admin's "Advanced" choice. A D3D12 the session cannot build runs D3D11
    /// and says why; ignored off Windows.
    /// Default: "auto", which is also what a value the file holds that is not
    /// one of the three reads as.
    QString nativeVideoPipeline() const;
    /// Stores one of the three names, in any case. False, with the file left
    /// as it was, for anything else.
    bool setNativeVideoPipeline(const QString& pipeline);

    // ── The native host's apps ──────────────────────────────────────────────
    //
    // A card apiece beside the displays, each opening a program on this
    // machine (Linux: in its own gamescope when it can, plan Idées Punktfunk,
    // G5; elsewhere on the virtual display, or the primary one). Key
    // "host_apps": [{"id", "app_id", "name", "command", "quit_command"?,
    // "source", "image"?}], in the owner's order.
    //  - id: a uuid, the entry's name for the page and its image file.
    //  - app_id: the card's id, never reused (host_apps_next_id): a stale grid
    //    can never open another program than the one it shows.
    //  - source: "user", or the launcher found on this PC that seeded it
    //    ("steam", "playnite"); a seeded entry the owner removes goes to
    //    "host_apps_dismissed" and never comes back on its own.
    //  - image: the hash of host-app-images/<id>.png, set by the server only.
    // A command is the host owner's to write, never a viewer's: it runs as the
    // user of this desktop.

    /// The valid entries, in the owner's order. One the file holds that is
    /// not valid is left out.
    QJsonArray hostApps() const;
    /// The entry whose card is @p appId, or an empty object.
    QJsonObject hostApp(int appId) const;
    /// Stores the list the admin page sends: name and command from @p apps,
    /// an entry with a known "id" keeping its card id, source and image, one
    /// without getting new ones. False with @p error, the file left as it was,
    /// for anything not valid (16 entries at most, a name of 1 to 40
    /// characters, a command of 1 to 1000 on one line). The ids of the entries
    /// it dropped go to @p removed, so their images can go too; a seeded one
    /// dropped is dismissed.
    bool setHostApps(const QJsonArray& apps, QString* error, QStringList* removed = nullptr);
    /// Records the image of entry @p id (the hash of its file), or none when
    /// @p hash is empty. False when there is no such entry.
    bool setHostAppImage(const QString& id, const QString& hash);
    /// The sources the owner removed (host_apps_dismissed).
    QStringList hostAppsDismissed() const;
    /// Adds the launcher @p detected ({source, name, command, quit_command?})
    /// found on this PC, at the owner's request: it leaves the dismissed list.
    /// The new entry, or an empty object with @p error (already there, full).
    QJsonObject restoreHostApp(const QJsonObject& detected, QString* error);
    /// Adds each launcher of @p detected not listed yet nor dismissed, once
    /// per version of this app (host_apps_seeded_version): the first start
    /// after an install or an update. The entries it added go to @p added.
    /// False, the file untouched, when this version has already seeded.
    bool seedHostApps(const QJsonArray& detected, const QString& version, QJsonArray* added);
    /// Moves the apps of 0.4.0's betas ("gamescope_apps", Linux) into
    /// host_apps, each keeping its card id (1100 + its rank), and gives an id
    /// to an entry edited in by hand without one. Idempotent; at startup.
    void migrateHostApps();
    /// Where the entries' images live: host-app-images/ beside this file.
    QString hostAppImagesDir() const;
    /// The limits setHostApps holds to, for the page that edits them.
    static constexpr int kHostAppsMax = 16;
    static constexpr int kHostAppNameMax = 40;
    static constexpr int kHostAppCommandMax = 1000;
    /// The first card id handed out; the betas' gamescope apps kept 1100-1115.
    static constexpr int kHostAppIdFirst = 2000;

    // ── HMAC key for session tokens ──────────────────────────────────────────

    /// Persisted HMAC signing key. Generated once, reused across restarts.
    /// Stored as Base64 string. Empty if never set.
    QByteArray hmacKey() const;
    void setHmacKey(const QByteArray& key);

    /// Certificate PEM source: env var name (e.g. "MW_CERT_PEM") or file path.
    /// Defaults to "MW_CERT_PEM" (reads the PEM from environment variable).
    /// A file path when the user dropped their own certificate in.
    QString certPem() const;
    void setCertPem(const QString& value);

    /// Private key source: env var name (e.g. "MW_CERT_KEY") or file path.
    /// Defaults to "MW_CERT_KEY" (reads the PEM from environment variable).
    /// A file path when the user dropped their own key in.
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

    /// Whether the password is also accepted from outside the LAN, through the
    /// rendezvous tunnel only. Default: false — an operator who set a password
    /// for the machines in the house never finds it facing the internet unless
    /// they asked for exactly that.
    bool remoteAdminInternet() const;

    /// Open or close the internet side of the remote admin door.
    void setRemoteAdminInternet(bool enabled);

    // ── Rendezvous identity (0.3.0+) ────────────────────────────────────────
    //
    // How the instance is reached from the internet:
    // https://stream.{MW_DOMAIN}/{rendezvous_id}. No DNS record, no
    // certificate order, no port forward.
    //
    // rendezvous_id is a LOCATOR, not a secret — it identifies, it does not
    // authenticate (that is the pairing signature's job). It is drawn once and
    // never rotates: an address that changes cannot be bookmarked.
    //
    // rendezvous_token is the credential that proves ownership of that id to
    // the rendezvous server, which stores only HMAC(token).

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
