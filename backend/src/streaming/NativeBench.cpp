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

#include "NativeBench.h"

#include "mw/native/NativeHost.h"
#include "mw/native/StageStats.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct BenchSpec
{
    int display = -1;
    int seconds = 10;
    QString out;
    mw::native::Codec codec = mw::native::Codec::Hevc;
    int fps = 0;
    int bitrateKbps = 20000;
    int width = 0;
    int height = 0;
    bool yuv444 = false;
    bool hdr = false;
    bool intraRefresh = false;
    /// intra=2: the stream must refresh by intra-refresh, whatever the route
    /// (SessionConfig::intraRefreshRequired).
    bool intraRefreshRequired = false;
    /// The GPU to encode on, -1 for the display's own. See SessionConfig.
    int gpu = -1;
    /// The encoder knobs under test; default = the engine's own choices.
    mw::native::EncoderTuning tuning;
    /// The encoded stream, written as it comes; empty = not kept.
    QString dump;
    /// Every this many frames, the latest one is reported lost (0 = never).
    int loseEvery = 0;
    /// How many frames in a row each loss takes, each reported as it comes out:
    /// what the relay does when a stalled link makes it drop frame after frame,
    /// the repair frames included.
    int loseBurst = 1;
    /// Each burst closed by a keyframe request, as the relay's is when the link
    /// drains again ("keyframe requested at once"): the next picture is then
    /// asked for as a keyframe while the last loss's repair is still pending.
    bool loseKeyframe = false;
    /// The target alternates between bitrateKbps and this, every rampSeconds.
    int rampKbps = 0;
    double rampSeconds = 2.0;
};

/// What one frame cost, copied out of the callback. The bytes themselves are
/// not kept: this is a sink, and the point is that nothing downstream of the
/// encoder is being measured here.
struct BenchRow
{
    uint32_t frameNumber = 0;
    bool keyframe = false;
    /// False for a re-send of a still picture (no capture happened).
    bool captured = false;
    size_t bytes = 0;
    int avgQp = -1;
    int64_t presentUs = 0;
    int64_t capturedUs = 0;
    int64_t submittedUs = 0;
    int64_t convertedUs = 0;
    int64_t encodedUs = 0;
    /// GPU time of the conversion and the encode, -1 when not measured.
    int64_t gpuConvertUs = -1;
    int64_t gpuEncodeUs = -1;
    /// The target the bench had set when the frame came out (ramp=).
    int targetKbps = 0;
    /// The rate the encoder held for it (EncodedFrame::encoderKbps), 0 when
    /// the platform does not say.
    int encoderKbps = 0;
    /// The chain that made it: a D3D12 session that failed while streaming
    /// goes on in D3D11 (MW_D3D12_FAULT puts that on the bench).
    mw::native::VideoPipeline pipeline = mw::native::VideoPipeline::Auto;
};

const char* const kUsage =
    "usage: --native-bench <key=value,...>\n"
    "  display=<id>     the display to capture (omit to list them)\n"
    "  seconds=<n>      how long to run (default 10)\n"
    "  codec=hevc|h264|av1   (default hevc)\n"
    "  fps=<n>          stream frame rate, 0 = the display's own (default 0)\n"
    "  bitrate=<kbps>   (default 20000)\n"
    "  width=<px>,height=<px>   output size, 0 = the display's (default 0)\n"
    "  yuv444=0|1       (default 0)\n"
    "  hdr=0|1          capture FP16 scRGB and encode BT.2020 PQ 10-bit (default 0).\n"
    "                   Needs Windows HDR ON for that display and HEVC or AV1;\n"
    "                   the session says so and runs SDR when it is not there.\n"
    "  intra=0|1|2      intra-refresh instead of keyframes (default 0); 2 = required, as\n"
    "                   the guests' shared feed has it: a route that grants none gives way\n"
    "  gpu=<id>         encode on this GPU instead of the display's own (cross-GPU copy)\n"
    "  out=<path.csv>   one row per frame (default native-bench-<time>.csv here)\n"
    "encoder knobs, each defaulting to the engine's own choice:\n"
    "  preset=1..7      NVENC P1 (fastest) .. P7 (best)\n"
    "  tuning=ull|ll    NVENC latency tuning\n"
    "  multipass=off|quarter|full   NVENC two-pass rate control\n"
    "  nvminqp=<qp>     NVENC QP floor (AV1: qindex 1..255); -1 = none, default 18 / AV1 32\n"
    "  nvirperiod=<frames>  NVENC frames between intra-refresh sweep starts (with intra=1)\n"
    "  nvircnt=<frames>     NVENC frames one sweep takes (with intra=1)\n"
    "  aq=0|1           spatial adaptive quantization (NVENC AQ, AMF VBAQ/CAQ)\n"
    "  taq=0|1          NVENC temporal AQ\n"
    "  preanalysis=0|1  AMF pre-analysis\n"
    "  quality=speed|balanced|quality   AMF quality preset\n"
    "  lowlatency=0|1   AMF internal low-latency mode (H.264/HEVC)\n"
    "  amfminqp=<qp>    AMF QP floor (H.264/HEVC); -1 = none, default 18\n"
    "  vaminqp=<qp>     VA-API QP floor (H.264/HEVC); -1 = none, default 18\n"
    "  tu=1..7          oneVPL TargetUsage (1 quality .. 7 speed)\n"
    "  lowpower=0|1     oneVPL fixed-function engine (VDENC); engine's own is on\n"
    "  mbbrc=0|1        oneVPL macroblock-level rate control\n"
    "  extbrc=0|1       oneVPL alternative bitrate controller\n"
    "  lowdelaybrc=0|1  oneVPL low-delay mode of the bitrate controller\n"
    "  gaming=0|1       oneVPL ScenarioInfo = remote gaming\n"
    "  winbrc=<frames>  oneVPL sliding-window rate cap, in frames\n"
    "  rc=cbr|vbr|qvbr<q>   oneVPL bitrate controller, same cap and buffer (qvbr26 = quality 26)\n"
    "  irqp=<delta>     oneVPL QP offset of the intra-refresh band (with intra=1)\n"
    "  irdist=<frames>  oneVPL, Vulkan Video and VA-API: frames between intra-refresh sweep\n"
    "                   starts (with intra=1); -1 = back to back, default = four periods\n"
    "  vbv=<frames>     VBV of exactly N frames at the stream rate, no floor\n"
    "  governor=0|1     the link governor; 0: the encoder gets bitrate= (and ramp=) as\n"
    "                   they are — no receiver here to report on the link\n"
    "  namedrops=0|1    a real session's (MW_NATIVE_TUNING): the delta the relay drops on a\n"
    "                   stalled link is named to the encoder rather than healed by a keyframe;\n"
    "                   default 1 for NVENC (D3D11 or D3D12 pictures) and AMF through\n"
    "                   D3D11, 0 elsewhere\n"
    "  dpb=<frames>     NVENC decoded picture buffer (default 4, for reference invalidation)\n"
    "  fallback=1|mf|mfsw|mfcpu|cpu  pretend no GPU encodes: the fallback tier (1), Media\n"
    "                   Foundation (mf), Microsoft's software transform even where a hardware\n"
    "                   one exists (mfsw), the hardware transform fed through system memory\n"
    "                   (mfcpu), or OpenH264 (cpu) — how they get measured beside NVENC\n"
    "the picture chain (Windows), each defaulting to the engine's own choice:\n"
    "  pipeline=auto|d3d11|d3d12   over the setting and the vendor table\n"
    "  pipelined=0|1               D3D12 Video Encode on a thread of its own, the next\n"
    "                              picture converted meanwhile (two in flight at most,\n"
    "                              a waiting one dropped for a newer one); default 1 on\n"
    "                              an Intel GPU with memory of its own, 0 elsewhere\n"
    "  keep12=0|1                 a capture restart keeps the D3D12 Video Encode encoder\n"
    "                              when the stream it codes is the same (default 1)\n"
    "  conv12=direct|compute       the D3D12 conversion's queue\n"
    "  enc12=ve|nvenc|amf|pyrowave the D3D12 route's encoder; pyrowave = POC Ultra\n"
    "  ultrambps=<Mbit/s>          the PyroWave encoder's rate (default 170)\n"
    "  aroadpace=<x>               the audio road's chunks paced at x times what it carried,\n"
    "                              50 Mbit/s at least; 0 = a frame in one run (default 0)\n"
    "  aroadwin=<KiB>              the audio road's send window: at most this much sent and\n"
    "                              not yet acknowledged by the page (default: none)\n"
    "  aroadbudget=<%>             the audio road's resends: at most this share of what it\n"
    "                              first sent over the last 100 ms (default 20)\n"
    "  aroadchunk=<bytes>          the audio road's chunk, 256 to 1400 bytes of the frame per\n"
    "                              packet (default 1100, 1400 with enc12=pyrowave); over 1100\n"
    "                              for the LAN only\n"
    "  rc12=driver|qp             D3D12 Video Encode's rate control; qp = the in-house one\n"
    "  reencode=0|1                in-house rate control: a picture far over its budget is\n"
    "                              coded again, at the QP that fits it (1); 0 sends it as is\n"
    "  refit=0|1                   how: at two budgets by the slope learned, then the\n"
    "                              textbook (1, the default), or at its budget by the\n"
    "                              textbook's slope (0)\n"
    "  interfloor=<k>|off          in-house rate control: a new picture is never believed\n"
    "                              to cost under 1/2^k of an intra one (off: no floor)\n"
    "  prio12=normal|high|realtime the D3D12 queues' priority (default: the GPU class's)\n"
    "  creator12=own|default       each queue's own CreatorID, or the runtime's shared one\n"
    "  ddasync=gpu|none|cpu        how the capture and the D3D12 read are ordered\n"
    "  gputiming=0|1               GPU times per frame (gpu_convert_us, gpu_encode_us)\n"
    "  strict12=0|1                end rather than run D3D11 when D3D12 was asked for\n"
    "  cadence=client|host|host-ceiling|host-guarded|deadline\n"
    "                              whose rate the stream runs at: the client's (the\n"
    "                              default), or the host display's — every present\n"
    "                              (host), held to the display's refresh (host-ceiling),\n"
    "                              or skipped while the client's decode queue is over a\n"
    "                              frame (host-guarded); the client's ceilings are logged,\n"
    "                              not applied. deadline: one picture per refresh of the\n"
    "                              client's screen, taken as late as it can make it\n"
    "  (in the environment, MW_D3D12_FAULT=open|convert|timeout|removed|encode[@N]: that fault\n"
    "  at the Nth D3D12 open or conversion — without strict12, to watch the way back to D3D11)\n"
    "the picture chain (Linux), each defaulting to the engine's own choice:\n"
    "  pipeline=auto|vaapi|vulkan  over the setting and the vendor table: vaapi is the chain\n"
    "                              without Vulkan (GL, then VA-API), vulkan the Vulkan Video\n"
    "                              one, taken once its pixel proof passes\n"
    "  convert=gl|vulkan           the conversion in front of VA-API: GL, or Vulkan compute\n"
    "                              (the split route)\n"
    "  priovk=normal|high          the Vulkan queues' priority (default: high with\n"
    "                              CAP_SYS_NICE, normal without)\n"
    "  portaldmabuf=0|1            the portal asked for DMA-BUF (default 1); 0: shared memory\n"
    "                              only, what a compositor without DMA-BUF hands over; 1 asks\n"
    "                              for it on GNOME's virtual monitor before GNOME 48 too,\n"
    "                              whose frames then keep trails of the pointer\n"
    "  mutter=0|1                  the virtual display through GNOME's own screen cast\n"
    "                              (default 1); 0: through the portal, as before C2\n"
    "  (in the environment, MW_PORTAL_RESTORE_TOKEN=<grant>: the portal's consent replayed,\n"
    "  on a binary that cannot read the scanout; a new grant is printed as \"portal grant:\")\n"
    "the link, a real session's only (MW_NATIVE_TUNING or native_tuning; plan Idees\n"
    "Punktfunk, A0), each off by default:\n"
    "  loss=<permille>   video messages thrown away before SCTP gets them, on a fixed seed\n"
    "  burst=<n>         each loss takes n messages in a row (with loss=)\n"
    "  sctpcc=0..3       usrsctp's congestion control: RFC 2581, HSTCP, H-TCP, RTCC\n"
    "  flood=<kbps>|max  messages of no use on channel id 3, paced, or as much as SCTP\n"
    "                    takes; the client counts them when its localStorage has mw_flood\n"
    "  floodsize=<bytes> their size, 64-16000 (default 1100)\n"
    "  floodchannel=fec|video   unordered with no retransmission (default), or the video\n"
    "                    channel's own: ordered, given up on after 500 ms\n"
    "  ultra=synthetic:<KiB>   POC Ultra U1.1: per video frame sent, that many KiB of\n"
    "                    incompressible bytes on channel id 5 (1-8192); the client\n"
    "                    times them when its localStorage has mw_ultra_sink=1\n"
    "  ultrachannel=video|unordered   their channel: the video's reliability (default),\n"
    "                    or unordered with no retransmission\n"
    "  relaylog=0|1     each video frame's way through the relay, a CSV next to the log\n"
    "                    when the session ends (plan Wi-Fi W1)\n"
    "  pace=<n>          a frame's chunks handed to SCTP at n times the stream's bitrate\n"
    "                    at most, so a frame no longer leaves in one run (plan Wi-Fi W2 A)\n"
    "  paceburst=<KB>    what leaves back to back under pace= (default 16, one chunk)\n"
    "  retrcut=<permille>  the rate governor also cuts when SCTP retransmits at least that\n"
    "                    many chunks in a thousand (Windows host: 3 unless said, 0 off;\n"
    "                    plan Wi-Fi W2 B)\n"
    "  sctpbuf=<KB>      usrsctp's send buffer, 24-1024, really: also the largest message\n"
    "                    either side may send (256 KB otherwise; plan Wi-Fi W2 C)\n"
    "  sctpss=0|1|3|4|5  usrsctp's stream scheduler: default, round robin, priority, fair\n"
    "                    bandwidth (shortest message first), first come (plan Wi-Fi W2.3);\n"
    "                    not 2, round robin by packet: Chrome's SCTP fails within seconds\n"
    "  sctpburst=<n>     usrsctp's max burst, in packets; 0 no limit (every native\n"
    "                    host's own; 10 libdatachannel's; plan Wi-Fi W2.5)\n"
    "  linkhold=<ms>     a picture held, not encoded, once video has waited outside usrsctp\n"
    "                    that long, 1-100; the freshest goes once it drained (Windows host;\n"
    "                    with sctpbuf=)\n"
    "  audiolog=0|1      each audio packet's way through the host (pacer tick and queue,\n"
    "                    peak, relay thread, RTP track), a CSV next to the log when the\n"
    "                    session ends (plan audio + DSCP, A0)\n"
    "  audioframe=5|10|20  the Opus frame of the host's sound, in ms: 5 the product; 10 or\n"
    "                    20 send a half or a quarter of the packets, and wait that much\n"
    "                    longer before each (plan audio + DSCP, A3 L2)\n"
    "  clicktrace=0|1    the click's way through the host: each press handed to the OS,\n"
    "                    each wake-up of the capture with the OS's and the compositor's\n"
    "                    stamps, a CSV next to the log when the session ends (Windows\n"
    "                    and macOS hosts; plan attente, A0 and AM0)\n"
    "  sckdepth=<n>      ScreenCaptureKit's pool of surfaces (queueDepth), 1-8; 3 the\n"
    "                    product (macOS host; plan attente, AM2)\n"
    "  sckinterval=<us>  the least time between two ScreenCaptureKit frames, 0-100000;\n"
    "                    0 none, one display refresh the product (macOS host; AM2)\n"
    "the bench's own:\n"
    "  dump=<path>      the encoded stream as it comes out (Annex-B, or OBUs for AV1)\n"
    "  lose=<frames>[x<burst>][k]  every N frames, report the latest one lost (reference\n"
    "                   invalidation); with x<burst>, that many frames in a row, each\n"
    "                   reported as it comes out, as a stalled link's drops are; with k,\n"
    "                   each burst closed by a keyframe request, as when the link drains\n"
    "  ramp=<kbps>[@<s>]  the target alternates between bitrate= and <kbps> every <s> s\n"
    "                   (default 2), as the rate governor would move it\n";

bool parseChoice(const QString& value, mw::native::EncoderTuning::Choice& out)
{
    if (value == "0" || value.compare("off", Qt::CaseInsensitive) == 0) {
        out = mw::native::EncoderTuning::Choice::Off;
        return true;
    }
    if (value == "1" || value.compare("on", Qt::CaseInsensitive) == 0) {
        out = mw::native::EncoderTuning::Choice::On;
        return true;
    }
    return false;
}

/// One encoder knob, `key=value`. Returns false when the key is not a knob at
/// all (so the caller can try its own keys), and sets @p ok false on a bad
/// value for a knob it does know.
bool applyTuningKey(const QString& key, const QString& value, mw::native::EncoderTuning& tuning,
                    int& gpu, bool& ok)
{
    ok = true;
    if (key == "gpu")
        gpu = value.toInt(&ok);
    else if (key == "preset") {
        const int p = value.toInt(&ok);
        ok = ok && p >= 1 && p <= 7;
        tuning.nvencPreset = p;
    } else if (key == "tu") {
        const int tu = value.toInt(&ok);
        ok = ok && tu >= 1 && tu <= 7;
        tuning.vplTargetUsage = tu;
    } else if (key == "vbv") {
        const int frames = value.toInt(&ok);
        ok = ok && frames >= 1 && frames <= 16;
        tuning.vbvFrames = frames;
    } else if (key == "dpb") {
        const int frames = value.toInt(&ok);
        ok = ok && frames >= 1 && frames <= 16;
        tuning.dpbFrames = frames;
    } else if (key == "winbrc") {
        const int frames = value.toInt(&ok);
        ok = ok && frames >= 1 && frames <= 600;
        tuning.vplWinBrcFrames = frames;
    } else if (key == "rc") {
        using Rc = mw::native::EncoderTuning::VplRateControl;
        const QString rc = value.toLower();
        if (rc == "cbr")
            tuning.vplRateControl = Rc::Cbr;
        else if (rc == "vbr")
            tuning.vplRateControl = Rc::Vbr;
        else if (rc.startsWith("qvbr")) {
            // qvbr<q>: the quality rides along, "rc=qvbr26".
            tuning.vplRateControl = Rc::Qvbr;
            tuning.vplQvbrQuality = rc.mid(4).toInt(&ok);
            ok = ok && tuning.vplQvbrQuality >= 1 && tuning.vplQvbrQuality <= 51;
        } else
            ok = false;
    } else if (key == "nvminqp") {
        tuning.nvencMinQp = value.toInt(&ok);
        ok =
            ok && (tuning.nvencMinQp == -1 || (tuning.nvencMinQp >= 1 && tuning.nvencMinQp <= 255));
    } else if (key == "amfminqp") {
        tuning.amfMinQp = value.toInt(&ok);
        ok = ok && (tuning.amfMinQp == -1 || (tuning.amfMinQp >= 1 && tuning.amfMinQp <= 51));
    } else if (key == "vaminqp") {
        tuning.vaapiMinQp = value.toInt(&ok);
        ok = ok && (tuning.vaapiMinQp == -1 || (tuning.vaapiMinQp >= 1 && tuning.vaapiMinQp <= 51));
    } else if (key == "nvirperiod") {
        tuning.nvencIntraRefreshPeriod = value.toInt(&ok);
        ok = ok && tuning.nvencIntraRefreshPeriod >= 1 && tuning.nvencIntraRefreshPeriod <= 3600;
    } else if (key == "nvircnt") {
        tuning.nvencIntraRefreshCount = value.toInt(&ok);
        ok = ok && tuning.nvencIntraRefreshCount >= 1 && tuning.nvencIntraRefreshCount <= 3600;
    } else if (key == "irqp") {
        tuning.vplIntraRefreshQpDelta = value.toInt(&ok);
        ok = ok && tuning.vplIntraRefreshQpDelta >= -51 && tuning.vplIntraRefreshQpDelta <= 51;
    } else if (key == "irdist") {
        tuning.intraRefreshDist = value.toInt(&ok);
        ok = ok && (tuning.intraRefreshDist == -1 ||
                    (tuning.intraRefreshDist >= 1 && tuning.intraRefreshDist <= 3600));
    } else if (key == "fallback") {
        using Fallback = mw::native::EncoderTuning::Fallback;
        if (value == "1" || value.compare("tier", Qt::CaseInsensitive) == 0)
            tuning.fallback = Fallback::Tier;
        else if (value.compare("mf", Qt::CaseInsensitive) == 0)
            tuning.fallback = Fallback::MediaFoundation;
        else if (value.compare("mfsw", Qt::CaseInsensitive) == 0)
            tuning.fallback = Fallback::MediaFoundationSoftware;
        else if (value.compare("mfcpu", Qt::CaseInsensitive) == 0)
            tuning.fallback = Fallback::MediaFoundationCpuInput;
        else if (value.compare("cpu", Qt::CaseInsensitive) == 0)
            tuning.fallback = Fallback::Cpu;
        else
            ok = false;
    } else if (key == "lowpower")
        ok = parseChoice(value, tuning.vplLowPower);
    else if (key == "mbbrc")
        ok = parseChoice(value, tuning.vplMbBrc);
    else if (key == "extbrc")
        ok = parseChoice(value, tuning.vplExtBrc);
    else if (key == "lowdelaybrc")
        ok = parseChoice(value, tuning.vplLowDelayBrc);
    else if (key == "gaming")
        ok = parseChoice(value, tuning.vplGamingScenario);
    else if (key == "governor")
        ok = parseChoice(value, tuning.linkGovernor);
    else if (key == "namedrops")
        ok = parseChoice(value, tuning.nameLinkDrops);
    else if (key == "aq")
        ok = parseChoice(value, tuning.spatialAq);
    else if (key == "taq")
        ok = parseChoice(value, tuning.temporalAq);
    else if (key == "preanalysis")
        ok = parseChoice(value, tuning.preAnalysis);
    else if (key == "lowlatency")
        ok = parseChoice(value, tuning.amfLowLatency);
    else if (key == "tuning") {
        const QString t = value.toLower();
        if (t == "ull")
            tuning.nvencTuning = mw::native::EncoderTuning::Latency::UltraLow;
        else if (t == "ll")
            tuning.nvencTuning = mw::native::EncoderTuning::Latency::Low;
        else
            ok = false;
    } else if (key == "multipass") {
        const QString m = value.toLower();
        if (m == "off" || m == "0")
            tuning.nvencMultiPass = mw::native::EncoderTuning::MultiPass::Off;
        else if (m == "quarter")
            tuning.nvencMultiPass = mw::native::EncoderTuning::MultiPass::QuarterRes;
        else if (m == "full")
            tuning.nvencMultiPass = mw::native::EncoderTuning::MultiPass::FullRes;
        else
            ok = false;
    } else if (key == "quality") {
        const QString q = value.toLower();
        if (q == "speed")
            tuning.amfQuality = mw::native::EncoderTuning::AmfQuality::Speed;
        else if (q == "balanced")
            tuning.amfQuality = mw::native::EncoderTuning::AmfQuality::Balanced;
        else if (q == "quality")
            tuning.amfQuality = mw::native::EncoderTuning::AmfQuality::Quality;
        else
            ok = false;
    } else if (key == "pipeline") {
        ok = mw::native::parseVideoPipeline(value.toStdString(), tuning.pipeline);
    } else if (key == "conv12") {
        using Q = mw::native::EncoderTuning::ConvertQueue12;
        const QString q = value.toLower();
        if (q == "direct")
            tuning.conv12 = Q::Direct;
        else if (q == "compute")
            tuning.conv12 = Q::Compute;
        else
            ok = false;
    } else if (key == "enc12") {
        using E = mw::native::EncoderTuning::Encoder12;
        const QString e = value.toLower();
        if (e == "ve")
            tuning.enc12 = E::VideoEncode;
        else if (e == "nvenc")
            tuning.enc12 = E::Nvenc;
        else if (e == "amf")
            tuning.enc12 = E::Amf;
        else if (e == "pyrowave")
            tuning.enc12 = E::Pyrowave;
        else
            ok = false;
    } else if (key == "rc12") {
        using R = mw::native::EncoderTuning::RateControl12;
        const QString r = value.toLower();
        if (r == "driver")
            tuning.rc12 = R::Driver;
        else if (r == "qp")
            tuning.rc12 = R::Qp;
        else
            ok = false;
    } else if (key == "reencode") {
        ok = parseChoice(value, tuning.reencode12);
    } else if (key == "refit") {
        ok = parseChoice(value, tuning.reencodeFit12);
    } else if (key == "interfloor") {
        if (value.compare("off", Qt::CaseInsensitive) == 0) {
            tuning.interFloor12 = -1;
        } else {
            tuning.interFloor12 = value.toInt(&ok);
            ok = ok && tuning.interFloor12 >= 1 && tuning.interFloor12 <= 8;
        }
    } else if (key == "prio12") {
        using P = mw::native::EncoderTuning::Priority12;
        const QString p = value.toLower();
        if (p == "normal")
            tuning.prio12 = P::Normal;
        else if (p == "high")
            tuning.prio12 = P::High;
        else if (p == "realtime")
            tuning.prio12 = P::GlobalRealtime;
        else
            ok = false;
    } else if (key == "creator12") {
        const QString c = value.toLower();
        if (c == "own")
            tuning.ownCreator12 = mw::native::EncoderTuning::Choice::On;
        else if (c == "default")
            tuning.ownCreator12 = mw::native::EncoderTuning::Choice::Off;
        else
            ok = false;
    } else if (key == "ddasync") {
        using S = mw::native::EncoderTuning::DdaSync;
        const QString s = value.toLower();
        if (s == "gpu")
            tuning.ddaSync = S::Gpu;
        else if (s == "none")
            tuning.ddaSync = S::None;
        else if (s == "cpu")
            tuning.ddaSync = S::Cpu;
        else
            ok = false;
    } else if (key == "gputiming") {
        tuning.gpuTiming = value.toInt(&ok) != 0;
    } else if (key == "strict12") {
        tuning.strict12 = value.toInt(&ok) != 0;
    } else if (key == "pipelined") {
        ok = parseChoice(value, tuning.pipelined);
    } else if (key == "keep12") {
        ok = parseChoice(value, tuning.keep12);
    } else if (key == "cadence") {
        using C = mw::native::EncoderTuning::Cadence;
        const QString c = value.toLower();
        if (c == "client")
            tuning.cadence = C::Default;
        else if (c == "host")
            tuning.cadence = C::Host;
        else if (c == "host-ceiling")
            tuning.cadence = C::HostCeiling;
        else if (c == "host-guarded")
            tuning.cadence = C::HostGuarded;
        else if (c == "deadline")
            tuning.cadence = C::Deadline;
        else
            ok = false;
    } else if (key == "convert") {
        using C = mw::native::EncoderTuning::ConvertLinux;
        const QString c = value.toLower();
        if (c == "gl")
            tuning.convertLinux = C::Gl;
        else if (c == "vulkan")
            tuning.convertLinux = C::Vulkan;
        else
            ok = false;
    } else if (key == "priovk") {
        using P = mw::native::EncoderTuning::PriorityVk;
        const QString p = value.toLower();
        if (p == "normal")
            tuning.prioVk = P::Normal;
        else if (p == "high")
            tuning.prioVk = P::High;
        else
            ok = false;
    } else if (key == "portaldmabuf") {
        ok = parseChoice(value, tuning.portalDmabuf);
    } else if (key == "mutter") {
        ok = parseChoice(value, tuning.mutterDirect);
    } else if (key == "loss") {
        tuning.lossPermille = value.toInt(&ok);
        ok = ok && tuning.lossPermille >= 0 && tuning.lossPermille <= 1000;
    } else if (key == "burst") {
        tuning.lossBurst = value.toInt(&ok);
        ok = ok && tuning.lossBurst >= 1 && tuning.lossBurst <= 64;
    } else if (key == "sctpcc") {
        tuning.sctpCongestion = value.toInt(&ok);
        ok = ok && tuning.sctpCongestion >= 0 && tuning.sctpCongestion <= 3;
    } else if (key == "flood") {
        if (value.compare("max", Qt::CaseInsensitive) == 0) {
            tuning.floodKbps = -1;
        } else {
            tuning.floodKbps = value.toInt(&ok);
            ok = ok && tuning.floodKbps >= 0 && tuning.floodKbps <= 2'000'000;
        }
    } else if (key == "floodsize") {
        tuning.floodBytes = value.toInt(&ok);
        ok = ok && tuning.floodBytes >= 64 && tuning.floodBytes <= 16000;
    } else if (key == "floodchannel") {
        const QString c = value.toLower();
        if (c == "fec")
            tuning.floodLikeVideo = false;
        else if (c == "video")
            tuning.floodLikeVideo = true;
        else
            ok = false;
    } else if (key == "ultra") {
        const QString v = value.toLower();
        ok = v.startsWith(QLatin1String("synthetic:"));
        if (ok) {
            tuning.ultraSynthKb = v.mid(10).toInt(&ok);
            ok = ok && tuning.ultraSynthKb >= 1 && tuning.ultraSynthKb <= 8192;
        }
    } else if (key == "aroadpace") {
        tuning.aroadPace = value.toDouble(&ok);
        ok = ok && tuning.aroadPace >= 0 && tuning.aroadPace <= 20;
    } else if (key == "aroadwin") {
        tuning.aroadWindowKb = value.toInt(&ok);
        ok = ok && tuning.aroadWindowKb >= 1 && tuning.aroadWindowKb <= 65536;
    } else if (key == "aroadbudget") {
        tuning.aroadBudgetPct = value.toInt(&ok);
        ok = ok && tuning.aroadBudgetPct >= 1 && tuning.aroadBudgetPct <= 100;
    } else if (key == "aroadchunk") {
        tuning.aroadChunk = value.toInt(&ok);
        ok = ok && tuning.aroadChunk >= 256 && tuning.aroadChunk <= 1400;
    } else if (key == "ultrambps") {
        tuning.ultraMbps = value.toInt(&ok);
        ok = ok && tuning.ultraMbps >= 1 && tuning.ultraMbps <= 2000;
    } else if (key == "ultrachannel") {
        const QString c = value.toLower();
        if (c == "video")
            tuning.ultraUnordered = false;
        else if (c == "unordered")
            tuning.ultraUnordered = true;
        else
            ok = false;
    } else if (key == "relaylog") {
        const int v = value.toInt(&ok);
        ok = ok && (v == 0 || v == 1);
        tuning.relayLog = v == 1;
    } else if (key == "clicktrace") {
        const int v = value.toInt(&ok);
        ok = ok && (v == 0 || v == 1);
        tuning.clickTrace = v == 1;
    } else if (key == "sckdepth") {
        tuning.sckQueueDepth = value.toInt(&ok);
        ok = ok && tuning.sckQueueDepth >= 1 && tuning.sckQueueDepth <= 8;
    } else if (key == "sckinterval") {
        tuning.sckMinIntervalUs = value.toInt(&ok);
        ok = ok && tuning.sckMinIntervalUs >= 0 && tuning.sckMinIntervalUs <= 100000;
    } else if (key == "pace") {
        tuning.paceMultiple = value.toInt(&ok);
        ok = ok && tuning.paceMultiple >= 0 && tuning.paceMultiple <= 50;
    } else if (key == "paceburst") {
        tuning.paceBurstKb = value.toInt(&ok);
        ok = ok && tuning.paceBurstKb >= 2 && tuning.paceBurstKb <= 1024;
    } else if (key == "retrcut") {
        tuning.retransCutPermille = value.toInt(&ok);
        ok = ok && tuning.retransCutPermille >= 0 && tuning.retransCutPermille <= 1000;
    } else if (key == "sctpbuf") {
        tuning.sctpBufferKb = value.toInt(&ok);
        ok = ok && tuning.sctpBufferKb >= 24 && tuning.sctpBufferKb <= 1024;
    } else if (key == "sctpss") {
        tuning.sctpScheduler = value.toInt(&ok);
        // Not 2, round robin by packet: against Chrome the association failed
        // ~8 s after the channel opened, twice out of two (04/10/2026).
        ok = ok && tuning.sctpScheduler >= 0 && tuning.sctpScheduler <= 5 &&
             tuning.sctpScheduler != 2;
    } else if (key == "sctpburst") {
        tuning.sctpMaxBurst = value.toInt(&ok);
        ok = ok && tuning.sctpMaxBurst >= 0 && tuning.sctpMaxBurst <= 1000;
    } else if (key == "linkhold") {
        tuning.linkHoldMs = value.toInt(&ok);
        ok = ok && tuning.linkHoldMs >= 0 && tuning.linkHoldMs <= 100;
    } else if (key == "audiolog") {
        const int v = value.toInt(&ok);
        ok = ok && (v == 0 || v == 1);
        tuning.audioLog = v == 1;
    } else if (key == "audioframe") {
        const int v = value.toInt(&ok);
        ok = ok && (v == 5 || v == 10 || v == 20);
        tuning.audioFrameMs = v == 5 ? 0 : v;
    } else {
        return false;
    }
    return true;
}

bool parseSpec(const QString& text, BenchSpec& spec, QString& error)
{
    const QStringList items = text.split(QRegularExpression("[,;]"), Qt::SkipEmptyParts);
    for (const QString& raw : items) {
        const QString item = raw.trimmed();
        const int eq = item.indexOf('=');
        if (eq <= 0) {
            error = "not a key=value pair: " + item;
            return false;
        }
        const QString key = item.left(eq).trimmed().toLower();
        const QString value = item.mid(eq + 1).trimmed();
        bool ok = true;
        if (applyTuningKey(key, value, spec.tuning, spec.gpu, ok)) {
            // handled, ok says whether the value was good
        } else if (key == "display")
            spec.display = value.toInt(&ok);
        else if (key == "seconds")
            spec.seconds = value.toInt(&ok);
        else if (key == "fps")
            spec.fps = value.toInt(&ok);
        else if (key == "bitrate")
            spec.bitrateKbps = value.toInt(&ok);
        else if (key == "width")
            spec.width = value.toInt(&ok);
        else if (key == "height")
            spec.height = value.toInt(&ok);
        else if (key == "yuv444")
            spec.yuv444 = value.toInt(&ok) != 0;
        else if (key == "hdr")
            spec.hdr = value.toInt(&ok) != 0;
        else if (key == "intra") {
            const int intra = value.toInt(&ok);
            ok = ok && intra >= 0 && intra <= 2;
            spec.intraRefresh = intra != 0;
            spec.intraRefreshRequired = intra == 2;
        } else if (key == "out")
            spec.out = value;
        else if (key == "dump")
            spec.dump = value;
        else if (key == "lose") {
            // <every>[x<burst>][k]
            QString loss = value;
            if (loss.endsWith('k')) {
                spec.loseKeyframe = true;
                loss.chop(1);
            }
            const QStringList parts = loss.split('x');
            spec.loseEvery = parts[0].toInt(&ok);
            ok = ok && spec.loseEvery >= 2 && spec.loseEvery <= 100000;
            if (ok && parts.size() == 2) {
                spec.loseBurst = parts[1].toInt(&ok);
                ok = ok && spec.loseBurst >= 1 && spec.loseBurst < spec.loseEvery;
            } else if (parts.size() > 2) {
                ok = false;
            }
        } else if (key == "ramp") {
            // <kbps>[@<seconds>]
            const QStringList parts = value.split('@');
            spec.rampKbps = parts[0].toInt(&ok);
            ok = ok && spec.rampKbps >= 100 && parts.size() <= 2;
            if (ok && parts.size() == 2) {
                spec.rampSeconds = parts[1].toDouble(&ok);
                ok = ok && spec.rampSeconds >= 0.1 && spec.rampSeconds <= 600;
            }
        } else if (key == "codec") {
            const QString c = value.toLower();
            if (c == "hevc" || c == "h265")
                spec.codec = mw::native::Codec::Hevc;
            else if (c == "h264" || c == "avc")
                spec.codec = mw::native::Codec::H264;
            else if (c == "av1")
                spec.codec = mw::native::Codec::Av1;
            else
                ok = false;
        } else {
            error = "unknown key: " + key;
            return false;
        }
        if (!ok) {
            error = "bad value for " + key + ": " + value;
            return false;
        }
    }
    if (spec.seconds <= 0 || spec.seconds > 3600) {
        error = "seconds must be between 1 and 3600";
        return false;
    }
    return true;
}

QString describeDisplay(const mw::native::DisplayInfo& d)
{
    return QString("  display=%1  %2  %3x%4 @ %5 Hz  gpu %6%7")
        .arg(d.id)
        .arg(QString::fromStdString(d.label), -10)
        .arg(d.width)
        .arg(d.height)
        .arg(d.refreshMilliHz / 1000.0, 0, 'f', 2)
        .arg(d.gpuId)
        .arg(d.detail.empty() ? QString() : "  (" + QString::fromStdString(d.detail) + ")");
}

QString describeGpu(const mw::native::GpuInfo& g)
{
    QString codecs;
    for (mw::native::Codec c : g.codecs) {
        if (!codecs.isEmpty()) codecs += ' ';
        codecs += mw::native::toString(c);
    }
    return QString("  gpu=%1  %2  %3%4")
        .arg(g.id)
        .arg(QString::fromStdString(g.name), -32)
        .arg(g.encoders.empty() ? QString("no encoder")
                                : QString(mw::native::toString(g.encoders.front())) + ": " + codecs)
        .arg(g.supports10Bit ? " (10-bit)" : "");
}

/// mean / p95 / p99 of an integer quantity, with a unit divisor.
QString tail(const mw::native::LatencyHistogram& h, double divisor, int decimals)
{
    if (h.count() == 0) return "-";
    return QString("%1 / %2 / %3")
        .arg(h.meanUs() / divisor, 0, 'f', decimals)
        .arg(h.percentileUs(0.95) / divisor, 0, 'f', decimals)
        .arg(h.percentileUs(0.99) / divisor, 0, 'f', decimals);
}

} // namespace

bool parseEncoderTuningSpec(const QString& spec, mw::native::EncoderTuning& tuning, int& gpu,
                            QString& error)
{
    gpu = -1;
    const QStringList items = spec.split(QRegularExpression("[,;]"), Qt::SkipEmptyParts);
    for (const QString& raw : items) {
        const QString item = raw.trimmed();
        const int eq = item.indexOf('=');
        if (eq <= 0) {
            error = "not a key=value pair: " + item;
            return false;
        }
        const QString key = item.left(eq).trimmed().toLower();
        const QString value = item.mid(eq + 1).trimmed();
        bool ok = true;
        if (!applyTuningKey(key, value, tuning, gpu, ok)) {
            error = "not an encoder knob: " + key;
            return false;
        }
        if (!ok) {
            error = "bad value for " + key + ": " + value;
            return false;
        }
    }
    return true;
}

QString effectiveTuningSpec(const QString& fromSettings, QString* source)
{
    const QString env = qEnvironmentVariable("MW_NATIVE_TUNING");
    if (!env.isEmpty()) {
        if (source) *source = QStringLiteral("MW_NATIVE_TUNING");
        return env;
    }
    if (source) *source = QStringLiteral("settings.json native_tuning");
    return fromSettings.trimmed();
}

int runNativeBenchCommand(const QString& specText)
{
    QTextStream out(stdout);
    QTextStream err(stderr);

    BenchSpec spec;
    QString parseError;
    if (!parseSpec(specText, spec, parseError)) {
        err << "native-bench: " << parseError << "\n\n" << kUsage;
        err.flush();
        return 2;
    }

    // The engine explains itself in its log — which GPU, which encoder, why a
    // fallback — and a bench that hid that would leave a bad number with
    // nothing to go on.
    mw::native::NativeHost::setLogSink([](int level, const std::string& message) {
        static const char* const kNames[] = {"debug", "info", "warn", "error"};
        const char* name = (level >= 0 && level <= 3) ? kNames[level] : "?";
        std::fprintf(stderr, "[%s] %s\n", name, message.c_str());
    });

    const mw::native::Capabilities caps = mw::native::NativeHost::probe();
    if (!caps.available) {
        err << "native-bench: the native engine is not available here: "
            << mw::native::toString(caps.reason)
            << (caps.diagnostic.empty() ? QString()
                                        : " (" + QString::fromStdString(caps.diagnostic) + ")")
            << "\n";
        err.flush();
        return 1;
    }

    const mw::native::DisplayInfo* display = nullptr;
    for (const mw::native::DisplayInfo& d : caps.displays)
        if (d.id == spec.display) display = &d;
    if (!display) {
        if (spec.display >= 0) err << "native-bench: no display with id " << spec.display << "\n";
        out << "Displays:\n";
        for (const mw::native::DisplayInfo& d : caps.displays)
            out << describeDisplay(d) << "\n";
        out << "GPUs:\n";
        for (const mw::native::GpuInfo& g : caps.gpus)
            out << describeGpu(g) << "\n";
        out << "\nRun again with display=<id> (and gpu=<id> to encode elsewhere).\n";
        out.flush();
        err.flush();
        return spec.display >= 0 ? 1 : 2;
    }

    mw::native::SessionConfig config;
    config.displayId = spec.display;
    config.width = spec.width;
    config.height = spec.height;
    config.fps = spec.fps;
    config.bitrateKbps = spec.bitrateKbps;
    config.clientCodecs = {spec.codec};
    config.yuv444 = spec.yuv444;
    config.hdr = spec.hdr;
    config.intraRefresh = spec.intraRefresh;
    config.intraRefreshRequired = spec.intraRefreshRequired;
    config.encodeGpuId = spec.gpu;
    config.tuning = spec.tuning;
    // The portal route (Linux, a binary without the capability to read the
    // scanout) asks the user once: a grant from an earlier run replays that
    // consent without a dialog, and a new one is printed below for the next.
    config.portalRestoreToken = qEnvironmentVariable("MW_PORTAL_RESTORE_TOKEN").toStdString();

    std::mutex rowsMutex;
    std::vector<BenchRow> rows;
    rows.reserve(static_cast<size_t>(spec.seconds) * 300);
    std::atomic<bool> ended{false};
    std::string endReason;
    // What the loop below reads and moves: the newest frame (lose=) and the
    // target (ramp=), stamped on each row.
    std::atomic<bool> anyFrame{false};
    std::atomic<uint32_t> latestFrame{0};
    std::atomic<int> targetKbps{spec.bitrateKbps};

    // The stream as it comes out, for ffmpeg to check. Written from the
    // callback once the frame's stamps are taken: it costs the next frame a
    // copy into the OS cache and nothing that is measured.
    QFile dumpFile(spec.dump);
    if (!spec.dump.isEmpty() && !dumpFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        err << "native-bench: cannot write " << spec.dump << ": " << dumpFile.errorString() << "\n";
        err.flush();
        return 1;
    }

    std::string error;
    // Set before start(), read by the callback on the session's thread — the
    // one that writes the session's info.
    mw::native::Session* live = nullptr;
    std::unique_ptr<mw::native::Session> session = mw::native::NativeHost::createSession(
        config,
        [&](const mw::native::EncodedFrame& f) {
            BenchRow row;
            if (live) row.pipeline = live->info().videoPipeline;
            row.frameNumber = f.frameNumber;
            row.keyframe = f.keyframe;
            // A re-send stamps present, captured and submitted with the same
            // "now" — see WindowsSession::emit. A capture never does.
            row.captured = !(f.presentUs == f.submittedUs && f.capturedUs == f.submittedUs);
            row.bytes = f.size;
            row.avgQp = f.avgQp;
            row.presentUs = f.presentUs;
            row.capturedUs = f.capturedUs;
            row.submittedUs = f.submittedUs;
            row.convertedUs = f.convertedUs;
            row.encodedUs = f.encodedUs;
            row.gpuConvertUs = f.gpuConvertUs;
            row.gpuEncodeUs = f.gpuEncodeUs;
            row.targetKbps = targetKbps.load();
            row.encoderKbps = f.encoderKbps;
            latestFrame.store(f.frameNumber);
            anyFrame.store(true);
            std::lock_guard<std::mutex> lock(rowsMutex);
            rows.push_back(row);
            if (dumpFile.isOpen())
                dumpFile.write(reinterpret_cast<const char*>(f.data), static_cast<qint64>(f.size));
        },
        nullptr, nullptr, nullptr,
        [&](const std::string& reason) {
            endReason = reason;
            ended.store(true);
        },
        error);
    if (!session) {
        err << "native-bench: could not create the session: " << QString::fromStdString(error)
            << "\n";
        err.flush();
        return 1;
    }
    live = session.get();
    // Called on this thread, inside start(), when the portal granted anew.
    session->setPortalGrantCallback([&](const std::string& token) {
        out << "portal grant: MW_PORTAL_RESTORE_TOKEN=" << QString::fromStdString(token) << "\n";
        out.flush();
    });
    if (!session->start(error)) {
        err << "native-bench: could not start the session: " << QString::fromStdString(error)
            << "\n";
        err.flush();
        return 1;
    }

    const mw::native::SessionInfo& info = session->info();
    out << "native-bench: " << QString::fromStdString(info.gpuName) << " · "
        << mw::native::toString(info.encoder) << " " << mw::native::toString(info.codec)
        << (info.yuv444 ? " 4:4:4" : " 4:2:0") << (info.hdr ? " HDR (BT.2020 PQ)" : " SDR") << " · "
        << info.width << "x" << info.height << " · capture " << mw::native::toString(info.capture)
        << " · fps " << (info.fps > 0 ? QString::number(info.fps) : QString("display")) << " · "
        << spec.bitrateKbps << " kbps" << (info.intraRefresh ? " · intra-refresh" : "") << " · "
        << spec.seconds << " s on " << QString::fromStdString(display->label)
        << (info.crossGpuCopy ? " · cross-GPU copy" : "")
        << (spec.tuning.isDefault() ? QString()
                                    : " · " + QString::fromStdString(spec.tuning.describe()))
        << "\n";
    // The chain the pictures take (Windows): a D3D12 row must be one.
    const mw::native::VideoPipeline pipeline = info.videoPipeline;
    if (pipeline != mw::native::VideoPipeline::Auto)
        out << "pipeline        " << mw::native::toString(pipeline) << " · "
            << QString::fromStdString(info.videoRoute) << " · "
            << QString::fromStdString(info.videoPipelineReason) << "\n";
    out.flush();
    if (spec.tuning.strict12 && spec.tuning.pipeline == mw::native::VideoPipeline::D3d12 &&
        pipeline != mw::native::VideoPipeline::D3d12) {
        err << "native-bench: strict12: D3D12 was asked for and the session runs "
            << mw::native::toString(pipeline)
            << (info.videoPipelineReason.empty()
                    ? QString()
                    : " (" + QString::fromStdString(info.videoPipelineReason) + ")")
            << "\n";
        err.flush();
        session->stop();
        return 1;
    }

    // lose= and ramp= act between frames, so the loop wakes often for them.
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(spec.seconds);
    const auto rampStep = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(spec.rampSeconds));
    auto nextStep = started + rampStep;
    uint32_t nextLoss = static_cast<uint32_t>(spec.loseEvery);
    uint32_t lastLost = 0;
    int burstLeft = 0;
    int losses = 0, steps = 0;
    bool low = false;
    const bool driving = spec.loseEvery > 0 || spec.rampKbps > 0;
    while (std::chrono::steady_clock::now() < deadline && !ended.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(driving ? 2 : 50));
        if (spec.loseEvery > 0 && anyFrame.load()) {
            const uint32_t latest = latestFrame.load();
            if (burstLeft > 0 && latest > lastLost) {
                // The rest of a burst: each new frame, the repair ones included.
                session->invalidateReference(latest);
                ++losses;
                lastLost = latest;
                if (--burstLeft == 0 && spec.loseKeyframe) session->requestKeyframe();
            } else if (burstLeft == 0 && latest >= nextLoss) {
                session->invalidateReference(latest);
                ++losses;
                lastLost = latest;
                burstLeft = spec.loseBurst - 1;
                nextLoss = latest + static_cast<uint32_t>(spec.loseEvery);
                if (burstLeft == 0 && spec.loseKeyframe) session->requestKeyframe();
            }
        }
        if (spec.rampKbps > 0 && std::chrono::steady_clock::now() >= nextStep) {
            low = !low;
            const int kbps = low ? spec.rampKbps : spec.bitrateKbps;
            session->setTargetBitrate(kbps);
            targetKbps.store(kbps);
            ++steps;
            nextStep += rampStep;
        }
    }
    session->stop();
    // A D3D12 session that failed while streaming went on in D3D11: its rows
    // after that are not D3D12 ones, and the summary says so.
    const mw::native::SessionInfo endInfo = session->info();
    session.reset();
    if (dumpFile.isOpen()) dumpFile.close();
    if (ended.load())
        err << "native-bench: the session ended early: " << QString::fromStdString(endReason)
            << "\n";
    if (endInfo.videoPipeline != pipeline)
        err << "native-bench: the session went from " << mw::native::toString(pipeline) << " to "
            << mw::native::toString(endInfo.videoPipeline)
            << " while running: " << QString::fromStdString(endInfo.videoPipelineReason) << "\n";

    // ── CSV, one row per frame ──────────────────────────────────────────────
    QString path = spec.out;
    if (path.isEmpty())
        path = QDir::current().filePath(
            "native-bench-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".csv");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        err << "native-bench: cannot write " << path << ": " << file.errorString() << "\n";
        err.flush();
        return 1;
    }
    {
        QTextStream csv(&file);
        csv << "frame,keyframe,captured,bytes,avg_qp,t0_present_us,t1_captured_us,"
               "t1b_submitted_us,t2_converted_us,t3_encoded_us,acquire_us,convert_us,"
               "encode_us,host_total_us,gpu_convert_us,gpu_encode_us,target_kbps,encoder_kbps,"
               "pipeline\n";
        for (const BenchRow& r : rows) {
            csv << r.frameNumber << ',' << (r.keyframe ? 1 : 0) << ',' << (r.captured ? 1 : 0)
                << ',' << static_cast<qulonglong>(r.bytes) << ',' << r.avgQp << ',' << r.presentUs
                << ',' << r.capturedUs << ',' << r.submittedUs << ',' << r.convertedUs << ','
                << r.encodedUs << ',' << (r.capturedUs - r.presentUs) << ','
                << (r.convertedUs - r.submittedUs) << ',' << (r.encodedUs - r.convertedUs) << ','
                << (r.encodedUs - r.presentUs) << ',' << r.gpuConvertUs << ',' << r.gpuEncodeUs
                << ',' << r.targetKbps << ',' << r.encoderKbps << ','
                << mw::native::toString(r.pipeline) << '\n';
        }
    }
    file.close();

    // ── Summary ─────────────────────────────────────────────────────────────
    // Captured frames only for the stage figures: a re-send has no capture and
    // would read as a 0 µs acquire. Bytes and QP over everything the encoder
    // produced, keyframes and re-sends included — that is what goes on the wire.
    mw::native::StageStats stages;
    mw::native::LatencyHistogram bytesAll, bytesDelta, qp, gpuConvert, gpuEncode;
    int captured = 0, keyframes = 0, resends = 0;
    int64_t firstUs = 0, lastUs = 0;
    for (const BenchRow& r : rows) {
        if (r.keyframe) keyframes++;
        bytesAll.add(static_cast<int64_t>(r.bytes));
        if (!r.keyframe) bytesDelta.add(static_cast<int64_t>(r.bytes));
        if (r.avgQp >= 0) qp.add(r.avgQp);
        if (r.gpuConvertUs >= 0) gpuConvert.add(r.gpuConvertUs);
        if (r.gpuEncodeUs >= 0) gpuEncode.add(r.gpuEncodeUs);
        if (!r.captured) {
            resends++;
            continue;
        }
        captured++;
        if (firstUs == 0) firstUs = r.presentUs;
        lastUs = r.presentUs;
        stages.record(mw::native::Stage::Acquire, r.capturedUs - r.presentUs);
        stages.record(mw::native::Stage::Convert, r.convertedUs - r.submittedUs);
        stages.record(mw::native::Stage::Encode, r.encodedUs - r.convertedUs);
        stages.record(mw::native::Stage::Total, r.encodedUs - r.presentUs);
    }
    const double spanS = (lastUs > firstUs) ? (lastUs - firstUs) / 1e6 : 0.0;
    const auto s = stages.session();
    const auto st = [&](mw::native::Stage stage) {
        const mw::native::StageSummary& x = s[static_cast<size_t>(stage)];
        if (x.count == 0) return QString("-");
        return QString("%1 / %2 / %3")
            .arg(x.meanUs / 1000.0, 0, 'f', 2)
            .arg(x.p95Us / 1000.0, 0, 'f', 2)
            .arg(x.p99Us / 1000.0, 0, 'f', 2);
    };

    out << "frames          " << rows.size() << " (" << captured << " captured, " << resends
        << " re-sent, " << keyframes << " keyframes)\n";
    // A chain that changed while running (MW_D3D12_FAULT): after which frame,
    // how long the viewer went without a picture, and whether the first one
    // after it decodes on its own.
    for (size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].pipeline == rows[i - 1].pipeline) continue;
        out << "pipeline change " << mw::native::toString(rows[i - 1].pipeline) << " → "
            << mw::native::toString(rows[i].pipeline) << " after frame " << rows[i - 1].frameNumber
            << ": " << QString::number((rows[i].encodedUs - rows[i - 1].encodedUs) / 1000.0, 'f', 1)
            << " ms without a picture, then "
            << (rows[i].keyframe ? "a keyframe" : "NOT a keyframe") << "\n";
    }
    if (spanS > 0)
        out << "capture rate    " << QString::number((captured - 1) / spanS, 'f', 1) << " fps over "
            << QString::number(spanS, 'f', 1) << " s\n";
    out << "                mean / p95 / p99\n";
    out << "acquire   ms    " << st(mw::native::Stage::Acquire) << "\n";
    out << "convert   ms    " << st(mw::native::Stage::Convert) << "\n";
    out << "encode    ms    " << st(mw::native::Stage::Encode) << "\n";
    out << "present→encoded " << st(mw::native::Stage::Total) << "\n";
    if (gpuConvert.count() > 0)
        out << "gpu convert ms  " << tail(gpuConvert, 1000.0, 2) << "  (mean / p95 / p99)\n";
    if (gpuEncode.count() > 0)
        out << "gpu encode ms   " << tail(gpuEncode, 1000.0, 2) << "  (mean / p95 / p99)\n";
    if (losses > 0)
        out << "losses          " << losses << " reported, every " << spec.loseEvery << " frames"
            << (spec.loseBurst > 1 ? QStringLiteral(", %1 in a row").arg(spec.loseBurst)
                                   : QString())
            << (spec.loseKeyframe ? ", each burst closed by a keyframe request" : "") << "\n";
    if (steps > 0)
        out << "ramp            " << steps << " steps between " << spec.bitrateKbps << " and "
            << spec.rampKbps << " kbps, every " << spec.rampSeconds << " s\n";
    out << "bytes/frame KB  " << tail(bytesAll, 1024.0, 1) << "  (deltas "
        << tail(bytesDelta, 1024.0, 1) << ")\n";
    out << "avg QP          " << tail(qp, 1.0, 1) << (qp.count() == 0 ? "  (not reported)" : "")
        << "\n";
    out << "csv             " << QDir::toNativeSeparators(path) << "\n";
    if (!spec.dump.isEmpty())
        out << "stream          " << QDir::toNativeSeparators(spec.dump) << "\n";
    out.flush();
    return 0;
}
