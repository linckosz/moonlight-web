/*
 * MoonlightWeb — native capture & encoding engine.
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

#include "Capabilities.h"
#include "EncoderTuning.h"
#include "VideoPipeline.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mw::native {

/// Where a session's pictures come from.
enum class VideoSource
{
    /// The session captures its display and encodes it: every session but
    /// one kind.
    Capture,
    /// Another process encodes them — the guests' shared feed of a native
    /// host (plan « flux commun des invités ») — and the consumer carries
    /// them to its browser itself. The session only injects its viewer's
    /// input and captures the host's audio: no capture, no encoder, no loop.
    External,
};

/// What one streaming session asks for. Deliberately small: every knob Sunshine
/// exposes (GOP, B-frames, preset, tuning, lookahead, VBV, QP) is decided by the
/// engine and is absent here on purpose (§28 of the mission).
///
/// The fields that ARE here all come from choices the user already makes today,
/// in the stream settings shared by every host — plus the display they clicked.
struct SessionConfig
{
    /// Which display to stream — DisplayInfo::id. The one and only technical
    /// choice a user makes.
    int displayId = -1;

    /// Requested output size. Zero means "native resolution of the display",
    /// which is the default. When smaller, the scale happens in the same GPU
    /// pass as the colour conversion, so it costs nothing extra.
    int width = 0;
    int height = 0;

    /// Requested frame rate. Zero means "the display's own refresh rate".
    int fps = 0;

    /// A rate the session may not exceed, whatever `fps` says and whatever the
    /// vsync alignment would rather have. Zero — the default — means none.
    ///
    /// `fps` is a wish: a client presenting on its vsync is served the nearest
    /// divisor of its own refresh within a fifth of it, which may be FASTER
    /// (144 Hz client, 120 set, 144 streamed — see CadenceAlign.h). That trade
    /// is the right one when the viewer named the rate. It is the wrong one
    /// when the rate was chosen FOR them: the browser's "Auto" sizes a pixel
    /// budget — resolution × rate × the whole chain — and picks the rate that
    /// fits it, so a host that then streams 20% more frames undoes the very
    /// arithmetic that produced the number. This is how "Auto" says so.
    ///
    /// Read together with the client's own live cap (a decoder that cannot
    /// keep up asks for fewer frames still): the smaller of the two wins.
    int maxFps = 0;

    int bitrateKbps = 20000;

    /// Codecs the BROWSER accepts, best first. The engine intersects this with
    /// what the display's GPU can encode and picks the first survivor — which
    /// is why no codec question is ever asked (§27).
    ///
    /// Empty is a programming error, not a default: an empty list would leave
    /// the engine guessing what the client can decode.
    ///
    /// Stays the client's WHOLE list all the way to the platform backend: the
    /// codec that was picked travels separately (ResolvedTarget::codec), and a
    /// backend that has to change its mind late — the Linux portal route
    /// discovers only mid-start that it must encode on the CPU, in H.264 —
    /// needs to know what else the client would accept.
    std::vector<Codec> clientCodecs;

    /// Ask for HDR. Honoured only when the display is actually in an HDR mode
    /// AND the chosen encoder does 10-bit; otherwise the session runs SDR and
    /// says so in SessionInfo, rather than failing.
    bool hdr = false;

    /// Ask for 4:4:4 chroma. MoonlightWeb already offers this for external
    /// hosts, and it matters on a desktop rather than on video: 4:2:0 keeps a
    /// quarter of the colour resolution, which shows on text and thin UI lines.
    /// Honoured only when the encoder supports it — see SessionInfo::yuv444.
    bool yuv444 = false;

    /// Encode with intra-refresh instead of relying on keyframes for recovery.
    ///
    /// Each frame then carries a moving band of intra blocks, so a receiver
    /// that has lost data repairs itself within one refresh cycle rather than
    /// waiting for a fresh keyframe.
    ///
    /// **Only worth asking for when the receiver rides it out.** MoonlightWeb's
    /// default recovery discards deltas and demands an IDR on any gap, which
    /// collects none of that benefit while still paying for it in slightly
    /// larger P-frames. So this is off unless the client says it will keep
    /// decoding through the damage — see SessionInfo::intraRefresh for what was
    /// actually granted.
    bool intraRefresh = false;

    /// Intra-refresh is not a wish here but the point: a route that would run
    /// without it is passed over for one that grants it, where the GPU has one
    /// (VideoPipelineChoice.h: the D3D12 route of an Arc sweeps a single frame,
    /// D3D11 oneVPL sweeps a real wave). Implies `intraRefresh`.
    /// SessionInfo::intraRefresh still says what was granted: an encoder that
    /// has no intra-refresh at all streams keyframes on demand.
    ///
    /// The bench's intra=2 only. The guests' shared feed of a native host was
    /// built with it and no longer is: on an Arc, its D3D11 oneVPL encoder
    /// beside the owner's D3D12 Video Encode took the owner's p99 from 7.8 to
    /// 27 ms (S9, 01/10/2026). The feed asks `intraRefresh` on the machine's
    /// own route, and rations keyframes where that route has none.
    bool intraRefreshRequired = false;

    /// The rate governor's floor, as a share of the setting (encode::
    /// RateGovernor). 20, the default, is every viewer's own stream: under a
    /// fifth of the setting the picture is not worth sending. The guests'
    /// shared feed holds 60, so that the slowest of them cannot drag the
    /// picture of all the others down with it.
    int governorFloorPercent = 20;

    /// Where the pictures come from — see VideoSource. With External, only
    /// `displayId`, `allowElevatedInput` and `muteHostAudio` are read, plus
    /// the rectangle below; the video fields are the feed's business.
    VideoSource videoSource = VideoSource::Capture;

    /// With External: where the display sits on the host's desktop, which is
    /// where absolute pointer positions land. The feed's capture is the one
    /// that knows it, so the feed says it (Session::setExternalDesktop
    /// follows a change). Empty (right <= left): the display's origin is not
    /// known, and absolute positions are not injected until it is.
    int externalLeft = 0;
    int externalTop = 0;
    int externalRight = 0;
    int externalBottom = 0;

    /// Draw the mouse cursor into the captured frame. On by default: the remote
    /// user needs to see where they are pointing.
    bool captureCursor = true;

    /// The client's display refresh, in millihertz, when the browser measured
    /// one. Zero means "unknown". Read together with clientVsync: a client
    /// that presents on its vsync gets a stream cadence that is an integer
    /// divisor of this rate, within a fifth of `fps`, so frames land on its
    /// grid instead of beating against it (§9.11, CadenceAlign.h). Ignored
    /// when `fps` is 0 (the host's own rate) or when the client tears.
    int clientRefreshMilliHz = 0;

    /// True when the client paints frames on its display's vsync — tearing
    /// off, or a browser that cannot tear. False, the default, means frames
    /// are painted the moment they are decoded and there is no grid to align
    /// on: the setting is followed as it is, which is the lowest latency.
    bool clientVsync = false;

    /// True when the client's tearing is real: with clientVsync false, its
    /// canvas shows each frame the moment it is drawn — Chromium on Windows.
    /// A browser that tears into a compositor that waits for vsync anyway
    /// (Chromium on macOS) or one not measured (Linux) says false, as does an
    /// older frontend. The admission window after a press follows it
    /// (EncoderTuning::inputAdmit): a client on its vsync gained nothing from
    /// it and stepped over pictures (bench §8w.9).
    bool clientTearingReal = false;

    /// True when the viewer cuts every decoded frame to the size the session
    /// announces (SessionInfo, DisplayFormat). An encoder that has to pad the
    /// frame past the picture says the picture in a field browsers skip — AV1's
    /// render size, which Chrome ignores — and a frame shown whole shows the
    /// padding. With this, the Linux Vulkan AV1 encoder pads (1920x1088 for
    /// 1080p); without, it scales the picture onto its grid (1792x1008).
    bool clientCropsToFrame = false;

    /// The viewer reads MWSR's sharpness by zone (its `/start` said
    /// `mwsr_map`): the session then takes the statistics of every captured
    /// picture and sends each frame with its grid (EncodedFrame::mwsrZones).
    /// Only where the grid was measured: a D3D11 chain, an 8-bit SDR capture,
    /// reduced to the stream (plan B, upscaler-mwsr-b.md §12 c). False, the
    /// default and an older frontend's: nothing taken, nothing sent, and the
    /// client keeps its law.
    bool mwsrZones = false;

    /// Let the viewer act on windows that run elevated (as administrator).
    ///
    /// True — the default — is the trusting setting, right for the machine's
    /// own administrator streaming their own desktop. False is for a viewer
    /// who was let in without that standing: the engine keeps injecting into
    /// ordinary windows and drops presses aimed at an elevated one, saying so
    /// through the InputGateCallback. The OS may refuse elevated windows on its
    /// own regardless (see InputGate); this flag only ever narrows further.
    bool allowElevatedInput = true;

    /// Silence the host's own speakers for the length of the session, the way
    /// GameStream's `localAudioPlayMode=0` does: the viewer hears the game, the
    /// room does not. Best effort — whether the machine has a way to mute its
    /// output without muting the capture is the platform's to find out
    /// (`audio/windows/HostMute.h`, `audio/macos/HostMute.h`,
    /// `audio/linux/HostMute.h` — three platforms, three different answers to
    /// where the tap sits), and SessionInfo::hostMuted says what happened.
    /// False leaves the speakers alone.
    bool muteHostAudio = false;

    /// A consent the user gave the ScreenCast portal on an earlier session, to
    /// be replayed so it is not asked for again.
    ///
    /// Linux only, and only on the machines that fall back to the portal at all
    /// (an AppImage: no capability to read the scanout). Empty on a first run,
    /// which is the one time the user sees a dialog — the grant that comes back
    /// arrives through SessionCallbacks::onPortalGrant, and whoever stores it
    /// hands it back here next time. Ignored everywhere else.
    ///
    /// ⚠️ The portal's virtual display (kPortalVirtualDisplayKey) is a consent
    /// of its own: a session on it takes, and reports, THAT grant — the
    /// consumer keeps the two apart.
    std::string portalRestoreToken;

    /// Let the frame follow the display's shape when the host changes its mode
    /// under a running session — a game switching the desktop to 4:3, a
    /// portrait rotation, a 16:10 panel swapped in.
    ///
    /// The session always STARTS in the display's shape (the Selector keeps
    /// only the client's height). What this decides is the change: true
    /// rebuilds the encoder at the new shape, same height, and says so through
    /// the DisplayFormatCallback; the client's decoder follows the new
    /// parameter sets on the keyframe that comes with it. False, the default,
    /// keeps the negotiated size and lets the conversion pass absorb the
    /// difference — letterboxed on macOS and on the Lanczos-2 resample path
    /// of Windows and Linux, stretched by the bilinear one (software tier).
    ///
    /// The browser sets it when the viewer's aspect setting is "Auto": a
    /// ratio chosen by hand is a ratio the viewer wants kept.
    bool followDisplayShape = false;

    /// Read `width` × `height` as a box to fit rather than a height to keep:
    /// the frame is the largest one of the display's shape inside it (a 16:9
    /// display asked for 1920x1200 streams 1920x1080). What a client sends
    /// with a size that is its own — its screen ("Same as your device") or a
    /// pair it typed ("Custom") — so the picture never comes out squeezed
    /// into a shape that is not the display's, and never wider than it asked.
    /// False, the default, keeps the height and takes the display's shape.
    bool fitRequestedBox = false;

    /// Let the frame be larger than the display. The Selector never upscales
    /// otherwise (the browser scales up for free, the host would pay for
    /// nothing) — the one reason to is a client asking for its own screen's
    /// size, since a pixel of the frame is then a pixel of that screen. Read
    /// only with fitRequestedBox.
    bool allowUpscale = false;

    /// Put the display in the requested mode (`width` × `height`) for the
    /// session, when its driver lists one: the client's "Match my screen",
    /// which then gets its own pixels without a single scale. Best effort —
    /// no listed mode of that size, or a refusal, leaves the display as it is
    /// and the frame follows the box rule above. The original mode is put
    /// back when the session ends. Read only with fitRequestedBox.
    bool matchClientDisplay = false;

    /// The size as the client asked it, before the Selector shaped `width` ×
    /// `height` to the display: what matchClientDisplay looks a display mode
    /// up for. Set by the engine on the resolved config a platform session
    /// receives; zero on a config a caller built.
    int requestedWidth = 0;
    int requestedHeight = 0;

    /// The box to fit instead when matchClientDisplay cannot be honoured —
    /// no listed mode, a refusal, a platform that changes no modes: the
    /// client's Auto box, so the session becomes exactly what Auto would have
    /// been (never upscaled). Zero: fit the requested box itself, no upscale.
    /// See fallBackFromMatch().
    int fallbackWidth = 0;
    int fallbackHeight = 0;

    /// The refresh rate a display this session makes for itself is created at
    /// — the portal's virtual monitor on Linux, made by the stream. 0 = the
    /// stream's own rate. The consumer sets it as it sets its own virtual
    /// display's (240 Hz on Windows and Linux, plan framerate-hote and C0 of
    /// plan Idées Punktfunk): a picture waits for the compositor at most
    /// 1/240 s, and the stream keeps its cadence — the session's gate takes
    /// the first present of each of its intervals. Ignored where the session
    /// makes no display.
    int virtualRefreshHz = 0;

    /// The owner's session on the display a session makes for itself — the
    /// virtual monitor on Linux: it is made the desktop's primary while it
    /// streams, as the server makes its own virtual display primary on Windows
    /// and macOS (on GNOME the top bar and the dock come to the stream, which
    /// would otherwise show an empty extension of the desktop). On GNOME's own
    /// route (Mutter's screen cast) the owner's session always makes its
    /// monitor, at its client's size; a guest's — false — records that one, the
    /// owner's desktop, and makes one itself, primary too, only when nobody
    /// else streams (capture/linux/SharedMonitor.h). Through the portal a
    /// guest's monitor stays beside the owner's. Ignored where the session
    /// makes no display, and off GNOME.
    bool virtualPrimary = false;

    /// One of the user's apps in its own gamescope, on Linux (kGamescopeAppDisplayKey):
    /// its name, which names its session, and the command it runs, through
    /// /bin/sh. Both from the server's settings, the host owner's to write.
    /// Empty everywhere else.
    std::string gamescopeApp;
    std::string gamescopeCommand;

    /// The chain that carries the pictures on Windows (VideoPipeline.h). Auto,
    /// the default, is the engine's vendor table; the admin's "Advanced"
    /// setting can force one. A D3D12 the session cannot build — a display
    /// captured through Windows.Graphics.Capture, a cross-GPU copy, 4:4:4, a
    /// codec the route does not do yet — runs D3D11 and says why in
    /// SessionInfo. Ignored off Windows.
    VideoPipeline videoPipeline = VideoPipeline::Auto;

    // ── Bench-only, below this line ─────────────────────────────────────────
    //
    // Neither field is ever set by a session a browser started. They exist so
    // `--native-bench` can measure the engine's choices against the
    // alternatives, on hardware the choices were not made on. See EncoderTuning.

    /// Encoder settings to override the engine's own with. Default = the
    /// engine's choice, which is the product.
    EncoderTuning tuning;

    /// Encode on THIS GPU (GpuInfo::id) rather than on the display's own,
    /// paying the cross-GPU copy the Selector otherwise avoids. -1, the
    /// default, lets the Selector decide. How the bench reaches an encoder
    /// that drives no display — an iGPU next to a discrete card, typically.
    int encodeGpuId = -1;
};

/// What the engine settled on once a session started. Mirrored into the stats
/// overlay so the user can see the GPU and encoder actually in use — the only
/// technical information the UI shows (§28).
struct SessionInfo
{
    int displayId = -1;
    int width = 0;
    int height = 0;
    int fps = 0;

    Codec codec = Codec::H264;
    EncoderApi encoder = EncoderApi::None;
    CaptureApi capture = CaptureApi::None;

    /// e.g. "NVIDIA GeForce RTX 4070"
    std::string gpuName;

    /// True when the session really is 10-bit HDR. May be false even though
    /// SessionConfig::hdr was true — see that field.
    bool hdr = false;

    /// The display as it stands when the session starts: its desktop size in
    /// pixels, and whether it is in an HDR mode. The frame size above is what
    /// is encoded; these are what is captured. Updated for the viewer through
    /// the DisplayFormatCallback, not here.
    int displayWidth = 0;
    int displayHeight = 0;
    bool displayHdr = false;

    /// Where the captured display sits on the host's desktop, as the capture
    /// found it (Windows; zero elsewhere): what the guests' shared feed tells
    /// its subscribers, whose input aims there (SessionConfig::externalLeft).
    /// Kept up to date across a capture restart, like the frame size.
    int desktopLeft = 0;
    int desktopTop = 0;
    int desktopRight = 0;
    int desktopBottom = 0;

    /// Whether this session's encoder would carry HDR, were the display in an
    /// HDR mode and the client asking — a 10-bit GPU encoder on HEVC or AV1.
    /// Lets a client tell "the host is in SDR" from "this host never does HDR",
    /// so it relaunches for the first and not for the second.
    bool hdrCapable = false;

    /// True when the session really is 4:4:4. Same contract as `hdr`: asked for
    /// is not granted, and the stats overlay is where the difference shows.
    bool yuv444 = false;

    /// True when the stream really refreshes by intra-refresh rather than by
    /// keyframes. Same contract again — an encoder that cannot do it says so
    /// here, and the receiver must then keep its usual keyframe recovery.
    bool intraRefresh = false;

    /// How many encoded frames one intra-refresh wave takes to sweep the whole
    /// picture — the period the encoder was configured with, in frames of the
    /// rate it encodes at (RateControl.h intraRefreshPeriodFrames). Zero when
    /// intraRefresh is false. The receiver counts the frames it gets against
    /// this to know when a wave has had its chance: in frames, because the
    /// wave advances per frame encoded, not per second — a game presenting at
    /// 30 under a 165 fps stream stretches a 330-frame period to 11 s.
    /// Where the encoder leaves a gap between waves (oneVPL, four periods —
    /// RateControl.h intraRefreshDistanceFrames), this is the gap instead: a
    /// loss just after a wave waits for the next one.
    int intraRefreshFrames = 0;

    /// True when a frame the receiver lost can be healed by an ordinary delta
    /// (Session::invalidateReference does what it says). The receiver then
    /// keeps decoding after a gap instead of discarding deltas until a
    /// keyframe; false means the engine answers an invalidation with an IDR.
    bool referenceInvalidation = false;

    /// Memory copies between capture and the wire, counted rather than
    /// estimated. Logged at session start and compared in the benchmarks (§16);
    /// if this number ever grows, a zero-copy path silently broke.
    int copiesPerFrame = 0;

    /// True when the frame has to cross from the display's GPU to a different
    /// GPU to be encoded (§6). Costly and rare — always worth a log line.
    bool crossGpuCopy = false;

    /// The chain the pictures take on Windows — D3D11 or D3D12; Auto on the
    /// platforms that have no such choice — the route inside it ("D3D11", or
    /// "DIRECT conversion → D3D12 Video Encode HEVC"), and why: the setting,
    /// the vendor table, a bench key, or what made a D3D12 session run D3D11.
    /// For the log, the stats overlay and the bench's rows.
    VideoPipeline videoPipeline = VideoPipeline::Auto;
    std::string videoRoute;
    std::string videoPipelineReason;
    /// The D3D12 route's encoder as the overlay names it ("D3D12 VE", "NVENC
    /// (D3D12)"), empty while D3D11 runs; and whether D3D12 was asked for — by
    /// the setting, a bench key or the vendor table — while D3D11 runs, so the
    /// overlay can say so in a word. The reason is the log's.
    std::string videoEncoder;
    bool videoPipelineRefused = false;
    /// The same D3D12 encoder as the engine knows it, Default while D3D11 runs:
    /// what a rule keyed on the encoder reads (nameLinkDropsByDefault), rather
    /// than the overlay's words.
    EncoderTuning::Encoder12 videoEncoder12 = EncoderTuning::Encoder12::Default;

    /// True when the session captures the host's playback and delivers Opus
    /// packets. False when no audio callback was given, or when the platform
    /// could not open a playback device — the stream is then silent, and the
    /// log says why.
    bool audio = false;

    /// True when the host's speakers are silenced for this session
    /// (SessionConfig::muteHostAudio honoured). False when it was not asked,
    /// or when this machine has no way to do it without silencing the capture
    /// too — the log names the reason.
    bool hostMuted = false;
};

} // namespace mw::native
