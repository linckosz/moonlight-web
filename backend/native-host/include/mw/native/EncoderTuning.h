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

#include "VideoPipeline.h"

#include <cstdio>
#include <string>

namespace mw::native {

/// The encoder knobs the engine decides for itself — exposed so the BENCH can
/// move them, one at a time, and measure what each one costs.
///
/// ── This is not configuration ───────────────────────────────────────────────
///
/// Nothing in the product sets any of these: the engine's own choice is what
/// every field's `Default` means, and a session started from a browser never
/// carries anything else (§28 of the mission — no preset, no tuning, no VBV in
/// front of a user). The struct exists because the choice has to be MEASURED
/// before it is made, on every vendor's silicon, and a benchmark that cannot
/// vary the setting under test is not one. `--native-bench` is the only caller
/// that fills it in.
///
/// Tri-state on purpose: "leave it to the preset" is a distinct answer from
/// "off", and the difference is exactly what the bench wants to see — what a
/// vendor's ultra-low-latency preset actually enables, and whether it was right.
struct EncoderTuning
{
    enum class Choice
    {
        Default,
        Off,
        On
    };

    /// NVENC: the P1 (fastest) … P7 (best quality) preset. 0 is the engine's
    /// own (P1 since the bench of 04/09/2026 — see NvencEncoder.cpp).
    int nvencPreset = 0;

    /// NVENC: which latency tuning the preset is fetched for.
    enum class Latency
    {
        Default,
        UltraLow,
        Low
    };
    Latency nvencTuning = Latency::Default;

    /// NVENC: a first pass at reduced or full resolution to steer the rate
    /// control of the second. Costs encode time; buys a steadier CBR.
    enum class MultiPass
    {
        Default,
        Off,
        QuarterRes,
        FullRes
    };
    MultiPass nvencMultiPass = MultiPass::Default;

    /// NVENC: the QP the rate control never goes below. 0 is the engine's own
    /// (18, qindex 32 for AV1 — see NvencEncoder.cpp), -1 none at all.
    int nvencMinQp = 0;
    /// AMF (H.264, HEVC): the same floor. 0 is the engine's own (18 — see
    /// AmfEncoder.cpp), -1 none at all.
    int amfMinQp = 0;
    /// VA-API: the same floor. 0 is the engine's own (18 — see
    /// encode::kVaapiMinQp, design §32.28), -1 none at all.
    int vaapiMinQp = 0;

    /// NVENC intra-refresh: frames from one sweep's start to the next
    /// (intraRefreshPeriod) and frames the band takes to cross the picture
    /// (intraRefreshCnt). 0 is the engine's own for each.
    int nvencIntraRefreshPeriod = 0;
    int nvencIntraRefreshCount = 0;

    /// Spatial adaptive quantization: NVENC `enableAQ`, AMF VBAQ, AMF AV1 CAQ.
    Choice spatialAq = Choice::Default;
    /// NVENC only: temporal AQ.
    Choice temporalAq = Choice::Default;
    /// AMF only: the pre-analysis module ahead of the rate control.
    Choice preAnalysis = Choice::Default;

    /// AMF: the quality preset — speed, balanced, quality. Default is whatever
    /// the ultra-low-latency usage picks.
    enum class AmfQuality
    {
        Default,
        Speed,
        Balanced,
        Quality
    };
    AmfQuality amfQuality = AmfQuality::Default;

    /// AMF H.264/HEVC: the encoder's internal low-latency mode
    /// (`LowLatencyInternal`, which also puts H.264 in POC mode 2).
    ///
    /// Its own header says `default = false` — flatly, not "depends on USAGE"
    /// like every other knob here — so the ultra-low-latency usage is not
    /// documented to switch it on. Worth a measurement rather than a guess: it
    /// is the one AMD knob this engine has never touched. AV1 has no such
    /// property; it has an explicit latency mode, already set to its lowest.
    Choice amfLowLatency = Choice::Default;

    /// oneVPL: TargetUsage 1 (quality) … 7 (speed). 0 is the engine's own (7).
    int vplTargetUsage = 0;

    // ── The rest of what Intel exposes ──────────────────────────────────────
    //
    // Six knobs, none of which this engine sets on its own except the first,
    // and all of which the Intel matrix measures rather than assumes. They live
    // here for the same reason every other field does: a benchmark that cannot
    // vary the setting under test is not one.

    /// The fixed-function encode engine (VDENC) rather than the shader-based
    /// one. The engine's own answer is ON — measured, it halves the encode time
    /// — with an automatic fall-back when a generation has none.
    Choice vplLowPower = Choice::Default;
    /// Macroblock-level rate control: spends bits where the picture needs them
    /// rather than evenly. Intel's answer to spatial AQ.
    Choice vplMbBrc = Choice::Default;
    /// The alternative bitrate controller. Intel documents it as better on
    /// low-delay content, which is exactly this pipeline's content.
    Choice vplExtBrc = Choice::Default;
    /// The low-delay mode of the bitrate controller — one frame in, one frame
    /// out, no lookahead budgeting.
    Choice vplLowDelayBrc = Choice::Default;
    /// `ScenarioInfo = MFX_SCENARIO_REMOTE_GAMING`, a hint Intel added for this
    /// exact use. What the driver does with it is not documented, which is why
    /// it is measured.
    Choice vplGamingScenario = Choice::Default;
    /// Sliding-window rate cap, in frames: no window of this many frames may
    /// average more than the target. A burst limiter, priced in quality.
    int vplWinBrcFrames = 0;
    /// The bitrate controller itself. CBR spends its whole budget on every
    /// frame, a still one included; VBR under the same MaxKbps and the same
    /// buffer may spend less; QVBR aims at a quality (vplQvbrQuality, 1..51)
    /// under that same cap.
    enum class VplRateControl
    {
        Default,
        Cbr,
        Vbr,
        Qvbr
    };
    VplRateControl vplRateControl = VplRateControl::Default;
    int vplQvbrQuality = 0;
    /// QP offset of the intra-refresh band, oneVPL (IntRefQPDelta, -51..51).
    /// 0 is the engine's own: the band at the frame's own quality.
    int vplIntraRefreshQpDelta = 0;
    /// Frames between the starts of two intra-refresh sweeps, for the encoders
    /// that leave a gap between them: oneVPL (IntRefCycleDist) and Vulkan Video
    /// (encode::IntraRefreshSweep). 0 is the engine's own
    /// (encode::intraRefreshDistanceFrames, four periods); -1 is back to back,
    /// oneVPL's fallback for a runtime that refuses the gap. VA-API too, since
    /// 05/10/2026 (design §32.28).
    int intraRefreshDist = 0;

    /// How many reference pictures the encoder keeps for healing a lost frame
    /// by a delta. NVENC: the decoded picture buffer's depth (engine's own: 4
    /// — see NvencEncoder). AMF: the number of long-term reference slots
    /// (engine's own: 4 — see AmfEncoder / ReferenceSlots). 0 is the engine's
    /// own; 1 is the bench's "before" — a single reference, no invalidation
    /// possible on either vendor — for the cost of the feature.
    int dpbFrames = 0;

    /// The link governor (encode::RateGovernor). Off, the encoder's target is
    /// the setting, moved both ways at once by setTargetBitrate: a bench has
    /// no receiver, so the governor would cut the rate for the silence and
    /// never pass a step back up. Every other value is the engine's own: on.
    Choice linkGovernor = Choice::Default;

    /// The delta the relay drops when the link stops draining (SendBacklog),
    /// named to the encoder like the sender's evictions (design §9.10.2): the
    /// next delta predicts from a frame the client has, where the stream
    /// otherwise waits for a keyframe or the refresh wave (plan §9-25). The
    /// engine's own is nameLinkDropsByDefault, below: on for NVENC, fed D3D11
    /// or D3D12 pictures, and for AMF through D3D11 since 29/09/2026, off
    /// elsewhere. A real session's (MW_NATIVE_TUNING): the relay reads it.
    Choice nameLinkDrops = Choice::Default;

    /// The VBV, in frames at the stream's own rate — exactly, with no floor.
    /// 0 is the engine's rule: one frame, never less than a sixtieth of a
    /// second's worth (RateControl.h says why). 1 and 2 are the two bounds the
    /// bench compares that rule against.
    int vbvFrames = 0;

    /// Bench only: pretend no GPU in this machine can encode, so the session
    /// lands on the fallback tier — and, optionally, on ONE named member of it.
    /// The only way to exercise Media Foundation or the CPU encoder on a bench
    /// that has NVENC, and to price them against it on the same content.
    enum class Fallback
    {
        None,                    ///< the engine's choice: GPUs first, the tier only without them
        Tier,                    ///< whatever the tier would pick on an encoder-less machine
        MediaFoundation,         ///< the Media Foundation transform, hardware or software
        MediaFoundationSoftware, ///< Microsoft's software transform even where hardware exists
        MediaFoundationCpuInput, ///< the hardware transform, fed through system memory
        Cpu                      ///< OpenH264
    };
    Fallback fallback = Fallback::None;

    // ── The D3D12 pipeline (plan pipeline-video-d3d12-v2) ───────────────────
    //
    // Each one defaults to the engine's own choice, like everything above;
    // none means anything to a session running D3D11.

    /// The chain, over the setting and the vendor table (VideoPipeline.h).
    VideoPipeline pipeline = VideoPipeline::Auto;
    /// The D3D12 conversion's queue: DIRECT with the pixel shaders D3D11 runs
    /// (the engine's own), or COMPUTE.
    enum class ConvertQueue12
    {
        Default,
        Direct,
        Compute
    };
    ConvertQueue12 conv12 = ConvertQueue12::Default;
    /// The D3D12 route's encoder: D3D12 Video Encode (the engine's own), or
    /// the vendor's SDK fed D3D12 pictures.
    enum class Encoder12
    {
        Default,
        VideoEncode,
        Nvenc,
        Amf,
        /// POC Ultra (U4): PyroWave on a compute queue, any GPU of the route.
        Pyrowave
    };
    Encoder12 enc12 = Encoder12::Default;
    /// D3D12 Video Encode's rate control: the driver's CBR where it can move
    /// its target, the in-house QP controller where it cannot (Intel). Qp
    /// runs the in-house one everywhere, as a witness where it is not needed.
    enum class RateControl12
    {
        Default,
        Driver,
        Qp
    };
    RateControl12 rc12 = RateControl12::Default;
    /// The in-house rate control only: a picture far over its budget is coded
    /// again at the QP that fits it — an encode more on that picture, against
    /// a burst on the link. On is the engine's own since the bench of
    /// 27/09/2026 (plan §9-5): scrolling text went from 12 such pictures sent
    /// to 1, for the same mean host time. Off is the bench's "before".
    Choice reencode12 = Choice::Default;
    /// How that picture is coded again. On, the engine's own since
    /// 28/09/2026: at two budgets — under the overshoot line — by the slope
    /// learned, then by the textbook if it still lands far over. Off, the
    /// bench's "before": once, at its budget, by the textbook's slope. On
    /// scrolling text, on went from 0.68 to 0.83 of the target on the N95 and
    /// sent no picture far over on either the N95 or the Arc, for a third
    /// encode on 0.5 % of the pictures (bench §8n.9-§8n.10, plan §9-14).
    Choice reencodeFit12 = Choice::Default;
    /// The in-house rate control only: a new picture is never believed to
    /// cost under 1/2^k of an intra one. 0 is the engine's own — no floor,
    /// since 27/09/2026: a page of text scrolling costs a thirtieth of its
    /// intra picture, and a floor of a quarter held it at QP 38-45 on a 20
    /// Mbit/s stream; -1 no floor either.
    int interFloor12 = 0;
    /// The priority of the D3D12 queues. The engine's own follows the
    /// process's GPU class: GLOBAL_REALTIME under REALTIME, HIGH otherwise.
    enum class Priority12
    {
        Default,
        Normal,
        High,
        GlobalRealtime
    };
    Priority12 prio12 = Priority12::Default;
    /// A CreatorID of each queue's own (the engine's) rather than the
    /// runtime's shared one, whose priority hardware scheduling ignores.
    Choice ownCreator12 = Choice::Default;
    /// How the capture and the D3D12 read of its surface are ordered: fences
    /// on the GPU both ways (the engine's own), none, or the CPU waiting for
    /// the read before ReleaseFrame. Measured: without it, reads come out
    /// stale or from the next frame.
    enum class DdaSync
    {
        Default,
        None,
        Gpu,
        Cpu
    };
    DdaSync ddaSync = DdaSync::Default;
    /// Timestamps on the D3D12 queues, into EncodedFrame's GPU times. Off by
    /// default: a query pair per pass is not free.
    bool gpuTiming = false;
    /// A session asked to run D3D12 that has to run D3D11 ends instead: a
    /// bench row labelled D3D12 is never a D3D11 one.
    bool strict12 = false;
    /// Two pictures in flight (plan Phase 10): D3D12 Video Encode codes a
    /// picture on a thread of its own while the capture converts the next
    /// into a second output; a converted picture still waiting when a newer
    /// one is ready is dropped, never queued. The engine's own since
    /// 29/09/2026 (§9-26): on an Intel GPU with memory of its own, one
    /// picture at a time elsewhere (pipelinedByDefault, VideoPipelineChoice.h).
    Choice pipelined = Choice::Default;
    /// A capture restart (a mode change, a locked screen, a new desktop)
    /// keeps the D3D12 Video Encode encoder when the stream it codes is the
    /// same — codec, size, rate, HDR — rather than making it again (plan
    /// C11.4): the stream starts over on a keyframe either way. On, the
    /// engine's own since 29/09/2026 (§9-27): the keyframe after a mode
    /// change came 717 → 546 ms sooner on the Arc; keep12=0 makes it again.
    Choice keep12 = Choice::Default;

    // ── The stream's cadence (plan framerate-hote, design §33) ─────────────

    /// Whose rate the stream runs at. The engine's own is the client's: the
    /// setting, under the ceiling "Auto" states and the one a slow decoder
    /// asks for, aligned on a client that paints on vsync (CadenceAlign.h),
    /// the gate keeping the first present of each interval (FrameCadence).
    /// The host ones run at the rate of the host's display, and receive the
    /// client's ceilings without applying them (CadenceChoice.h); Deadline
    /// sends one picture per refresh of the client's screen. Windows only.
    enum class Cadence
    {
        Default,
        /// No gate: every present the capture delivers is encoded.
        Host,
        /// The ceiling gate at the display's own rate: a source presenting
        /// faster than the display refreshes is held to it.
        HostCeiling,
        /// Host, and a present is skipped while the client says its decode
        /// queue holds more than a frame (DecodeCredit.h).
        HostGuarded,
        /// The engine's own until the client says when its screen refreshes
        /// (a `vsyncgrid`); then one picture per refresh, taken as late as it
        /// can still make it there (DeadlineCadence.h).
        Deadline
    };
    Cadence cadence = Cadence::Default;

    // ── The Linux chain (plan pipeline-video-d3d12-v2, Phase 13) ────────────

    /// The conversion in front of VA-API: GL through EGL, or Vulkan on a
    /// compute queue — the split route (§9-17). The engine's own is the
    /// vendor table's (core/LinuxRouteChoice.h): Vulkan compute on AMD since
    /// §9-20 (the portal's buffers too since C13.3 bis), GL elsewhere and
    /// whenever VA-API is asked for by name.
    enum class ConvertLinux
    {
        Default,
        Gl,
        Vulkan
    };
    ConvertLinux convertLinux = ConvertLinux::Default;
    /// The Vulkan conversion's queue priority. The engine's own is HIGH when
    /// the process holds CAP_SYS_NICE, normal otherwise; Normal measures the
    /// route without it on a process that has it.
    enum class PriorityVk
    {
        Default,
        Normal,
        High
    };
    PriorityVk prioVk = PriorityVk::Default;
    /// DMA-BUF asked of the ScreenCast portal (PortalCapture::offerDmabuf,
    /// C13.3 bis). Off asks for shared memory only, what a compositor without
    /// DMA-BUF hands over — measured on one that has it (C13.10). The engine's
    /// own is on, except on GNOME's virtual monitor before GNOME 48, whose
    /// DMA-BUF frames keep trails of the pointer; On asks for it there too,
    /// trails and all, to measure the shared memory's price (plan « attente »).
    Choice portalDmabuf = Choice::Default;
    /// GNOME's own screen cast for the virtual display (MutterScreenCast.h,
    /// plan Idées Punktfunk C2). Off makes it through the portal, as before
    /// C2: a dialog once, and each guest a monitor of its own. The engine's
    /// own is on.
    Choice mutterDirect = Choice::Default;

    // ── The link, on a real session (plan Idées Punktfunk, A0) ─────────────
    //
    // A lab for losses and for what SCTP carries under them. The relay reads
    // these, never an encoder; nothing in the product ever sets one.

    /// Video messages thrown away before SCTP is handed them, per thousand,
    /// on a fixed seed: the losses a client meets, with nothing for SCTP's
    /// congestion control to react to. 0, the product: none.
    int lossPermille = 0;
    /// How many messages in a row each loss takes. 0 or 1: one.
    int lossBurst = 0;
    /// usrsctp's congestion control: 0 RFC 2581, 1 HSTCP, 2 H-TCP, 3 RTCC.
    /// -1, the product: usrsctp's own (RFC 2581).
    int sctpCongestion = -1;
    /// Messages of no use on a channel of their own (id 3), to measure what
    /// SCTP carries: paced at this many kbps, or -1 for as much as SCTP takes
    /// (its backlog kept short). 0, the product: none.
    int floodKbps = 0;
    /// Their size in bytes, 64-16000. 0: 1100, a datagram's worth at
    /// libdatachannel's fixed MTU of 1280.
    int floodBytes = 0;
    /// Their channel: unordered with no retransmission (the FEC channel the
    /// plan wants, the default), or the video channel's own — ordered, given
    /// up on after 500 ms.
    bool floodLikeVideo = false;
    /// POC Ultra U1.1, the transport lab: a synthetic Ultra stream on a channel
    /// of its own (id 5; id 4 is the HID passthrough's). For every video frame
    /// that leaves, this many KiB of incompressible bytes go out with the
    /// frame's stamp, through a FrameSender of their own: the load an intra
    /// codec would put on SCTP, with no codec. 0, the product: none.
    int ultraSynthKb = 0;
    /// Its channel: the video channel's reliability (ordered, given up on after
    /// 500 ms; the default), or unordered with no retransmission.
    bool ultraUnordered = false;
    /// POC Ultra U4: the rate of the PyroWave encoder (enc12=pyrowave) in
    /// Mbit/s; 0, its own default (170, PyroWave's published threshold at
    /// 1080p60). The session's adaptive bitrate does not move it.
    int ultraMbps = 0;
    /// The audio road's pacing (POC Ultra U1.4 ter; plan « Wi-Fi », W4):
    /// its chunks leave at this multiple of the rate it carried over the last
    /// second, never under 50 Mbit/s; 0 sends each frame in one run. -1, the
    /// default: off (a Mac in Wi-Fi dropped more with it, W4).
    double aroadPace = -1;
    /// The audio road's send window, KiB (plan « Wi-Fi », W4): at most this
    /// many bytes sent and not yet acknowledged by the page (`aroadack`). 0,
    /// the default: no window.
    int aroadWindowKb = 0;
    /// The audio road's resend budget, percent of the bytes it first sent over
    /// the last 100 ms (plan « Wi-Fi », W4). 0, the default: 20.
    int aroadBudgetPct = 0;
    /// The audio road's chunk, bytes of the frame per packet (POC Ultra P-B).
    /// 0, the default: 1100, and 1400 with enc12=pyrowave. Up to 1400, LAN
    /// only: with the road's 12-byte head, RTP, the SRTP tag and IPv6, a
    /// 1500-byte MTU still holds it.
    int aroadChunk = 0;
    /// Each video frame's way through the relay, written as a CSV next to the
    /// log when the session ends (plan « Wi-Fi : la vidéo qui attend dans
    /// SCTP », W1). false, the product: nothing kept.
    bool relayLog = false;
    /// The click's way through the host — each press handed to the OS, each
    /// wake-up of the capture with the OS's and the compositor's own stamps —
    /// written as a CSV next to the log when the session ends (plan
    /// « attente », A0; see src/core/ClickTrace.h). false, the product:
    /// nothing kept.
    bool clickTrace = false;
    /// ScreenCaptureKit's pool of surfaces (SCStreamConfiguration.queueDepth,
    /// macOS host; plan « attente », AM2). 0, the product: 3.
    int sckQueueDepth = 0;
    /// The least time between two ScreenCaptureKit frames, µs
    /// (minimumFrameInterval, macOS host; plan « attente », AM2). 0: none at
    /// all. -1, the product: one refresh of the captured display.
    int sckMinIntervalUs = -1;
    /// The relay hands a frame's chunks to SCTP at this many times the
    /// stream's bitrate at most, paceBurstKb at a time (plan Wi-Fi W2 A:
    /// a frame sent in one run overflows the browser's UDP socket on Wi-Fi).
    /// 0, the product: as fast as the sender goes.
    int paceMultiple = 0;
    /// The most that leaves back to back under pacing, in KB. 0: 16, one chunk.
    int paceBurstKb = 0;
    /// The rate governor also cuts when SCTP retransmits at least this many
    /// chunks in a thousand over a report window (plan Wi-Fi W2 B; Windows
    /// host). -1, the product: the engine's own, RateGovernor's
    /// kRetransCutPermille; 0 the governor does not look at SCTP.
    int retransCutPermille = -1;
    /// usrsctp's send buffer, in KB, really (plan Wi-Fi W2 C). 0, the product:
    /// 256 KiB whatever the bitrate, since libdatachannel raises the buffer to
    /// its largest message. What waits in there the host cannot see; what it
    /// cannot take waits in `bufferedAmount`, where linkHoldMs looks.
    int sctpBufferKb = 0;
    /// A picture is held, not encoded, once video has waited outside usrsctp
    /// for this many milliseconds without the queue draining; the freshest
    /// goes once it drained (plan Wi-Fi W2 C; Windows host). A queue turned
    /// into a lower frame rate, with no hole in the references. Not at the
    /// first byte: on Ethernet a frame bigger than usrsctp's room overflows
    /// for a millisecond or two, and holding on that halved the frame rate
    /// (03/10/2026). 0, the product: every picture is encoded.
    int linkHoldMs = 0;
    /// usrsctp's max burst, in packets: the most it sends at one opportunity,
    /// a SACK's arrival among them (plan Wi-Fi W2.5). 0: no limit. -1, the
    /// product: the engine's own, no limit on every native host (Windows and
    /// Linux since 04/10/2026, macOS since 05/10; DataChannelRelay::
    /// kNativeSctpMaxBurst). At libdatachannel's 10 a frame of 20 to 35
    /// packets leaves over 2 to 4 SACK round trips, 8 to 9 ms each on a Mac in
    /// Wi-Fi.
    int sctpMaxBurst = -1;
    /// usrsctp's stream scheduler, which decides whose message goes next when
    /// several streams wait (plan Wi-Fi W2.3): 0 usrsctp's default, 1 round
    /// robin, 2 round robin by packet, 3 priority, 4 fair bandwidth (the
    /// shortest pending message first: the input channel's few hundred bytes
    /// before a 16 KB video chunk), 5 first come. -1, the product: usrsctp's
    /// default, never set. 4 changed nothing for the input channel's round
    /// trip on a Mac in Wi-Fi; 2 made the association with Chrome fail within
    /// seconds, and the key refuses it (04/10/2026).
    int sctpScheduler = -1;
    /// The connection's path MTU in bytes, 1280-1500 (plan Wi-Fi W2.6):
    /// libdatachannel's Configuration::mtu, from which usrsctp's packets are
    /// sized (mtu - 108: SCTP, DTLS, UDP and IPv6 headers) and DTLS fragments
    /// its handshake (mtu - 48). 0, the product: libdatachannel's 1280, the
    /// least IPv6 guarantees, so 1172-byte SCTP packets; 1500 makes them 1392.
    /// The host pays per packet (~12 us each on the audio road, POC Ultra
    /// §6.41), and a path that does not hold the size drops every full packet,
    /// the DTLS handshake's first: only for a LAN known to hold 1500.
    int sctpMtu = 0;
    /// Each audio packet's way through the host — the pacer's tick, its queue,
    /// the frame's peak, the relay thread, the RTP track — written as a CSV
    /// next to the log when the session ends (plan « le son et la priorité des
    /// paquets », A0). false, the product: nothing kept.
    bool audioLog = false;
    /// The Opus frame of the native host's sound, in ms: 5 (240 samples, the
    /// product), 10 or 20 (plan « le son et la priorité des paquets », A3 L2).
    /// A longer frame waits longer before it leaves, but sends a half or a
    /// quarter of the packets, each one a Wi-Fi transmission of its own. The
    /// encoder, the pacer and the relay's RTP clock all follow. 0, the
    /// product: 5 ms.
    int audioFrameMs = 0;

    /// Samples per channel of one audio frame at 48 kHz: 240, 480 or 960.
    int audioFrameSamples() const
    {
        return audioFrameMs == 10 ? 480 : audioFrameMs == 20 ? 960 : 240;
    }

    bool isDefault() const
    {
        return nvencPreset == 0 && nvencTuning == Latency::Default &&
               nvencMultiPass == MultiPass::Default && nvencMinQp == 0 && amfMinQp == 0 &&
               vaapiMinQp == 0 && nvencIntraRefreshPeriod == 0 && nvencIntraRefreshCount == 0 &&
               spatialAq == Choice::Default && temporalAq == Choice::Default &&
               preAnalysis == Choice::Default && amfQuality == AmfQuality::Default &&
               amfLowLatency == Choice::Default && vplTargetUsage == 0 &&
               vplLowPower == Choice::Default && vplMbBrc == Choice::Default &&
               vplExtBrc == Choice::Default && vplLowDelayBrc == Choice::Default &&
               vplGamingScenario == Choice::Default && vplWinBrcFrames == 0 &&
               vplRateControl == VplRateControl::Default && vplIntraRefreshQpDelta == 0 &&
               intraRefreshDist == 0 && linkGovernor == Choice::Default &&
               nameLinkDrops == Choice::Default && vbvFrames == 0 && dpbFrames == 0 &&
               fallback == Fallback::None && pipeline == VideoPipeline::Auto &&
               conv12 == ConvertQueue12::Default && enc12 == Encoder12::Default &&
               rc12 == RateControl12::Default && reencode12 == Choice::Default &&
               reencodeFit12 == Choice::Default && interFloor12 == 0 &&
               prio12 == Priority12::Default && ownCreator12 == Choice::Default &&
               ddaSync == DdaSync::Default && !gpuTiming && !strict12 &&
               pipelined == Choice::Default && keep12 == Choice::Default &&
               cadence == Cadence::Default && convertLinux == ConvertLinux::Default &&
               prioVk == PriorityVk::Default && portalDmabuf == Choice::Default &&
               mutterDirect == Choice::Default && lossPermille == 0 && lossBurst == 0 &&
               sctpCongestion < 0 && floodKbps == 0 && floodBytes == 0 && !floodLikeVideo &&
               ultraSynthKb == 0 && !ultraUnordered && ultraMbps == 0 && aroadPace < 0 &&
               aroadWindowKb == 0 && aroadBudgetPct == 0 && aroadChunk == 0 && !relayLog &&
               paceMultiple == 0 && paceBurstKb == 0 && retransCutPermille < 0 &&
               sctpBufferKb == 0 && linkHoldMs == 0 && sctpMaxBurst < 0 && sctpScheduler < 0 &&
               sctpMtu == 0 && !audioLog && audioFrameMs == 0 && !clickTrace &&
               sckQueueDepth == 0 && sckMinIntervalUs < 0;
    }

    /// One line naming every field that is NOT at its default, for the log and
    /// the bench summary. Empty when nothing is.
    std::string describe() const
    {
        std::string s;
        auto add = [&s](const std::string& item) {
            if (!s.empty()) s += ' ';
            s += item;
        };
        auto choice = [](Choice c) { return c == Choice::On ? "on" : "off"; };
        if (nvencPreset > 0) add("preset=P" + std::to_string(nvencPreset));
        if (nvencTuning == Latency::UltraLow) add("tuning=ULL");
        if (nvencTuning == Latency::Low) add("tuning=LL");
        if (nvencMultiPass == MultiPass::Off) add("multipass=off");
        if (nvencMultiPass == MultiPass::QuarterRes) add("multipass=quarter");
        if (nvencMultiPass == MultiPass::FullRes) add("multipass=full");
        if (nvencMinQp != 0) add("nvminqp=" + std::to_string(nvencMinQp));
        if (amfMinQp != 0) add("amfminqp=" + std::to_string(amfMinQp));
        if (vaapiMinQp != 0) add("vaminqp=" + std::to_string(vaapiMinQp));
        if (nvencIntraRefreshPeriod > 0)
            add("nvirperiod=" + std::to_string(nvencIntraRefreshPeriod));
        if (nvencIntraRefreshCount > 0) add("nvircnt=" + std::to_string(nvencIntraRefreshCount));
        if (spatialAq != Choice::Default) add(std::string("aq=") + choice(spatialAq));
        if (temporalAq != Choice::Default) add(std::string("taq=") + choice(temporalAq));
        if (preAnalysis != Choice::Default) add(std::string("preanalysis=") + choice(preAnalysis));
        if (amfQuality == AmfQuality::Speed) add("quality=speed");
        if (amfQuality == AmfQuality::Balanced) add("quality=balanced");
        if (amfQuality == AmfQuality::Quality) add("quality=quality");
        if (amfLowLatency != Choice::Default)
            add(std::string("lowlatency=") + choice(amfLowLatency));
        if (vplTargetUsage > 0) add("tu=" + std::to_string(vplTargetUsage));
        if (vplLowPower != Choice::Default) add(std::string("lowpower=") + choice(vplLowPower));
        if (vplMbBrc != Choice::Default) add(std::string("mbbrc=") + choice(vplMbBrc));
        if (vplExtBrc != Choice::Default) add(std::string("extbrc=") + choice(vplExtBrc));
        if (vplLowDelayBrc != Choice::Default)
            add(std::string("lowdelaybrc=") + choice(vplLowDelayBrc));
        if (vplGamingScenario != Choice::Default)
            add(std::string("gaming=") + choice(vplGamingScenario));
        if (vplWinBrcFrames > 0) add("winbrc=" + std::to_string(vplWinBrcFrames) + "f");
        if (vplRateControl == VplRateControl::Cbr) add("rc=cbr");
        if (vplRateControl == VplRateControl::Vbr) add("rc=vbr");
        if (vplRateControl == VplRateControl::Qvbr) add("rc=qvbr" + std::to_string(vplQvbrQuality));
        if (vplIntraRefreshQpDelta != 0) add("irqp=" + std::to_string(vplIntraRefreshQpDelta));
        if (intraRefreshDist != 0) add("irdist=" + std::to_string(intraRefreshDist));
        if (linkGovernor != Choice::Default) add(std::string("governor=") + choice(linkGovernor));
        if (nameLinkDrops != Choice::Default)
            add(std::string("namedrops=") + choice(nameLinkDrops));
        if (vbvFrames > 0) add("vbv=" + std::to_string(vbvFrames) + "f");
        if (dpbFrames > 0) add("dpb=" + std::to_string(dpbFrames));
        if (fallback == Fallback::Tier) add("fallback=1");
        if (fallback == Fallback::MediaFoundation) add("fallback=mf");
        if (fallback == Fallback::MediaFoundationSoftware) add("fallback=mfsw");
        if (fallback == Fallback::MediaFoundationCpuInput) add("fallback=mfcpu");
        if (fallback == Fallback::Cpu) add("fallback=cpu");
        if (pipeline != VideoPipeline::Auto) add(std::string("pipeline=") + toString(pipeline));
        if (conv12 == ConvertQueue12::Direct) add("conv12=direct");
        if (conv12 == ConvertQueue12::Compute) add("conv12=compute");
        if (enc12 == Encoder12::VideoEncode) add("enc12=ve");
        if (enc12 == Encoder12::Nvenc) add("enc12=nvenc");
        if (enc12 == Encoder12::Amf) add("enc12=amf");
        if (enc12 == Encoder12::Pyrowave) add("enc12=pyrowave");
        if (rc12 == RateControl12::Driver) add("rc12=driver");
        if (rc12 == RateControl12::Qp) add("rc12=qp");
        if (reencode12 != Choice::Default) add(std::string("reencode=") + choice(reencode12));
        if (reencodeFit12 != Choice::Default) add(std::string("refit=") + choice(reencodeFit12));
        if (interFloor12 < 0) add("interfloor=off");
        if (interFloor12 > 0) add("interfloor=" + std::to_string(interFloor12));
        if (prio12 == Priority12::Normal) add("prio12=normal");
        if (prio12 == Priority12::High) add("prio12=high");
        if (prio12 == Priority12::GlobalRealtime) add("prio12=realtime");
        if (ownCreator12 == Choice::On) add("creator12=own");
        if (ownCreator12 == Choice::Off) add("creator12=default");
        if (ddaSync == DdaSync::None) add("ddasync=none");
        if (ddaSync == DdaSync::Gpu) add("ddasync=gpu");
        if (ddaSync == DdaSync::Cpu) add("ddasync=cpu");
        if (gpuTiming) add("gputiming=1");
        if (strict12) add("strict12=1");
        if (pipelined != Choice::Default) add(std::string("pipelined=") + choice(pipelined));
        if (keep12 != Choice::Default) add(std::string("keep12=") + choice(keep12));
        if (cadence == Cadence::Host) add("cadence=host");
        if (cadence == Cadence::HostCeiling) add("cadence=host-ceiling");
        if (cadence == Cadence::HostGuarded) add("cadence=host-guarded");
        if (cadence == Cadence::Deadline) add("cadence=deadline");
        if (convertLinux == ConvertLinux::Gl) add("convert=gl");
        if (convertLinux == ConvertLinux::Vulkan) add("convert=vulkan");
        if (prioVk == PriorityVk::Normal) add("priovk=normal");
        if (prioVk == PriorityVk::High) add("priovk=high");
        if (portalDmabuf != Choice::Default)
            add(std::string("portaldmabuf=") + choice(portalDmabuf));
        if (mutterDirect != Choice::Default) add(std::string("mutter=") + choice(mutterDirect));
        if (lossPermille > 0) add("loss=" + std::to_string(lossPermille));
        if (lossBurst > 1) add("burst=" + std::to_string(lossBurst));
        if (sctpCongestion >= 0) add("sctpcc=" + std::to_string(sctpCongestion));
        if (floodKbps > 0) add("flood=" + std::to_string(floodKbps));
        if (floodKbps < 0) add("flood=max");
        if (floodBytes > 0) add("floodsize=" + std::to_string(floodBytes));
        if (floodLikeVideo) add("floodchannel=video");
        if (ultraSynthKb > 0) add("ultra=synthetic:" + std::to_string(ultraSynthKb));
        if (ultraMbps > 0) add("ultrambps=" + std::to_string(ultraMbps));
        if (aroadPace >= 0) {
            char pace[32];
            std::snprintf(pace, sizeof pace, "aroadpace=%g", aroadPace);
            add(pace);
        }
        if (aroadWindowKb > 0) add("aroadwin=" + std::to_string(aroadWindowKb));
        if (aroadBudgetPct > 0) add("aroadbudget=" + std::to_string(aroadBudgetPct));
        if (aroadChunk > 0) add("aroadchunk=" + std::to_string(aroadChunk));
        if (ultraUnordered) add("ultrachannel=unordered");
        if (relayLog) add("relaylog=1");
        if (paceMultiple > 0) add("pace=" + std::to_string(paceMultiple));
        if (paceBurstKb > 0) add("paceburst=" + std::to_string(paceBurstKb));
        if (retransCutPermille >= 0) add("retrcut=" + std::to_string(retransCutPermille));
        if (sctpBufferKb > 0) add("sctpbuf=" + std::to_string(sctpBufferKb));
        if (linkHoldMs > 0) add("linkhold=" + std::to_string(linkHoldMs));
        if (sctpMaxBurst >= 0) add("sctpburst=" + std::to_string(sctpMaxBurst));
        if (sctpScheduler >= 0) add("sctpss=" + std::to_string(sctpScheduler));
        if (sctpMtu > 0) add("sctpmtu=" + std::to_string(sctpMtu));
        if (audioLog) add("audiolog=1");
        if (audioFrameMs > 0) add("audioframe=" + std::to_string(audioFrameMs));
        if (clickTrace) add("clicktrace=1");
        if (sckQueueDepth > 0) add("sckdepth=" + std::to_string(sckQueueDepth));
        if (sckMinIntervalUs >= 0) add("sckinterval=" + std::to_string(sckMinIntervalUs));
        return s;
    }
};

/// EncoderTuning::nameLinkDrops when the key says nothing, for a session that
/// streams through @p pipeline with @p encoder — and, on the D3D12 route, the
/// encoder that route runs, @p encoder12: on for NVENC and AMF through D3D11
/// and for NVENC fed D3D12 pictures, off everywhere else (Bruno, 29/09/2026,
/// plan §9-25). On a link that stalled, the pictures a Chrome client showed
/// damaged fell by 85 to 93 % on NVENC, by 87 % on NVENC fed D3D12 pictures and
/// by 46 to 96 % on AMF, for the same freezes (bench §8n.27 to §8n.29). Never
/// oneVPL: it hung under it, three passes out of three. D3D12 Video Encode
/// gained nothing measurable, nor did AMF fed D3D12 pictures, which only a
/// bench key runs.
inline bool nameLinkDropsByDefault(VideoPipeline pipeline, EncoderApi encoder,
                                   EncoderTuning::Encoder12 encoder12)
{
    if (pipeline == VideoPipeline::D3d11)
        return encoder == EncoderApi::Nvenc || encoder == EncoderApi::Amf;
    if (pipeline == VideoPipeline::D3d12) return encoder12 == EncoderTuning::Encoder12::Nvenc;
    return false;
}

/// A client whose decoder falls silent under the encoder's reference repairs
/// asks for none (/start's `ref_invalidation`: false), and gets the bench's
/// "before" on every encoder: one reference, a keyframe for a loss. Seen on a
/// Freebox Player POP (Amlogic, 01/10/2026): AMF marked a long-term reference
/// every frame at 30 fps, and the box's decoder gave no picture back every
/// 2.5 s; with dpb=1, 25 fps steady. Nothing changes for any other client,
/// and a bench's own dpb= is left as it set it.
/// @returns whether the tuning changed
inline bool refuseReferenceRepairs(EncoderTuning& tuning)
{
    if (tuning.dpbFrames != 0) return false;
    tuning.dpbFrames = 1;
    return true;
}

} // namespace mw::native
