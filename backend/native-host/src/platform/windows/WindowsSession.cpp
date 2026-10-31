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

#include "../../audio/windows/HostMute.h"
#include "../../audio/windows/WasapiLoopback.h"
#include "../../capture/windows/DxgiDuplication.h"
#include "../../capture/windows/WgcCapture.h"
#include "../../convert/windows/ColorConvert.h"
#include "../../core/CadenceAlign.h"
#include "../../core/CadenceChoice.h"
#include "../../core/CadenceStep.h"
#include "../../core/CaptureHold.h"
#include "../../core/ClickTrace.h"
#include "../../core/CursorPositionGate.h"
#include "../../core/DeadlineCadence.h"
#include "../../core/DecodeCredit.h"
#include "../../core/FrameCadence.h"
#include "../../core/InputAdmit.h"
#include "../../core/Log.h"
#include "../../core/PresentLattice.h"
#include "../../core/Probe.h"
#include "../../core/RestartBackoff.h"
#include "../../core/Selector.h"
#include "../../core/Session.h"
#include "../../core/VideoPipelineChoice.h"
#include "../../encode/EncodeLoadCap.h"
#include "../../encode/RateControl.h"
#include "../../encode/RateGovernor.h"
#include "../../encode/windows/AmfApi.h"
#include "../../encode/windows/NvencApi.h"
#include "../../input/windows/Win32Input.h"
#include "DisplayModeHold.h"
#include "DisplayOutputs.h"
#include "InputDesktop.h"
#include "StreamPriority.h"
#include "Win32PointerSystem.h"
#include "video/D3d11VideoPipeline.h"
#include "video/D3d12VideoPipeline.h"

// GetCursorInfo/LoadCursorW, for naming the pointer — see currentCursorKind().
#include <windows.h>
// AvSetMmThreadCharacteristicsW: the capture loop runs as an MMCSS "Games" task.
#include <avrt.h>
// DwmGetCompositionTimingInfo, for the click trace (clicktrace=1) only.
#include <dwmapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mw::native {
namespace {

/// The most a composited pointer may be blown up past its real size.
///
/// Two reasons, and the second is the one that set the number. It is a 32-pixel
/// bitmap, so stretched far enough it stops reading as a pointer. And the client
/// asks for a size on the GLASS, which is constant by design — the same 18
/// pixels in portrait as in landscape — while the picture behind it is not: a
/// phone held upright shows a 16:9 desktop in a band a fifth of the screen tall,
/// and a pointer that keeps its screen size there eats a twelfth of the picture
/// it is supposed to be pointing into.
///
/// The cap is what says "not on a picture this small". It only ever binds when
/// the picture is displayed far smaller than it was encoded, which is exactly
/// that case: at 2.5 a phone in portrait draws a visible pointer that is no
/// longer out of scale with what is under it, and every larger picture — a
/// phone turned sideways, any zoom at all — is already below it and untouched.
constexpr float kMaxCursorMagnify = 2.5f;

/// The fastest still-screen floor a client may ask for.
///
/// The real limit is the stream's own frame rate and the loop applies it; this
/// one exists because the request arrives as a number from a browser and the
/// loop divides by it. High enough to be no limit at all in practice.
constexpr int kMaxFloorFps = 480;

/// How often an FP16 session re-reads the desktop's SDR white level. The
/// slider is live, its change repaints the Settings window (so a frame comes
/// to show it), and a QueryDisplayConfig costs about a tenth of a millisecond:
/// once a second is invisible both ways.
constexpr int64_t kSdrWhitePollUs = 1000000;

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// A QueryPerformanceCounter value on steadyNowUs()'s scale: MSVC's
/// steady_clock IS the counter, counted from zero. Whole seconds first, so a
/// long uptime neither overflows nor loses the fraction.
int64_t qpcToSteadyUs(int64_t qpc)
{
    static const int64_t frequency = [] {
        LARGE_INTEGER f = {};
        ::QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 1;
    }();
    return (qpc / frequency) * 1000000 + ((qpc % frequency) * 1000000) / frequency;
}

const char* acquireStatusName(capture::AcquireStatus status)
{
    switch (status) {
    case capture::AcquireStatus::Ok: return "ok";
    case capture::AcquireStatus::Timeout: return "timeout";
    case capture::AcquireStatus::PointerOnly: return "pointer";
    case capture::AcquireStatus::Lost: return "lost";
    case capture::AcquireStatus::Failed: return "failed";
    }
    return "";
}

/// The stamps a picture carries from the capture to the encoder — t₀ to t₂ of
/// EncodedFrame.
struct FrameStamps
{
    int64_t presentUs = 0;
    int64_t capturedUs = 0;
    int64_t submittedUs = 0;
    int64_t convertedUs = 0;
    /// The held picture encoded again (idle floor, refinement, a keyframe on
    /// request), not a new one: what it costs says nothing about keeping up.
    bool resend = false;
    /// For the click trace's frame rows: when the capture context had queued
    /// its wait for the conversion's read, right before the frame was given
    /// back (0 for a picture with no capture to give back).
    int64_t heldUs = 0;
};

/// A re-send has no present of its own. Reporting "now" for everything keeps
/// the latency figures honest — it measures zero capture latency because there
/// was no capture, rather than inheriting a stale present time and claiming
/// half a second of delay.
FrameStamps resendStamps(int64_t nowUs)
{
    return FrameStamps{nowUs, nowUs, nowUs, nowUs, true};
}

/// Sleep until @p deadlineUs on steadyNowUs()'s clock (cadence=deadline): a
/// high-resolution waitable timer to within kSpinUs of it, then a yielding
/// spin. The timer alone wakes up to half a millisecond late, and a late
/// wake-up spends the client's margin; the spin costs a quarter millisecond
/// of one core per client refresh.
void sleepUntilUs(HANDLE timer, int64_t deadlineUs)
{
    constexpr int64_t kSpinUs = 250;
    const int64_t coarseUs = deadlineUs - kSpinUs - steadyNowUs();
    if (coarseUs > 0 && timer) {
        LARGE_INTEGER due;
        due.QuadPart = -coarseUs * 10; // relative, in 100 ns units
        if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(timer, INFINITE);
    }
    while (steadyNowUs() < deadlineUs)
        SwitchToThread();
}

/// "165", "144", "60" — the refresh rate as a person would say it.
std::string hzString(int milliHz)
{
    return std::to_string((milliHz + 500) / 1000);
}

/// Name the pointer currently on screen, as a CSS cursor keyword.
///
/// Windows hands out the SAME handle for a standard cursor to every process, so
/// comparing what is on screen against the system set identifies it exactly —
/// no image matching, no heuristics. An application with a cursor of its own
/// matches nothing and gets "", which is the honest answer: there is no keyword
/// for someone's custom artwork.
///
/// This is a different source from the DXGI shape — user32 rather than the
/// duplication — and that is why it lives here rather than in the capture.
const char* currentCursorKind()
{
    struct Known
    {
        // LPCTSTR, not an explicit wide string: the IDC_* macros follow the
        // project's character set, and naming a width here would only compile
        // for one of them.
        LPCTSTR id;
        const char* css;
    };
    // Ordered as the Win32 headers list them. IDC_UPARROW and IDC_SIZE have no
    // CSS equivalent worth inventing, so they fall through to "".
    static const Known kKnown[] = {
        {IDC_ARROW, "default"},    {IDC_IBEAM, "text"},           {IDC_WAIT, "wait"},
        {IDC_CROSS, "crosshair"},  {IDC_SIZENWSE, "nwse-resize"}, {IDC_SIZENESW, "nesw-resize"},
        {IDC_SIZEWE, "ew-resize"}, {IDC_SIZENS, "ns-resize"},     {IDC_SIZEALL, "move"},
        {IDC_NO, "not-allowed"},   {IDC_HAND, "pointer"},         {IDC_APPSTARTING, "progress"},
        {IDC_HELP, "help"},
    };

    CURSORINFO info = {};
    info.cbSize = sizeof(info);
    if (!::GetCursorInfo(&info) || !info.hCursor) return "";

    for (const Known& known : kKnown) {
        // LoadCursor on a system cursor returns a shared handle and does not
        // need freeing; it is cheap enough to call per report and avoids
        // caching handles that a theme change could invalidate.
        if (info.hCursor == ::LoadCursor(nullptr, known.id)) return known.css;
    }
    return "";
}

/// The Windows capture → encode → deliver pipeline.
///
/// ── One thread, no queue ────────────────────────────────────────────────────
///
/// A frame is captured, converted, encoded and handed to the consumer on the
/// same thread, and the consumer sends it before returning. There is no ring
/// buffer, no worker pool and no hand-off, because each of those would add
/// latency that nothing here would win back:
///
///  - a queue only helps when the producer is faster than the consumer, and
///    here the consumer IS the network — falling behind means the link is full,
///    and buffering into a full link adds delay without delivering more;
///  - a second thread would cost a wake-up per frame to overlap work that takes
///    less than a millisecond.
///
/// The loop blocks in AcquireNextFrame, which wakes on the display's own
/// present. So the pipeline is paced by the screen rather than by a timer we
/// chose, and an idle desktop costs nothing at all.
class WindowsSession final : public Session
{
public:
    WindowsSession(const SessionConfig& config, const ResolvedTarget& target,
                   const SessionCallbacks& callbacks)
        : m_Config(config)
        , m_Target(target)
        , m_Callbacks(callbacks)
    {
        // The product's for a client whose tearing is real (Chromium on
        // Windows), the bench's inputadmit=1 for every one. Not under a bench
        // cadence, which says how the loop takes its pictures, nor under
        // PyroWave, whose ~270 KB pictures at a fixed rate would load the
        // link three at a time (plan « inputadmit »).
        const EncoderTuning& t = config.tuning;
        m_InputAdmitOn = (t.inputAdmit == 1 || (t.inputAdmit < 0 && config.clientTearingReal)) &&
                         t.cadence == EncoderTuning::Cadence::Default &&
                         t.enc12 != EncoderTuning::Encoder12::Pyrowave;
    }

    ~WindowsSession() override { stop(); }

    bool start(std::string& error) override
    {
        if (m_Running.load()) return true;

        // Confirm the display is still there — it can be unplugged between the
        // launch request and this call. What is NOT re-decided here is which
        // GPU encodes: the Selector already worked that out, and re-deriving it
        // from the display is exactly the bug that made an AMD-driven monitor
        // try to open NVENC.
        // By the display's key where there is one: the id is its place in
        // the probe's list, which another display becoming primary moves.
        const Capabilities caps = probe();
        const DisplayInfo* display = nullptr;
        for (const DisplayInfo& d : caps.displays) {
            if (m_Target.displayKey.empty() ? d.id == m_Target.displayId
                                            : d.key == m_Target.displayKey) {
                display = &d;
                break;
            }
        }
        if (!display) {
            error = "that display is no longer connected";
            return false;
        }

        // The guests' shared feed carries the pictures: input and audio only.
        if (m_Config.videoSource == VideoSource::External) return startExternal(*display, error);

        // Out of EcoQoS and ahead of the game on the GPU before anything is
        // built: a D3D12 queue takes the priority of the class it is made
        // under, for good. Taken after the first build, the class left every
        // D3D12 session's queues at HIGH under REALTIME (G2 bench, 27/09/2026).
        StreamPriority::engageProcess();

        // Everything between the captured picture and the bitstream — the
        // conversion, the encoder, and the cross-GPU bridge when the encoder
        // sits on another adapter — is the video pipeline's (design §32).
        // Opened once, here: what it keeps across capture restarts, it keeps
        // from now on.
        m_Pipeline = std::make_unique<D3d11VideoPipeline>();
        if (!m_Pipeline->open(m_Target.crossGpuCopy, m_Target.encodeAdapterHandle,
                              m_Target.encodeGpuName, error))
            return false;

        // HDR is carried only when the whole chain can. The Selector has checked
        // the display, the GPU and the codec; the capture has the last word,
        // and it is not a matter of opinion — the duplication hands over the
        // desktop as it is. A display that left HDR mode between the probe and
        // the click hands back 8-bit, and a session that went on believing it
        // had scRGB would run the PQ curve over sRGB bytes and paint a
        // blown-out picture rather than fail.
        //
        // An SDR session on an HDR desktop gets the same FP16 frames and the
        // converter tone-maps them itself (buildPipeline): DXGI's own 8-bit
        // rendition clips at 80 nits, under every window on a desktop whose
        // SDR brightness slider has been touched.
        // "Match my screen": the display in the client's own mode for the
        // session, when its driver lists one — before the capture opens, so
        // the first frame is already the right size. Best effort.
        const bool modeChanged = applyClientMode(*display);
        // No mode to switch to: the session becomes what Auto would have
        // been — the client's Auto box to fit, never upscaled — and the
        // Selector's shaping (done with upscaling allowed) is redone below.
        const bool fellBack = !modeChanged && fallBackFromMatch(m_Config);
        if (fellBack)
            log::info("[native] \"Match my screen\" falls back to Auto: fitting " +
                      std::to_string(m_Config.width) + "x" + std::to_string(m_Config.height));

        if (!openCapture(error)) return false;
        if (!convert::ColorConvert::supportsSource(m_Capture->format())) {
            error = "the display delivers frames in a format this build cannot convert (" +
                    std::to_string(static_cast<int>(m_Capture->format())) + ")";
            return false;
        }
        if (m_Target.hdr && !convert::ColorConvert::isHdrSource(m_Capture->format())) {
            log::info("[native] HDR was negotiated but the display handed over 8-bit frames — "
                      "streaming SDR from the tone-mapped desktop");
            m_Target.hdr = false;
        }

        // 4:4:4 as the Selector granted it: it has already steered the codec to
        // one that carries it, or given it up with a log line of its own. Said
        // again here because this is the line a session log is read from, and
        // a stream that quietly returns 4:2:0 looks like the setting does
        // nothing.
        const bool yuv444 = m_Target.yuv444;
        if (m_Config.yuv444 && !yuv444)
            log::info("[native] 4:4:4 requested but not granted — streaming 4:2:0");

        // The rate the encoder's budget is dimensioned for, and whether the
        // loop has to be held to it — chosen against the client's own screen
        // when it presents on vsync (chooseCadence). The Selector has already
        // resolved "0, the display's own" to the display's rounded refresh,
        // so the setting is always a number here; the guard in chooseCadence
        // is for a config that bypassed it.
        m_DisplayMilliHz =
            modeChanged && m_ModeChangedHz > 0 ? m_ModeChangedHz * 1000 : display->refreshMilliHz;
        m_DisplayMilliHzShared.store(m_DisplayMilliHz);
        m_ClientMilliHz.store(m_Config.clientRefreshMilliHz);
        m_ClientVsync.store(m_Config.clientVsync);
        {
            std::string line;
            m_EncodeFps =
                chooseCadence(m_Config.clientRefreshMilliHz, m_Config.clientVsync, m_Cadence, line);
            m_CadenceFps = m_EncodeFps;
            log::info(line);
        }
        armCadenceLock();
        logCadenceLock("");
        if (m_InputAdmitOn) {
            const EncoderTuning& t = m_Config.tuning;
            InputAdmit::Settings admit;
            if (t.inputAdmitMs > 0) admit.windowUs = t.inputAdmitMs * 1000LL;
            if (t.inputAdmitBurst > 0) admit.burst = t.inputAdmitBurst;
            if (t.inputAdmitRate > 0) admit.perSecond = t.inputAdmitRate;
            if (t.inputAdmitMax > 0) admit.maxExtra = t.inputAdmitMax;
            admit.gapUs = t.inputAdmitGapUs;
            m_InputAdmit.configure(admit, m_Cadence.intervalUs());
            char rate[16];
            std::snprintf(rate, sizeof(rate), "%g", admit.perSecond);
            log::info(
                "[native] input admit: for " + std::to_string(m_InputAdmit.windowUs() / 1000) +
                " ms after a press, every present goes, " + std::to_string(admit.maxExtra) +
                " more at most; a bucket of " + std::to_string(admit.burst) + " windows, " + rate +
                " more a second; " +
                (t.inputAdmit == 1 ? "every client (inputadmit=1)" : "while the client tears"));
        } else if (m_Config.tuning.inputAdmit != 0) {
            const EncoderTuning& t = m_Config.tuning;
            log::info(std::string("[native] input admit: off ") +
                      (t.cadence != EncoderTuning::Cadence::Default ? "under a bench cadence"
                       : t.enc12 == EncoderTuning::Encoder12::Pyrowave
                           ? "under PyroWave"
                           : "for a client whose tearing is not known to be real"));
        }

        // The client's own frame size is whatever it asked for, or the desktop
        // when it asked for nothing. The Selector shaped it to the display as
        // it was; a display just put in the client's mode reshapes it — the
        // box rule against the new size gives the box itself back.
        FrameSize frame{m_Config.width, m_Config.height};
        // A display just put in the client's mode is fitted against what the
        // client ASKED, not against the frame the Selector shaped to the old
        // mode: a 16:9 panel shaped 2560x1600 to 2560x1440, and the box rule
        // against the new 2560x1600 would then have streamed 2304x1440.
        if (modeChanged) frame = FrameSize{m_Config.requestedWidth, m_Config.requestedHeight};
        if (modeChanged || fellBack) {
            frame = frameForDisplay({m_Capture->width(), m_Capture->height()}, frame,
                                    policyOf(m_Config));
            log::info(std::string("[native] the frame follows ") +
                      (modeChanged ? "the display's new mode: " : "the Auto box: ") +
                      std::to_string(m_Config.width) + "x" + std::to_string(m_Config.height) +
                      " -> " + std::to_string(frame.width) + "x" + std::to_string(frame.height));
        }
        if (!buildPipeline(frame.width, frame.height, error)) return false;

        m_Info = SessionInfo{};
        m_Info.displayId = display->id;
        m_Info.width = m_Pipeline->outputWidth();
        m_Info.height = m_Pipeline->outputHeight();
        // What the cap scales from, fixed for the session.
        m_FullWidth = m_Info.width;
        m_FullHeight = m_Info.height;
        m_Info.fps = m_Config.fps;
        m_Info.codec = m_Target.codec;
        m_Info.encoder = m_Target.encoder;
        // What actually answered, not what the probe expected: a display that
        // fell back to WGC has to say so, because everything downstream — the
        // latency figures above all — is read differently for it.
        m_Info.capture = m_CaptureApi;
        m_Info.gpuName = m_Target.encodeGpuName;
        m_Info.hdr = m_Target.hdr;
        m_Info.displayWidth = m_Capture->width();
        m_Info.displayHeight = m_Capture->height();
        noteDesktopRect();
        m_Info.displayHdr = display->hdrActive;
        m_Info.hdrCapable = m_Target.hdrCapable;
        m_Info.yuv444 = yuv444;
        {
            std::lock_guard<std::mutex> lock(m_FormatMutex);
            m_LastFormat = DisplayFormat{m_Info.displayWidth, m_Info.displayHeight, m_Info.width,
                                         m_Info.height,       m_Info.displayHdr,    m_Info.hdr,
                                         m_Info.hdrCapable};
        }
        // Reported, not requested: an encoder that declined it says so, and the
        // receiver must then keep its usual keyframe recovery.
        m_Info.intraRefresh = m_Pipeline->intraRefreshEnabled();
        if (m_Config.intraRefresh && !m_Info.intraRefresh)
            log::info("[native] intra-refresh requested but this encoder declined it");
        // The wave's period, in frames of the rate the encoder was built for —
        // the same number the three encoders configured themselves with — or,
        // where the encoder leaves a gap between waves (oneVPL), the gap: it is
        // how long the receiver's ride-out watchdog must wait for a repair.
        const int horizon = m_Pipeline->intraRefreshHorizonFrames();
        m_Info.intraRefreshFrames = !m_Info.intraRefresh ? 0
                                    : horizon > 0        ? horizon
                                                  : encode::intraRefreshPeriodFrames(m_EncodeFps);
        // Reported the same way: the receiver decodes through a gap only when
        // the encoder really heals it. oneVPL's HEVC under intra-refresh does
        // not: a repair during a sweep hangs it, so it answers every loss with
        // a keyframe (encode::longTermRepairsSafe).
        m_Info.referenceInvalidation = m_Pipeline->supportsReferenceInvalidation();
        // Counted, not estimated: one GPU→CPU read of the bitstream. Everything
        // upstream of it stays in VRAM on the capturing adapter — unless the
        // bridge is in, which adds the readback and the upload of every frame.
        m_Info.copiesPerFrame = m_Pipeline->copiesPerFrame();
        m_Info.crossGpuCopy = m_Target.crossGpuCopy;
        m_Info.videoPipeline = m_PipelineChoice.pipeline;
        m_Info.videoRoute = m_PipelineChoice.route;
        m_Info.videoPipelineReason = m_PipelineChoice.reason;
        m_Info.videoEncoder = m_PipelineChoice.pipeline == VideoPipeline::D3d12
                                  ? m_PipelineChoice.encoder
                                  : std::string();
        m_Info.videoEncoder12 = m_PipelineChoice.pipeline == VideoPipeline::D3d12
                                    ? m_PipelineChoice.encoder12
                                    : EncoderTuning::Encoder12::Default;
        m_Info.videoPipelineRefused = m_PipelineChoice.refused;

        // Input comes up last, and its failure is NOT fatal. A session that
        // streams but cannot inject is degraded; a session that refuses to
        // start because of input gives the user nothing at all. The log says
        // which one they got.
        startInput(m_Capture->desktopRect());
        startAudio();

        // The first frame must be a keyframe — a client has nothing to decode
        // against otherwise.
        m_ForceKeyframe.store(true);
        // Awake for the stream's life (the process-wide part was taken before
        // the build): see StreamPriority.
        m_Priority.engage();
        // A host with no pointing device shows no pointer; Mouse Keys make it
        // show one, which takes a second or two — asked now rather than at the
        // loop's first look. Then every second from the loop.
        keepPointerShown();
        m_Running.store(true);
        m_Thread = std::thread([this] { run(); });
        return true;
    }

    /// A session of the guests' shared feed (VideoSource::External): the
    /// feed's own process captures and encodes, the consumer carries its
    /// pictures. What is left here is what belongs to this viewer alone —
    /// their input, and the host's audio, captured for them as for anyone.
    /// No capture, no pipeline, no loop, no GPU class: nothing here touches a
    /// GPU, and the display's mode is never the guest's to change.
    bool startExternal(const DisplayInfo& display, std::string& error)
    {
        (void)error;
        m_External = true;
        m_Info = SessionInfo{};
        m_Info.displayId = display.id;
        m_Info.displayWidth = display.width;
        m_Info.displayHeight = display.height;
        m_Info.displayHdr = display.hdrActive;
        m_Info.capture = CaptureApi::None;
        const capture::DesktopRect rect{m_Config.externalLeft, m_Config.externalTop,
                                        m_Config.externalRight, m_Config.externalBottom};
        m_Info.desktopLeft = rect.left;
        m_Info.desktopTop = rect.top;
        m_Info.desktopRight = rect.right;
        m_Info.desktopBottom = rect.bottom;
        if (!rect.valid())
            log::info("[native] input: the display's place on the desktop is not known yet — "
                      "absolute positions wait for the feed to say it");
        startInput(rect);
        startAudio();
        m_Running.store(true);
        log::info("[native] external session up: input and audio only, pictures from the feed");
        return true;
    }

    /// The viewer's keyboard, mouse and pads, aimed at @p rect. Its failure
    /// is not the session's: a session that streams but cannot inject is
    /// degraded, one that refused to start would give the viewer nothing.
    void startInput(const capture::DesktopRect& rect)
    {
        // Rumble travels the opposite way from every other input: the game
        // asks the pad to shake, and that has to reach the browser. Handed
        // straight to the session callback — it arrives on a ViGEm thread,
        // and the consumer is the one that knows how to marshal it.
        auto sink = std::make_unique<input::Win32Input>(rect, [this](const RumbleEvent& event) {
            if (m_Callbacks.onRumble) m_Callbacks.onRumble(event);
        });
        sink->setAllowElevated(m_Config.allowElevatedInput);
        if (m_Config.tuning.clickTrace) sink->setClickTrace(&m_ClickTrace);
        // On a host with no pointing device, a mouse of its own: the pointer,
        // and a device for games (SystemMouse.h). Mouse Keys when it cannot.
        sink->setVirtualMouse(m_Config.tuning.virtualMouse);
        std::string inputError;
        if (sink->start(inputError)) {
            std::lock_guard<std::mutex> lock(m_InputMutex);
            // A listener registered before start() is handed over here;
            // one registered later reaches the sink through the setter.
            sink->setGateCallback(m_OnInputGate);
            m_Input = std::move(sink);
        } else {
            log::warning("[native] input unavailable, streaming view-only: " + inputError);
        }
    }

    /// The host's playback, on the same terms as input: wanted only when the
    /// consumer gave a callback, and its failure degrades the session
    /// (silent) rather than refusing it. A machine with no playback device at
    /// all still streams its screen.
    void startAudio()
    {
        if (m_Callbacks.onAudio) {
            // Before the loopback opens: the mute may move the default output
            // to a device without speakers, and the capture must open on THAT
            // one. Best effort, and said in the log either way — a machine
            // whose output mutes in software keeps playing (see HostMute.h).
            if (m_Config.muteHostAudio) {
                std::string how;
                const auto strategy = m_HostMute.engage(how);
                m_Info.hostMuted = strategy != audio::HostMute::Strategy::None;
                log::info(std::string("[native] audio: ") + how);
            }
            auto audio = std::make_unique<audio::WasapiLoopback>(
                m_Callbacks.onAudio, m_Config.tuning.audioFrameSamples(),
                m_Config.tuning.audioGraceUs < 0 ? audio::WasapiLoopback::kDefaultGraceUs
                                                 : m_Config.tuning.audioGraceUs);
            std::string audioError;
            if (audio->start(audioError)) {
                m_Audio = std::move(audio);
                m_Info.audio = true;
            } else {
                log::warning("[native] audio unavailable, streaming silent: " + audioError);
            }
        }
    }

    void stop() override
    {
        // Idempotent, and safe from inside a callback: the flag is checked by
        // the loop, and the join is skipped when we ARE the loop.
        const bool wasRunning = m_Running.exchange(false);
        if (m_Thread.joinable()) {
            if (std::this_thread::get_id() == m_Thread.get_id())
                m_Thread.detach();
            else
                m_Thread.join();
        }
        // Torn down before the capture, and under the lock, because inject()
        // runs on the network thread and may be in flight right now. Dropping
        // the sink releases whatever the user was still holding.
        {
            std::lock_guard<std::mutex> lock(m_InputMutex);
            m_Input.reset();
        }
        // Joins the audio thread; its last packet has been delivered when this
        // returns, so the consumer can be torn down after us.
        m_Audio.reset();
        // After the loopback is closed: the speakers come back, or the default
        // output goes back to where it was.
        m_HostMute.release();
        // The display gets its own mode back (a CDS_FULLSCREEN change is
        // undone by passing no mode — and by the OS itself, should this
        // process die first).
        restoreDisplayMode();
        m_Priority.release();
        // After the join: the loop polls the same object.
        releasePointerKeys();

        if (!wasRunning && !(m_Pipeline && m_Pipeline->hasEncoder()) && !m_Capture) return;

        // Encoder, converter, held desktop, then the bridge they may live on.
        if (m_Pipeline) m_Pipeline->close();
        m_Capture.reset();
    }

    const SessionInfo& info() const override { return m_Info; }

    void sendInput(const InputEvent& event) override
    {
        // Injected HERE, on the caller's thread, never handed to the capture
        // thread: SendInput costs microseconds, and queueing it behind a frame
        // being encoded would add a whole frame time to the one path where
        // delay is felt directly.
        //
        // The lock only guards the sink's lifetime against stop(); it is
        // uncontended in steady state, since the capture thread never touches
        // input at all.
        std::lock_guard<std::mutex> lock(m_InputMutex);
        // The admission window after a press (InputAdmit.h) is told of each
        // press before it goes to the OS. An atomic for the loop.
        if (m_InputAdmitOn && m_PressFilter.isPress(event)) {
            const int64_t nowUs = steadyNowUs();
            m_InputAdmit.notePress(nowUs);
            if (m_Config.tuning.clickTrace) m_ClickTrace.admit("press", nowUs, 0);
        }
        if (m_Input) m_Input->inject(event);
    }

    void setInputGateCallback(InputGateCallback callback) override
    {
        // Under the same lock as inject(): the sink reads the callback there.
        std::lock_guard<std::mutex> lock(m_InputMutex);
        m_OnInputGate = std::move(callback);
        if (m_Input) m_Input->setGateCallback(m_OnInputGate);
    }

    void setMouseKeysCallback(MouseKeysCallback callback) override
    {
        // Before start(), by contract: read without a lock by start(), the
        // loop and stop(), one after the other.
        m_OnMouseKeys = std::move(callback);
    }

    void setDisplayFormatCallback(DisplayFormatCallback callback) override
    {
        std::lock_guard<std::mutex> lock(m_FormatMutex);
        m_OnDisplayFormat = std::move(callback);
    }

    /// Where the display the capture holds sits on the desktop, in SessionInfo
    /// for whoever carries the pictures on (the guests' shared feed).
    void noteDesktopRect()
    {
        const capture::DesktopRect& rect = m_Capture->desktopRect();
        m_Info.desktopLeft = rect.left;
        m_Info.desktopTop = rect.top;
        m_Info.desktopRight = rect.right;
        m_Info.desktopBottom = rect.bottom;
    }

    void setExternalDesktop(int left, int top, int right, int bottom) override
    {
        // A session that captures follows its own capture (restartCapture).
        if (!m_External) return;
        std::lock_guard<std::mutex> lock(m_InputMutex);
        if (m_Input) m_Input->setDisplayRect(left, top, right, bottom);
    }

    bool releaseInputBlock() override
    {
        // The sink's business: it is the side that knows the gate is closed,
        // which window closed it, and who to tell once it is open again. Under
        // the same lock as inject(), which is what guards the sink's lifetime.
        std::lock_guard<std::mutex> lock(m_InputMutex);
        return m_Input && m_Input->releaseBlock();
    }

    std::string clickTraceCsv() const override
    {
        if (!m_Config.tuning.clickTrace) return {};
        if (m_ClickTrace.dropped() > 0)
            log::warning("[native] click trace: full, " + std::to_string(m_ClickTrace.dropped()) +
                         " rows past the first " + std::to_string(ClickTrace::kMaxRows) +
                         " not kept");
        return m_ClickTrace.csv();
    }

    void setCompositeCursor(bool composite, int cursorFramePx) override
    {
        // Read by the capture thread each frame. A change takes effect on the
        // next one, which is the whole point of it being runtime-settable.
        const int wanted = cursorFramePx > 0 ? cursorFramePx : 0;
        if (m_CursorFramePx.exchange(wanted) != wanted && composite) {
            // The pointer is about to be drawn at a different size on a screen
            // that may not be moving at all — a viewer pinch-zooming a still
            // desktop. Nothing else in the loop would ever notice.
            m_CursorDirty.store(true);
        }
        if (m_CompositeCursor.exchange(composite) == composite) return;
        log::info(composite ? "[native] cursor: drawn into the picture (gaming)"
                            : "[native] cursor: handed to the client to draw (desktop)");
        // The client that just took over drawing has never seen a shape, and
        // the shape only arrives from DXGI when it CHANGES — which, for a
        // pointer sitting still, may be never. Force one report.
        m_ResendCursor.store(true);
        // Going back to compositing means the picture must show the pointer
        // again, and the last frame the client has does not.
        if (composite) m_ForceKeyframe.store(true);
        // Either way the picture is drawn again, the pointer in or out: on a
        // still screen nothing else would, and the client kept a picture
        // without it — or one with a pointer frozen beside its own.
        m_PointerChangedHands.store(true);
    }

    void setFrameFloorFps(int fps) override
    {
        // Bounded here rather than trusted: the number crosses the network from
        // a page, and the loop divides by it. The stream's own rate is the other
        // half of the clamp and lives in the loop, which is where the two are
        // compared — see floorIntervalUs().
        if (fps < 0) fps = 0;
        if (fps > kMaxFloorFps) fps = kMaxFloorFps;
        if (m_FloorFps.exchange(fps) == fps) return;
        log::info("[native] still-screen floor: " +
                  (fps > 0 ? std::to_string(fps) + " fps" : std::string("the engine's own")));
    }

    void requestKeyframe() override { m_ForceKeyframe.store(true); }

    void invalidateReference(uint32_t frameNumber) override
    {
        // Kept for the capture thread, which owns the encoder: the driver call
        // has to land between two encodes, not during one. An encoder without
        // the feature turns every one of these into a keyframe there.
        std::lock_guard<std::mutex> lock(m_InvalidateMutex);
        if (m_PendingInvalidations.size() >= kMaxPendingInvalidations) {
            // More lost frames than the DPB could ever cover: a keyframe is
            // cheaper than the list, and certain.
            m_PendingInvalidations.clear();
            m_ForceKeyframe.store(true);
            return;
        }
        m_PendingInvalidations.push_back(frameNumber);
    }

    void setTargetBitrate(int kbps) override { m_PendingBitrate.store(kbps); }

    void reportLink(const LinkFeedback& feedback) override
    {
        // Kept for the loop, which owns the governor. Two reports between two
        // wake-ups fold into one: the worse delay and the summed counts, so
        // nothing the receiver saw is lost and the loop never runs the
        // governor twice for one moment.
        std::lock_guard<std::mutex> lock(m_LinkMutex);
        if (m_LinkPending) {
            if (feedback.owdRiseMs > m_LinkFeedback.owdRiseMs)
                m_LinkFeedback.owdRiseMs = feedback.owdRiseMs;
            m_LinkFeedback.gaps += feedback.gaps;
            m_LinkFeedback.evictions += feedback.evictions;
            m_LinkFeedback.overflows += feedback.overflows;
            if (feedback.retransPermille > m_LinkFeedback.retransPermille)
                m_LinkFeedback.retransPermille = feedback.retransPermille;
            m_LinkFeedback.receivedFps = feedback.receivedFps;
        } else {
            m_LinkFeedback = feedback;
            m_LinkPending = true;
        }
    }

    void setClientRefresh(int milliHz, bool vsync) override
    {
        // Bounded, and both values kept: see storeClientRefresh.
        if (!storeClientRefresh(m_ClientMilliHz, m_ClientVsync, milliHz, vsync)) return;
        // The loop owns the gate and the budget; it re-chooses on its next
        // wake-up, between two frames.
        m_ClientRefreshDirty.store(true);
    }

    void setClientFpsCap(int fps) override
    {
        // Bounded like everything that crosses the network from a page. Zero
        // lifts the cap; a cap under 15 would be a slideshow nobody asked for.
        if (fps < 0) fps = 0;
        if (fps > 0 && fps < 15) fps = 15;
        if (fps > 1000) fps = 1000;
        if (m_ClientFpsCap.exchange(fps) == fps) return;
        // A decoder that asks for fewer frames is never stepped above its
        // rate: the step it may have had is over (CadenceStep.h).
        if (fps > 0 && m_StepFps.exchange(0) > 0)
            log::info("[native] cadence step lifted: the client's decoder asked for no more than " +
                      std::to_string(fps) + " fps");
        // Same road as a client screen that changed: the loop re-chooses the
        // gate between two frames.
        m_ClientRefreshDirty.store(true);
    }

    FpsStep setClientFpsStep(int fps) override
    {
        // Bounded like everything that crosses the network from a page.
        if (fps < 0) fps = 0;
        if (fps > 1000) fps = 1000;
        StepInputs in;
        in.askedFps = fps;
        in.baseFps = m_BaseFps.load();
        in.displayMilliHz = m_DisplayMilliHzShared.load();
        in.encodeP95Us = m_EncodeP95Us.load();
        in.encoderOnePicture = m_EncoderOnePicture.load();
        in.benchCadence = m_Config.tuning.cadence != EncoderTuning::Cadence::Default;
        in.clientCapFps = m_ClientFpsCap.load();
        in.clientVsync = m_ClientVsync.load();
        FpsStep step = decideStep(in);
        if (step.verdict != FpsStep::Verdict::Base)
            m_StepsAsked.fetch_add(1, std::memory_order_relaxed);
        if (step.verdict == FpsStep::Verdict::Refused) {
            m_StepsRefused.fetch_add(1, std::memory_order_relaxed);
            // The step in force before, if any, stays: a 240 the encoder
            // cannot hold does not undo the 120 the client kept.
            step.fps = m_StepFps.load();
            char p95[32];
            std::snprintf(p95, sizeof(p95), "%.2f", in.encodeP95Us / 1000.0);
            log::info(
                "[native] cadence step to " + std::to_string(fps) + " fps refused: " + step.why +
                " (stream at " + std::to_string(in.baseFps) + " fps" +
                (step.fps > 0 ? ", stepped to " + std::to_string(step.fps) : std::string()) + ", " +
                hzString(in.displayMilliHz) + " Hz display, encode p95 " + p95 + " ms)");
            return step;
        }
        // The loop re-chooses the gate between two frames and logs the line.
        if (m_StepFps.exchange(step.fps) != step.fps) m_ClientRefreshDirty.store(true);
        return step;
    }

    CadenceStatus cadenceStatus() const override
    {
        CadenceStatus status;
        status.presentsPerSecond = m_PresentsPerSecond.load(std::memory_order_relaxed);
        status.baseFps = m_BaseFps.load(std::memory_order_relaxed);
        status.stepFps = m_StepFps.load(std::memory_order_relaxed);
        status.displayHz = (m_DisplayMilliHzShared.load(std::memory_order_relaxed) + 500) / 1000;
        return status;
    }

    // Written by the capture thread as its loop ends (logCadence), read once
    // stop() joined it.
    CaptureHoldStatus captureHold() const override { return m_HoldStatus; }

    void setClientDecodeQueue(int depth) override { m_DecodeCredit.note(depth, steadyNowUs()); }

    void setLinkBusyProbe(LinkBusyProbe probe) override
    {
        std::lock_guard<std::mutex> lk(m_LinkBusyMutex);
        m_LinkBusyProbe = std::move(probe);
    }

    /// The relay's probe, for linkhold= (see setLinkBusyProbe). Uncontended
    /// but for the rare moment the relay sets it.
    bool linkBusy()
    {
        std::lock_guard<std::mutex> lk(m_LinkBusyMutex);
        return m_LinkBusyProbe && m_LinkBusyProbe();
    }

    void setClientVsyncGrid(double periodUs, int64_t phaseUs, int64_t leadUs, bool tearing,
                            bool steady, double budgetFps) override
    {
        if (m_Config.tuning.cadence != EncoderTuning::Cadence::Deadline) return;
        if (!m_Deadline.note(periodUs, phaseUs, leadUs, tearing, steady, budgetFps, steadyNowUs()))
            m_DeadlineRefused.fetch_add(1, std::memory_order_relaxed);
    }

    VsyncGridStatus vsyncGridStatus() const override
    {
        VsyncGridStatus status;
        if (m_Config.tuning.cadence != EncoderTuning::Cadence::Deadline) return status;
        status.wanted = true;
        // Followed within the last half second: the loop is following the grid.
        status.followed =
            steadyNowUs() - m_DeadlineAimedAtUs.load(std::memory_order_relaxed) < 500 * 1000;
        status.aimed = status.followed && !m_DeadlineAsItComes.load(std::memory_order_relaxed);
        status.presentUs = m_DisplayPeriodUs.load(std::memory_order_relaxed);
        return status;
    }

private:
    /// Take the receiver's latest report, if one arrived since the last call.
    bool takeLinkFeedback(LinkFeedback& out)
    {
        std::lock_guard<std::mutex> lock(m_LinkMutex);
        if (!m_LinkPending) return false;
        out = m_LinkFeedback;
        m_LinkPending = false;
        return true;
    }

    /// Build the converter and the encoder against whatever the capture is
    /// handing out RIGHT NOW — its device, its size, its format.
    ///
    /// @p outputWidth / @p outputHeight is the size the client decodes at; zero
    /// means "follow the desktop", which is what a session start passes when the
    /// client asked for no particular resolution. A rebuild passes the size back
    /// in, so the host changing its own mode does not change the client's
    /// geometry underneath a decoder that is already configured.
    /// Open the capture: Desktop Duplication, or Windows.Graphics.Capture when
    /// it refuses.
    ///
    /// DDA is tried first every time, including on a restart, because it is the
    /// better of the two by the measure this engine cares about: it wakes the
    /// caller ON the present, where WGC wakes it on a compositor queue. The
    /// fallback is for the machines where DDA cannot work at all — a hybrid
    /// laptop whose panel is not scanned out by the adapter it is asked of
    /// answers DXGI_ERROR_UNSUPPORTED and always will.
    // ── "Match my screen": the display in the client's mode ──────────────
    //
    // The client wants its own pixels, one for one. That is a display mode
    // of the client screen's size — and only a mode the display's driver
    // lists can be set (a physical panel takes its EDID's, a virtual display
    // its configuration's). The exact size is taken when listed, at the
    // highest refresh that is at least the display's own; otherwise the
    // largest listed mode of the client's shape (within 0.5%) that fits
    // inside it; otherwise nothing changes and the frame follows the box
    // rule as usual (the host upscales). CDS_FULLSCREEN: the change is the
    // session's — undone at stop(), and by Windows if the process dies.
    //
    // A client that relaunches starts its new session before the old one has
    // ended: the display is in the client's mode already, and the old
    // session's end would put the display's own back under the new stream.
    // So the mode has a holder (DisplayModeHold.h): the new session sets that
    // mode again and holds it, and an old one that no longer holds it leaves
    // it at its end.

    /// @returns true when the display's mode was changed for this session.
    bool applyClientMode(const DisplayInfo& display)
    {
        if (!m_Config.matchClientDisplay || !m_Config.fitRequestedBox) return false;
        const int wantW = m_Config.requestedWidth, wantH = m_Config.requestedHeight;
        if (wantW <= 0 || wantH <= 0 || display.osName.empty()) return false;

        std::wstring name(display.osName.begin(), display.osName.end()); // ASCII, "\\.\DISPLAYn"
        auto hold = std::make_unique<platform::DisplayModeHold>(display.key.empty() ? display.osName
                                                                                    : display.key);
        const platform::DisplayModeHold::Turn turn(hold.get());
        // The display as it is now, not as the probe saw it: a session ending
        // meanwhile may have put its own mode back.
        DEVMODEW now{};
        now.dmSize = sizeof(now);
        const bool nowKnown =
            ::EnumDisplaySettingsExW(name.c_str(), ENUM_CURRENT_SETTINGS, &now, 0) != FALSE;
        const int nowW = nowKnown ? static_cast<int>(now.dmPelsWidth) : display.width;
        const int nowH = nowKnown ? static_cast<int>(now.dmPelsHeight) : display.height;
        if (nowW == wantW && nowH == wantH) {
            if (nowKnown && hold->heldByAnother()) takeOverClientMode(display, now, hold);
            return false;
        }

        const int currentHz = (display.refreshMilliHz + 500) / 1000;
        const double wantAspect = static_cast<double>(wantW) / wantH;

        DEVMODEW best{};
        bool exact = false;
        long long bestArea = 0;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; ::EnumDisplaySettingsExW(name.c_str(), i, &dm, 0); ++i) {
            const int w = static_cast<int>(dm.dmPelsWidth), h = static_cast<int>(dm.dmPelsHeight);
            const int hz = static_cast<int>(dm.dmDisplayFrequency);
            if (w <= 0 || h <= 0) continue;
            if (w == wantW && h == wantH) {
                // The exact size: the highest refresh, the display's own at least.
                if (!exact || static_cast<int>(best.dmDisplayFrequency) < hz) best = dm;
                exact = true;
                continue;
            }
            if (exact || w > wantW || h > wantH) continue;
            const double aspect = static_cast<double>(w) / h;
            if (std::abs(aspect - wantAspect) / wantAspect > 0.005) continue;
            const long long area = static_cast<long long>(w) * h;
            if (area > bestArea ||
                (area == bestArea && hz > static_cast<int>(best.dmDisplayFrequency))) {
                best = dm;
                bestArea = area;
            }
        }
        if (best.dmPelsWidth == 0) {
            log::info("[native] " + display.osName + " lists no mode of " + std::to_string(wantW) +
                      "x" + std::to_string(wantH) + " or of its shape — the display keeps " +
                      std::to_string(display.width) + "x" + std::to_string(display.height));
            return false;
        }
        if (exact && static_cast<int>(best.dmDisplayFrequency) < currentHz)
            log::info("[native] " + std::to_string(wantW) + "x" + std::to_string(wantH) +
                      " is listed at " + std::to_string(best.dmDisplayFrequency) + " Hz only");

        DEVMODEW wanted{};
        wanted.dmSize = sizeof(wanted);
        wanted.dmPelsWidth = best.dmPelsWidth;
        wanted.dmPelsHeight = best.dmPelsHeight;
        wanted.dmDisplayFrequency = best.dmDisplayFrequency;
        wanted.dmBitsPerPel = best.dmBitsPerPel;
        wanted.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
        const LONG rc =
            ::ChangeDisplaySettingsExW(name.c_str(), &wanted, nullptr, CDS_FULLSCREEN, nullptr);
        if (rc != DISP_CHANGE_SUCCESSFUL) {
            log::warning("[native] " + display.osName + " refused " +
                         std::to_string(wanted.dmPelsWidth) + "x" +
                         std::to_string(wanted.dmPelsHeight) + " (ChangeDisplaySettingsEx " +
                         std::to_string(rc) + ") — the display keeps its mode");
            return false;
        }
        // The mode changed: the display's own, unless another session's was
        // on it (DisplayModeHold.h).
        hold->claim(nowKnown ? platform::modehold::mode(now.dmPelsWidth, now.dmPelsHeight,
                                                        now.dmDisplayFrequency, now.dmBitsPerPel)
                             : 0);
        m_ModeHold = std::move(hold);
        m_ModeChangedDevice = name;
        m_ModeChangedHz = static_cast<int>(wanted.dmDisplayFrequency);
        m_ModeSet = platform::modehold::mode(wanted.dmPelsWidth, wanted.dmPelsHeight,
                                             wanted.dmDisplayFrequency, wanted.dmBitsPerPel);
        log::info("[native] " + display.osName + " put in " + std::to_string(wanted.dmPelsWidth) +
                  "x" + std::to_string(wanted.dmPelsHeight) + " @ " +
                  std::to_string(wanted.dmDisplayFrequency) + " Hz for the session (" +
                  (exact ? "the client's own size" : "the largest of its shape that fits") +
                  ", was " + std::to_string(display.width) + "x" + std::to_string(display.height) +
                  ")");
        return true;
    }

    /// The client's mode, which another session set on @p display and holds
    /// for the moment — the old session of a client that relaunched — set
    /// again from this one (@p now, as the display runs it), which holds it
    /// from then on: Windows no longer undoes it with that session's process,
    /// and that session's end leaves it. Not a change of this session's: the
    /// display's rate stays the probe's, to the millihertz.
    void takeOverClientMode(const DisplayInfo& display, const DEVMODEW& now,
                            std::unique_ptr<platform::DisplayModeHold>& hold)
    {
        DEVMODEW again{};
        again.dmSize = sizeof(again);
        again.dmPelsWidth = now.dmPelsWidth;
        again.dmPelsHeight = now.dmPelsHeight;
        again.dmDisplayFrequency = now.dmDisplayFrequency;
        again.dmBitsPerPel = now.dmBitsPerPel;
        again.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
        const std::wstring name(display.osName.begin(), display.osName.end());
        const LONG rc =
            ::ChangeDisplaySettingsExW(name.c_str(), &again, nullptr, CDS_FULLSCREEN, nullptr);
        if (rc != DISP_CHANGE_SUCCESSFUL) {
            log::warning("[native] " + display.osName + " is in " +
                         std::to_string(now.dmPelsWidth) + "x" + std::to_string(now.dmPelsHeight) +
                         " for another session, and could not be taken over "
                         "(ChangeDisplaySettingsEx " +
                         std::to_string(rc) + ") — that session's end may put its own mode back");
            return;
        }
        hold->claim();
        m_ModeHold = std::move(hold);
        m_ModeChangedDevice = name;
        m_ModeChangedHz = 0;
        m_ModeSet = platform::modehold::mode(now.dmPelsWidth, now.dmPelsHeight,
                                             now.dmDisplayFrequency, now.dmBitsPerPel);
        log::info("[native] " + display.osName + " is in " + std::to_string(now.dmPelsWidth) + "x" +
                  std::to_string(now.dmPelsHeight) + " @ " +
                  std::to_string(now.dmDisplayFrequency) +
                  " Hz for another session that is ending — taken over, so its end leaves it");
    }

    /// The mode the session changed, put back — unless another session has
    /// taken it over since (takeOverClientMode). Idempotent.
    void restoreDisplayMode()
    {
        if (m_ModeChangedDevice.empty()) return;
        {
            const platform::DisplayModeHold::Turn turn(m_ModeHold.get());
            if (m_ModeHold && m_ModeHold->valid() && !m_ModeHold->held()) {
                log::info("[native] the display's mode belongs to the session that took it over "
                          "— left as it is");
            } else {
                if (!putOwnModeBack()) {
                    const LONG rc = ::ChangeDisplaySettingsExW(m_ModeChangedDevice.c_str(), nullptr,
                                                               nullptr, 0, nullptr);
                    if (rc == DISP_CHANGE_SUCCESSFUL)
                        log::info("[native] the display's own mode is back");
                    else
                        log::warning("[native] the display's own mode could not be put back "
                                     "(ChangeDisplaySettingsEx " +
                                     std::to_string(rc) + ")");
                }
                if (m_ModeHold) m_ModeHold->release();
            }
        }
        m_ModeHold.reset();
        m_ModeChangedDevice.clear();
        m_ModeChangedHz = 0;
        m_ModeSet = 0;
    }

    /// The display's own mode, kept since a session first changed it, set
    /// again by this session — when Windows' registry mode, which passing no
    /// mode would give, is another: 800x600 for the owner's virtual display
    /// while ours is on (DisplayModeHold.h). A change of no CDS_FULLSCREEN,
    /// which Windows leaves as it is when this process exits. False, nothing
    /// done, when the registry's is the one, or when the set failed.
    bool putOwnModeBack()
    {
        const uint64_t own = m_ModeHold ? m_ModeHold->ownMode() : 0;
        DEVMODEW registry{};
        registry.dmSize = sizeof(registry);
        const uint64_t inRegistry =
            ::EnumDisplaySettingsExW(m_ModeChangedDevice.c_str(), ENUM_REGISTRY_SETTINGS, &registry,
                                     0)
                ? platform::modehold::mode(registry.dmPelsWidth, registry.dmPelsHeight,
                                           registry.dmDisplayFrequency, registry.dmBitsPerPel)
                : 0;
        if (!platform::modehold::putsOwnBackItself(own, inRegistry)) return false;
        DEVMODEW back{};
        back.dmSize = sizeof(back);
        back.dmPelsWidth = platform::modehold::widthOf(own);
        back.dmPelsHeight = platform::modehold::heightOf(own);
        back.dmDisplayFrequency = platform::modehold::hzOf(own);
        back.dmBitsPerPel = platform::modehold::bitsPerPixelOf(own);
        back.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
        const auto spelled = [](uint64_t m) {
            return std::to_string(platform::modehold::widthOf(m)) + "x" +
                   std::to_string(platform::modehold::heightOf(m)) + " @ " +
                   std::to_string(platform::modehold::hzOf(m)) + " Hz";
        };
        const LONG rc =
            ::ChangeDisplaySettingsExW(m_ModeChangedDevice.c_str(), &back, nullptr, 0, nullptr);
        if (rc != DISP_CHANGE_SUCCESSFUL) {
            log::warning("[native] the display's own mode, " + spelled(own) +
                         ", could not be set again (ChangeDisplaySettingsEx " + std::to_string(rc) +
                         ") — Windows' registry mode instead");
            return false;
        }
        log::info("[native] the display's own mode is back: " + spelled(own) +
                  (inRegistry ? ", where Windows' registry has " + spelled(inRegistry)
                              : std::string(", Windows' registry mode unread")));
        return true;
    }

    /// The client's mode set again on the display this session put in it,
    /// found in another size as the capture comes back — while the session
    /// still holds that mode (DisplayModeHold.h): our virtual display turning
    /// off under a "Match my screen" stream of another display put that one
    /// back in its own mode, and the stream went on in it (N95, 19/10/2026).
    /// True when a change was tried, whatever came of it: once per restart.
    bool setClientModeAgain()
    {
        if (m_ModeChangedDevice.empty() || m_ModeSet == 0) return false;
        const platform::DisplayModeHold::Turn turn(m_ModeHold.get());
        // Another session's now, or no holder to tell: left as it is.
        if (!m_ModeHold || !m_ModeHold->valid() || !m_ModeHold->held()) return false;
        DEVMODEW now{};
        now.dmSize = sizeof(now);
        if (!::EnumDisplaySettingsExW(m_ModeChangedDevice.c_str(), ENUM_CURRENT_SETTINGS, &now, 0))
            return false;
        const uint64_t running = platform::modehold::mode(now.dmPelsWidth, now.dmPelsHeight,
                                                          now.dmDisplayFrequency, now.dmBitsPerPel);
        if (!platform::modehold::setsClientModeAgain(m_ModeSet, running)) return false;
        DEVMODEW again{};
        again.dmSize = sizeof(again);
        again.dmPelsWidth = platform::modehold::widthOf(m_ModeSet);
        again.dmPelsHeight = platform::modehold::heightOf(m_ModeSet);
        again.dmDisplayFrequency = platform::modehold::hzOf(m_ModeSet);
        again.dmBitsPerPel = platform::modehold::bitsPerPixelOf(m_ModeSet);
        again.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
        const std::string sizes = std::to_string(again.dmPelsWidth) + "x" +
                                  std::to_string(again.dmPelsHeight) + ", found in " +
                                  std::to_string(now.dmPelsWidth) + "x" +
                                  std::to_string(now.dmPelsHeight);
        const LONG rc = ::ChangeDisplaySettingsExW(m_ModeChangedDevice.c_str(), &again, nullptr,
                                                   CDS_FULLSCREEN, nullptr);
        if (rc == DISP_CHANGE_SUCCESSFUL)
            log::info("[native] the display is in the client's mode again: " + sizes);
        else
            log::warning("[native] the client's mode, " + sizes +
                         ", could not be set again (ChangeDisplaySettingsEx " + std::to_string(rc) +
                         ") — the stream follows the display");
        return true;
    }

    /// Start @p capture's duplication from a thread standing on the input
    /// desktop. DXGI duplicates the desktop of the calling thread, and a
    /// thread that owns a window cannot be moved off `Default` — the worker's
    /// Qt thread, which starts the session, is one. Started there while the
    /// secure desktop was up (a Ctrl+Alt+Suppr screen, a locked PC), a session
    /// ended at once: 0x80070005 (29/09/2026). A thread of its own owns
    /// nothing, follows, and hands the duplication back; the capture thread
    /// joins the same desktop before it reads (run()).
    static bool startCapture(capture::IWindowsCapture& capture, bool attached, std::string& error)
    {
        if (attached || !platform::runningAsSystem()) return capture.start(error);
        bool started = false;
        std::thread helper([&] {
            std::string desktop;
            platform::attachThread(&desktop);
            log::info("[native] the capture opens from a thread on the \"" + desktop +
                      "\" desktop");
            started = capture.start(error);
        });
        helper.join();
        return started;
    }

    bool openCapture(std::string& error)
    {
        // Follow the desktop switch before asking for a duplication. DXGI
        // duplicates the desktop of the CALLING thread, and refuses outright
        // ("only a SecureUI is displayed") when that thread sits on `Default`
        // while Windows shows the UAC prompt or the lock screen on `Winlogon`.
        // A SYSTEM worker may cross; anything less may not, and for it this is
        // a no-op, leaving the wait in restartCapture() as the whole answer.
        //
        // Placed here rather than in the loop so that the one call site that
        // matters — restartCapture(), which runs ON the capture thread — is
        // covered, and so is every future one. The other one, start(), runs on
        // the caller's thread, which may own a window and then cannot move:
        // startCapture() takes it from there.
        const bool attached = platform::attachThread();

        // Which output the display is now — before either backend is asked
        // for it by index. See locateOutput().
        if (!locateOutput(error)) return false;

        // MW_CAPTURE=wgc takes the fallback on a machine where Desktop
        // Duplication works perfectly well. Without it the WGC path is only
        // reachable on hardware nobody here has, which is how a fallback rots:
        // it is written once, never run, and found broken on the one machine
        // that needed it. Same purpose as MW_CONSOLE_LAUNCH=force for the
        // console launcher.
        bool forceWgc = false;
        {
            char value[16] = {};
            const DWORD n = ::GetEnvironmentVariableA("MW_CAPTURE", value, sizeof(value));
            forceWgc = n > 0 && n < sizeof(value) && ::_stricmp(value, "wgc") == 0;
        }

        // The bench's key (capture=, EncoderTuning::Capture; plan « Gels du
        // lien », route B). wgc asks for Windows.Graphics.Capture first and
        // takes the duplication when WGC cannot serve — said in the log, so
        // that a pass labelled wgc is not read as one — where the variable
        // above fails the session instead. A duplication found to paint the
        // pointer in stays on WGC, as without the key.
        const EncoderTuning::Capture asked = m_Config.tuning.capture;
        if (asked == EncoderTuning::Capture::Wgc && !forceWgc && !m_DuplicationPaintsPointer) {
            std::string wgcError;
            if (!capture::WgcCapture::available(&wgcError))
                wgcError = "Windows.Graphics.Capture is unavailable here: " + wgcError;
            else if (openWgc(wgcError)) {
                m_DdaMayReturn = false;
                return true;
            }
            log::info("[native] capture=wgc: Windows.Graphics.Capture cannot serve this display (" +
                      wgcError + ") — Desktop Duplication instead");
            bool mayReturn = false;
            if (openDuplication(attached, error, mayReturn)) return true;
            error = wgcError + " (and Desktop Duplication: " + error + ")";
            return false;
        }

        std::string ddaError;
        // Whether Desktop Duplication may serve this display later in the
        // session, so that the fallback below keeps looking for it.
        bool ddaMayReturn = false;
        if (m_DuplicationPaintsPointer) {
            // Settled earlier in this session, and a restart would not unsettle
            // it: the driver has no hardware pointer. See PaintedPointer.h.
            ddaError = "it paints the pointer into the picture";
        } else if (forceWgc) {
            ddaError = "MW_CAPTURE=wgc";
        } else if (ddaRefusedOnPurpose(ddaError)) {
            ddaMayReturn = true;
        } else if (openDuplication(attached, ddaError, ddaMayReturn)) {
            return true;
        }

        if (asked == EncoderTuning::Capture::Dda) {
            // The duplication alone, as a machine without WGC has it: a pass
            // labelled dda is never a WGC one. The loop tries again, as it
            // does on such a machine.
            log::info("[native] capture=dda: Desktop Duplication refused this display (" +
                      ddaError + "), and no fallback is taken");
            error = ddaError;
            return false;
        }

        std::string wgcWhy;
        if (!capture::WgcCapture::available(&wgcWhy)) {
            // No fallback to offer: report the FIRST failure, which is the one
            // that describes this machine. "WGC is unavailable" would send the
            // reader after the wrong thing entirely — but the log says why,
            // for the session that asked for WGC itself.
            log::info("[native] Windows.Graphics.Capture is unavailable here: " + wgcWhy);
            error = ddaError;
            return false;
        }

        log::info("[native] Desktop Duplication refused this display (" + ddaError +
                  ") — falling back to Windows.Graphics.Capture");

        std::string wgcError;
        if (!openWgc(wgcError)) {
            // Both failed. The DDA reason is the one worth carrying: WGC is the
            // afterthought, and its message would hide why the primary path
            // could not serve this display.
            error = ddaError + " (and the fallback failed too: " + wgcError + ")";
            return false;
        }

        // A refusal that may pass — the secure desktop, for a worker below
        // SYSTEM, is the usual one — is not the session's last word: the loop
        // looks for the duplication to come back (duplicationBack), and with
        // it the wake-up on the present, which WGC has not (plan C8.3 bis).
        m_DdaMayReturn = ddaMayReturn;
        m_DdaTries = 0;
        m_NextDdaLookUs = steadyNowUs() + kDdaLookUs;
        return true;
    }

    /// Desktop Duplication on the display: m_Capture and m_CaptureApi set.
    /// False with its reason, and whether that reason may pass
    /// (DxgiDuplication::refusalMayPass).
    bool openDuplication(bool attached, std::string& error, bool& mayReturn)
    {
        m_Capture = std::make_unique<capture::DxgiDuplication>(m_Target.captureAdapterHandle,
                                                               m_Target.outputIndex);
        if (startCapture(*m_Capture, attached, error)) {
            m_CaptureApi = CaptureApi::DxgiDuplication;
            m_DdaMayReturn = false;
            return true;
        }
        mayReturn = static_cast<capture::DxgiDuplication&>(*m_Capture).refusalMayPass();
        return false;
    }

    /// Windows.Graphics.Capture on the display (the caller has asked
    /// WgcCapture::available first): m_Capture and m_CaptureApi set. False
    /// with start()'s reason.
    bool openWgc(std::string& error)
    {
        m_Capture = std::make_unique<capture::WgcCapture>(m_Target.captureAdapterHandle,
                                                          m_Target.outputIndex);
        if (!m_Capture->start(error)) {
            m_Capture.reset();
            return false;
        }
        // ⚠️ HDR is not carried on this path. WGC can deliver FP16, but nothing
        // here has ever seen it do so — the machines that need the fallback are
        // exactly the ones nobody has an HDR panel on — and an unwatched colour
        // pipeline is how a stream ends up subtly wrong for months (the rule
        // AMF and oneVPL are held to in §16.3). The session downgrades itself
        // below, on the format the capture really hands over.
        m_CaptureApi = CaptureApi::WindowsGraphicsCapture;
        return true;
    }

    /// While Windows.Graphics.Capture stands in for a duplication refused for
    /// a reason that may pass: whether Desktop Duplication would open this
    /// display now. A cheap look every half second first — below SYSTEM the
    /// secure desktop cannot even be read, so nothing is tried until the
    /// user's desktop is back — then a duplication opened on the side, tried
    /// less and less often while it keeps failing; WGC streams on meanwhile.
    /// True sends the loop through the ordinary restart, whose openCapture()
    /// takes Desktop Duplication first.
    bool duplicationBack()
    {
        const int64_t nowUs = steadyNowUs();
        if (nowUs < m_NextDdaLookUs) return false;
        m_NextDdaLookUs = nowUs + kDdaLookUs;
        // The secure desktop, or the bench's stand-in for it: nothing to try
        // until the user's desktop is back, and then at once.
        std::string why;
        if (ddaRefusedOnPurpose(why) ||
            (!platform::runningAsSystem() && platform::inputDesktopName() != "Default"))
            return false;

        // Gone or moved meanwhile: the index is looked up again first, and a
        // display that is not there is not one to look for a duplication of.
        if (!locateOutput(why)) return false;
        capture::DxgiDuplication probe(m_Target.captureAdapterHandle, m_Target.outputIndex);
        if (probe.start(why)) {
            probe.stop();
            log::info("[native] Desktop Duplication serves this display again — leaving "
                      "Windows.Graphics.Capture");
            return true;
        }
        if (!probe.refusalMayPass()) {
            m_DdaMayReturn = false;
            log::info("[native] Desktop Duplication will not serve this display (" + why +
                      ") — Windows.Graphics.Capture for the rest of the session");
            return false;
        }
        // 1 s, 2 s, 4 s … then every 30 s.
        ++m_DdaTries;
        const int64_t waitUs = kDdaTryUs << (m_DdaTries < 6 ? m_DdaTries - 1 : 5);
        m_NextDdaLookUs = nowUs + (waitUs < kDdaTryMaxUs ? waitUs : kDdaTryMaxUs);
        if (m_DdaTries == 1)
            log::info("[native] Desktop Duplication still refuses this display (" + why +
                      ") — asking again, less and less often");
        return false;
    }

    /// MW_DDA_REFUSE=[<at>+]<seconds>: Desktop Duplication refused as the
    /// secure desktop refuses a worker below SYSTEM (0x80070005), for that
    /// long — from the session's first capture, or from <at> seconds after
    /// it, when the duplication running then is lost first, as a lock screen
    /// would take it. How the road to the fallback and back is put on the
    /// bench without a lock screen someone has to unlock (plan C8.3 bis);
    /// nothing happens without the variable.
    bool ddaRefusedOnPurpose(std::string& why)
    {
        readDdaRefusal();
        const int64_t nowUs = steadyNowUs();
        if (nowUs < m_RefuseDdaFromUs || nowUs >= m_RefuseDdaUntilUs) return false;
        why = "could not start Desktop Duplication (0x80070005, MW_DDA_REFUSE)";
        return true;
    }

    /// MW_DDA_REFUSE with an <at>: true once, when the duplication running
    /// then is to be taken as lost.
    bool ddaLostOnPurpose()
    {
        readDdaRefusal();
        if (m_DdaLostOnPurpose || m_RefuseDdaFromUs <= m_RefuseDdaReadUs) return false;
        if (steadyNowUs() < m_RefuseDdaFromUs) return false;
        m_DdaLostOnPurpose = true;
        log::info("[native] MW_DDA_REFUSE: the duplication taken as lost, as a lock screen would");
        return true;
    }

    void readDdaRefusal()
    {
        if (m_RefuseDdaReadUs >= 0) return;
        m_RefuseDdaReadUs = steadyNowUs();
        char value[32] = {};
        const DWORD n = ::GetEnvironmentVariableA("MW_DDA_REFUSE", value, sizeof(value));
        if (n == 0 || n >= sizeof(value)) return;
        const char* plus = std::strchr(value, '+');
        const int at = plus ? std::atoi(value) : 0;
        const int seconds = std::atoi(plus ? plus + 1 : value);
        if (seconds <= 0 || at < 0) {
            log::info(std::string("[native] MW_DDA_REFUSE=") + value +
                      " is not [<at>+]<seconds> — ignored");
            return;
        }
        m_RefuseDdaFromUs = m_RefuseDdaReadUs + static_cast<int64_t>(at) * 1000000;
        m_RefuseDdaUntilUs = m_RefuseDdaFromUs + static_cast<int64_t>(seconds) * 1000000;
        log::info("[native] MW_DDA_REFUSE in effect: Desktop Duplication refused for " +
                  std::to_string(seconds) + " s" +
                  (at > 0 ? ", " + std::to_string(at) + " s into the session" : std::string()) +
                  ", as the secure desktop refuses a worker below SYSTEM");
    }

    /// What the choice of a chain needs to know about this build.
    VideoPipelineFacts pipelineFacts() const
    {
        VideoPipelineFacts f;
        f.benchKey = m_Config.tuning.pipeline;
        f.setting = m_Config.videoPipeline;
        f.encoder = m_Target.encoder;
        f.codec = m_Target.codec;
        f.conv12 = m_Config.tuning.conv12;
        f.enc12 = m_Config.tuning.enc12;
        f.crossGpuCopy = m_Target.crossGpuCopy;
        f.yuv444 = m_Target.yuv444;
        // Taken for granted until a build finds out: a GPU without it (Windows
        // 10, a driver that takes no HEVC) refuses the D3D12 build, which says
        // why, and D3D11 carries on. One with no D3D12 Video Encode at all is
        // then known, and the builds after it choose D3D11 from the start.
        f.videoEncode12 =
            D3d12VideoPipeline::videoEncodeMissing(m_Target.encodeAdapterHandle).empty();
        // The vendors' runtimes take D3D12 pictures wherever they load: the
        // headers this is built against have the interfaces, and an older
        // driver refuses at load, with its reason (AMF's is asked for the
        // headers' 1.5, past the 1.4.33 its DX12 needs).
        f.nvenc12 =
            m_Target.encoder == EncoderApi::Nvenc && encode::NvencApi::instance()->available();
        f.amf12 = m_Target.encoder == EncoderApi::Amf && encode::AmfApi::instance()->available();
        f.driverExcluded =
            d3d12DriverExcluded(m_Target.encodeVendorId, m_Target.encodeDriverVersion);
        f.intraRefreshRequired = m_Config.intraRefreshRequired;
        f.d3d12IntraRefresh = !m_D3d12NoIntraRefresh;
        return f;
    }

    /// @p kind in place, opened: the pipeline already there, or its
    /// replacement — the old one closed first, its encoder and converter
    /// with it. A D3D12 chain ending in another encoder (@p encoder12) is a
    /// replacement too.
    bool usePipeline(VideoPipeline kind, EncoderTuning::Encoder12 encoder12, std::string& error)
    {
        const char* wanted = kind == VideoPipeline::D3d12 ? "d3d12" : "d3d11";
        if (m_Pipeline && std::strcmp(m_Pipeline->kind(), wanted) == 0) {
            const auto* chain = dynamic_cast<const D3d12VideoPipeline*>(m_Pipeline.get());
            if (!chain || chain->encoder12() == encoder12) return true;
        }
        if (m_Pipeline) m_Pipeline->close();
        if (kind == VideoPipeline::D3d12)
            m_Pipeline = std::make_unique<D3d12VideoPipeline>(m_Config.tuning, encoder12);
        else
            m_Pipeline = std::make_unique<D3d11VideoPipeline>();
        return m_Pipeline->open(m_Target.crossGpuCopy, m_Target.encodeAdapterHandle,
                                m_Target.encodeGpuName, error);
    }

    /// The pipeline for this build — the chain VideoPipelineChoice says, D3D12
    /// refused for this build only when it will not build, D3D11 for the rest
    /// of the session once D3D12 failed while streaming — built at
    /// @p outputWidth × @p outputHeight.
    ///
    /// @p keepHeld keeps the desktop copy the pointer-only and still-screen
    /// paths redraw from: the load cap rebuilds on the same capture, whose
    /// picture is still the right one.
    bool buildPipeline(int outputWidth, int outputHeight, std::string& error, bool keepHeld = false)
    {
        VideoPipelineChoice choice = chooseVideoPipeline(pipelineFacts());
        // PyroWave (POC Ultra) has no D3D11 twin, and a page in Ultra mode
        // decodes nothing else: HEVC in its place would be a silent black
        // stream. It is strict12 by nature, failing out loud.
        const bool strict =
            m_Config.tuning.strict12 || m_Config.tuning.enc12 == EncoderTuning::Encoder12::Pyrowave;
        const char* strictWhy = m_Config.tuning.strict12 ? "strict12" : "enc12=pyrowave";
        if (m_Config.tuning.enc12 == EncoderTuning::Encoder12::Pyrowave &&
            choice.pipeline != VideoPipeline::D3d12) {
            error =
                "enc12=pyrowave runs on the D3D12 route only (pipeline=d3d12): " + choice.reason;
            return false;
        }
        if (choice.pipeline == VideoPipeline::D3d12 && m_D3d12Failed && strict) {
            error = std::string(strictWhy) +
                    ": no D3D11 fallback, D3D12 failed earlier in this session (" +
                    m_D3d12FailedWhy + ")";
            return false;
        }
        if (choice.pipeline == VideoPipeline::D3d12 && m_D3d12Failed) {
            choice.pipeline = VideoPipeline::D3d11;
            choice.route = "D3D11";
            choice.refused = true;
            choice.reason +=
                ", D3D11 runs: D3D12 failed earlier in this session (" + m_D3d12FailedWhy + ")";
        }
        if (choice.pipeline == VideoPipeline::D3d12) {
            std::string why;
            bool built = usePipeline(VideoPipeline::D3d12, choice.encoder12, why) &&
                         buildOn(outputWidth, outputHeight, why, keepHeld, choice.encoder12);
            // A stream that must refresh by intra-refresh does not settle for
            // a D3D12 encoder that declined it: known from now on, and D3D11 is
            // built instead, below.
            const bool noWave =
                built && m_Config.intraRefreshRequired && !m_Pipeline->intraRefreshEnabled();
            if (built && !noWave) {
                notePipeline(choice);
                return true;
            }
            if (noWave) {
                m_D3d12NoIntraRefresh = true;
                why = "its encoder grants no intra-refresh, which this stream requires";
            }
            if (strict) {
                error = std::string(strictWhy) + ": the D3D12 chain does not build: " + why;
                return false;
            }
            choice.pipeline = VideoPipeline::D3d11;
            choice.route = "D3D11";
            choice.refused = true;
            choice.reason += noWave ? ", D3D11 runs: " + why
                                    : ", D3D11 runs: the D3D12 build failed (" + why + ")";
            // The held copy, if any, was the D3D12 chain's.
            keepHeld = false;
        } else if (choice.refused && strict) {
            error = std::string(strictWhy) + ": " + choice.reason;
            return false;
        }
        if (!usePipeline(VideoPipeline::D3d11, EncoderTuning::Encoder12::VideoEncode, error) ||
            !buildOn(outputWidth, outputHeight, error, keepHeld))
            return false;
        notePipeline(choice);
        return true;
    }

    /// The chain a build settled on, in SessionInfo and — when it changed —
    /// in the log.
    void notePipeline(const VideoPipelineChoice& choice)
    {
        const bool changed = choice.pipeline != m_PipelineChoice.pipeline ||
                             choice.route != m_PipelineChoice.route ||
                             choice.reason != m_PipelineChoice.reason;
        m_PipelineChoice = choice;
        m_PipelineLost = false;
        m_Info.videoPipeline = choice.pipeline;
        m_Info.videoRoute = choice.route;
        m_Info.videoPipelineReason = choice.reason;
        // A choice turned D3D11 by its build still names the D3D12 encoder it
        // was for: only a running D3D12 chain has one to show.
        m_Info.videoEncoder =
            choice.pipeline == VideoPipeline::D3d12 ? choice.encoder : std::string();
        m_Info.videoEncoder12 = choice.pipeline == VideoPipeline::D3d12
                                    ? choice.encoder12
                                    : EncoderTuning::Encoder12::Default;
        m_Info.videoPipelineRefused = choice.refused;
        if (changed)
            log::info(std::string("[native] video pipeline: ") +
                      (choice.pipeline == VideoPipeline::D3d12 ? "D3D12 (" + choice.route + ")"
                                                               : std::string("D3D11")) +
                      ", because " + choice.reason);
    }

    /// Both halves of the current pipeline, built at @p outputWidth ×
    /// @p outputHeight — see buildPipeline. @p encoder12: the D3D12 chain's
    /// encoder, as the choice resolved it.
    bool buildOn(int outputWidth, int outputHeight, std::string& error, bool keepHeld,
                 EncoderTuning::Encoder12 encoder12 = EncoderTuning::Encoder12::VideoEncode)
    {
        // Released before the replacements are built, not after — see
        // WindowsVideoPipeline::teardown.
        m_Pipeline->teardown(keepHeld);

        // Ahead of the game in the GPU's queue, on every (re)build: a lost
        // duplication comes back on a new device. See StreamPriority.
        StreamPriority::raiseDevice(m_Capture->device(), "capture");
        m_Pipeline->raisePriority();

        // HDR only while the capture REALLY hands FP16 over, whatever was
        // negotiated: a rebuild happens after a mode change, and turning
        // Windows HDR off is one of the changes that triggers it. The
        // converter refuses PQ over 8-bit rather than misreading the bytes,
        // and this is what keeps it from ever seeing that. The other way —
        // FP16 into an SDR session — is the tone map, and a session that
        // started SDR stays SDR: the client negotiated that, not the desktop.
        const bool hdr = m_Target.hdr && convert::ColorConvert::isHdrSource(m_Capture->format());
        if (m_Target.hdr && !hdr) {
            log::info("[native] the display left HDR — rebuilding on the SDR path");
            m_Target.hdr = false;
            m_Info.hdr = false;
        }

        // The resample filter for a stream smaller than the screen: the
        // bench's pick (Lanczos-2 dilated, linear light — docs/bench-native-host
        // §8j) on every hardware encoder, the free bilinear on the software
        // tier, whose machine has no GPU time to spend either. MW_SCALER=
        // bilinear|lanczos2 overrides it, for the A/B on a real stream.
        using ScaleFilter = convert::ColorConvert::ScaleFilter;
        ScaleFilter filter = m_Target.encoder == EncoderApi::Software ? ScaleFilter::Bilinear
                                                                      : ScaleFilter::Lanczos2;
        {
            char value[16] = {};
            const DWORD n = ::GetEnvironmentVariableA("MW_SCALER", value, sizeof(value));
            if (n > 0 && n < sizeof(value)) {
                if (convert::parseScaleFilter(value, filter)) {
                    m_ScalerPinned = true;
                    log::info(std::string("[native] MW_SCALER in effect: ") + toString(filter));
                } else
                    log::info(std::string("[native] MW_SCALER=") + value +
                              " is not a filter (bilinear, lanczos2) — ignored");
            }
        }

        WindowsVideoPipeline::ConverterBuild converter;
        converter.outputWidth = outputWidth;
        converter.outputHeight = outputHeight;
        converter.yuv444 = m_Target.yuv444;
        converter.hdr = hdr;
        converter.filter = filter;
        converter.mwsrStats = m_Config.mwsrZones;
        if (!m_Pipeline->buildConverter(*m_Capture, converter, error)) return false;
        if (m_Pipeline->toneMapsToSdr())
            log::info("[native] SDR stream of an HDR desktop — tone-mapped on the GPU");
        // A new converter starts at the 80-nit default; the display's real
        // level goes in before the first frame, and the log says what it was.
        m_SdrWhite = 0.0f;
        if (m_Pipeline->scRgbSource()) applySdrWhite(readSdrWhite());
        // A new converter is back on the filter chosen above: the guard judges
        // it afresh rather than on the previous pipeline's frames.
        m_ScalerWindowUs = 0;
        m_ScalerWindowFrames = 0;

        WindowsVideoPipeline::EncoderBuild encoder;
        encoder.encoder = m_Target.encoder;
        encoder.codec = m_Target.codec;
        encoder.fps = m_EncodeFps;
        encoder.bitrateKbps = m_Config.bitrateKbps;
        encoder.yuv444 = m_Target.yuv444;
        encoder.hdr = hdr;
        encoder.intraRefresh = m_Config.intraRefresh;
        encoder.tuning = m_Config.tuning;
        encoder.encoder12 = encoder12;
        if (!m_Pipeline->buildEncoder(*m_Capture, encoder, error)) return false;
        m_EncoderKbps = encoder.bitrateKbps;
        return true;
    }

    /// The duplication was lost — a resolution change, a mode set, a desktop
    /// switch, a driver restart. Open it again and rebuild everything behind it.
    ///
    /// Everything, because DxgiDuplication::start() creates a NEW D3D11 device.
    /// The converter and the encoder were built on the old one, and their
    /// textures belong to a device the capture no longer uses: kept across the
    /// restart they convert nothing and encode nothing, which is precisely how
    /// changing the host's resolution mid-stream turned the picture black and
    /// then, a few seconds later, killed the session outright.
    ///
    /// What deliberately does NOT change is the size the client decodes at. The
    /// host's desktop may have gone from 1440p to 1080p; the stream stays at the
    /// resolution that was negotiated, and the converter — which scales anyway —
    /// absorbs the difference. A decoder reconfiguring mid-stream is a second
    /// black screen, and the viewer asked for neither.
    ///
    /// And it retries, for as long as the session runs. During a mode set the
    /// output is genuinely absent for a moment, so the first attempt failing is
    /// the normal case, not an error. Giving up there is what left the viewer
    /// staring at a dead stream with the host showing a "Keep these display
    /// settings?" dialog they could no longer reach — the one thing that has
    /// to keep working through a mode change is the ability to click Revert.
    ///
    /// ── The locked screen ───────────────────────────────────────────────────
    ///
    /// Win+L, a screensaver, a UAC prompt: Windows switches to the secure
    /// desktop, the duplication is lost, and DXGI refuses to open a new one
    /// until the user's desktop is back — for as long as that takes. The first
    /// version bounded the wait at ten seconds, so locking the PC from the
    /// stream ended the stream. Now the loop waits (RestartBackoff.h says how),
    /// and while it waits it keeps the stream ALIVE: the browser declares
    /// starvation after a second of silence and walks its quality ladder down
    /// on a session that is merely locked, so something has to go out.
    ///
    /// What goes out is a black picture, at the still-screen floor, encoded by
    /// the encoder the session still has. Black rather than the last desktop:
    /// a frozen desktop looks like a hang, and the viewer who has just pressed
    /// Win+L would press it again. Black says "the screen went away", which is
    /// the truth. Input keeps flowing the whole time — SendInput reaches the
    /// secure desktop — so a password typed into the dark unlocks the host,
    /// the duplication reopens, and the first frame back is a keyframe.
    ///
    /// The old converter and encoder are released only once the duplication
    /// is open again, and before the new pipeline is built: the encoder holds
    /// a hardware session — a consumer GPU has famously few — and building the
    /// replacement while the old one is still open is how a rebuild fails with
    /// a vendor error that says nothing about the real cause.
    enum class Restart
    {
        /// Capturing again on a fresh pipeline.
        Restarted,
        /// stop() was called while waiting. Nothing has been reported.
        Stopped,
        /// The session ended while waiting (an encode failed); finish() has
        /// already been called.
        Ended,
        /// The duplication reopened but the pipeline could not be rebuilt.
        /// @p error says why; nothing has been reported.
        Failed,
    };

    Restart restartCapture(uint32_t& frameNumber, int64_t floorIntervalUs, std::string& error)
    {
        // A retake's picture will not come: the capture it was asked of goes.
        m_RetakePending = false;

        // Pipelined: nothing in flight, and what went out taken in, before the
        // chain behind the capture goes.
        if (m_Pipeline->pipelined()) {
            m_Pipeline->settle();
            if (!absorbDelivered()) return Restart::Ended;
        }

        // The longest the wait may sleep in one go, so a stop() is noticed and
        // a floor due sooner than the next attempt is met.
        constexpr int64_t kMaxSleepUs = 100 * 1000;
        // When "still waiting" is said once more in the log, so a session log
        // read afterwards shows a locked screen rather than a hung loop.
        constexpr int64_t kStillWaitingUs = 10 * 1000 * 1000;

        // The picture sent while the screen is away. Made now, on the device
        // the duplication was opened on — start() replaces that device, and a
        // failed start() leaves none at all — and the size of what was being
        // captured, which is what the converter is set up to take. Failing to
        // make one is not fatal: the last picture is re-sent instead, as the
        // idle floor does. Held by the pipeline until this function returns,
        // whichever way it does — after the rebuild, as it always was. Through
        // the pointer, not the object: the rebuild may replace the pipeline
        // (D3D12 to D3D11), and the one released is whichever is in place.
        struct BlankRelease
        {
            std::unique_ptr<WindowsVideoPipeline>& pipeline;
            ~BlankRelease()
            {
                if (pipeline) pipeline->releaseBlank();
            }
        } blankRelease{m_Pipeline};
        bool blank = m_Pipeline->prepareBlank(m_Capture.get(), error);
        if (!blank)
            log::warning("[native] no blank picture for the wait, re-sending the last: " + error);
        bool blankShown = false;

        const int64_t lostUs = steadyNowUs();
        int64_t lastSentUs = 0;
        int64_t nextTryUs = lostUs;
        int failures = 0;
        bool saidStillWaiting = false;
        // Tried once per restart (setClientModeAgain).
        bool modeSetAgain = false;
        for (;;) {
            if (!m_Running.load()) return Restart::Stopped;

            int64_t nowUs = steadyNowUs();
            if (nowUs >= nextTryUs) {
                // The display back in another size than the client's mode
                // this session holds: that mode first, then the duplication.
                if (!modeSetAgain) modeSetAgain = setClientModeAgain();
                // Through openCapture(), not m_Capture->start(): the reason the
                // display went away may be the reason Desktop Duplication can
                // serve it again — or stop being able to. A driver restart or a
                // mode change is exactly when the right backend changes, and
                // re-running the choice costs one failed DDA attempt.
                if (openCapture(error)) break;
                failures++;
                nowUs = steadyNowUs();
                nextTryUs = nowUs + restartRetryDelayMs(failures) * 1000;
                if (failures == 1)
                    log::info("[native] display is away (reconfiguring, or locked), waiting for "
                              "it: " +
                              error);
            }
            if (!saidStillWaiting && nowUs - lostUs >= kStillWaitingUs) {
                saidStillWaiting = true;
                log::info("[native] display still away after 10 s, keeping the stream alive "
                          "until it returns");
            }

            // The floor, from the very first failure: the viewer's screen goes
            // dark the moment the host's did, not half a second later.
            if (m_Pipeline->built() && (lastSentUs == 0 || nowUs - lastSentUs >= floorIntervalUs)) {
                if (blank && !blankShown) {
                    if (m_Pipeline->convertBlank(cursorDraw(), error)) {
                        blankShown = true;
                    } else {
                        log::warning("[native] could not draw the blank picture: " + error);
                        m_Pipeline->releaseBlank();
                        blank = false;
                    }
                }
                if (!emit(frameNumber, resendStamps(nowUs), error)) return Restart::Ended;
                lastSentUs = nowUs;
                nowUs = steadyNowUs();
            }

            int64_t sleepUs = nextTryUs - nowUs;
            if (lastSentUs != 0 && lastSentUs + floorIntervalUs - nowUs < sleepUs)
                sleepUs = lastSentUs + floorIntervalUs - nowUs;
            if (sleepUs > kMaxSleepUs) sleepUs = kMaxSleepUs;
            if (sleepUs < 1000) sleepUs = 1000;
            std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
        }

        if (failures > 0) {
            char span[32];
            std::snprintf(span, sizeof(span), "%.1f", (steadyNowUs() - lostUs) / 1e6);
            log::info("[native] display is back after " + std::string(span) + " s and " +
                      std::to_string(failures) + " failed attempt" + (failures > 1 ? "s" : ""));
        }

        // The frame keeps its size — unless the viewer follows the display's
        // shape and the mode change moved it (SessionConfig::followDisplayShape):
        // then the same height at the new shape, and the client's decoder
        // follows on the keyframe the loop forces after a restart. Followed or
        // not, it is never larger than the display (frameForDisplay): a mode
        // that no longer holds the frame brings it down. The load cap keeps its
        // percentage of the new full size.
        int frameWidth = m_Info.width;
        int frameHeight = m_Info.height;
        FrameSize full{m_FullWidth, m_FullHeight};
        const FrameSize display{m_Capture->width(), m_Capture->height()};
        if (m_Config.followDisplayShape || full.width > display.width ||
            full.height > display.height) {
            // From the size the session was set up with, not the current one: a
            // display that shrank below it would otherwise keep the frame small
            // once it grew back (1920x1080 -> 1280x960 -> 1706x960 on the
            // portal's CPU pair, 15/09/2026).
            full = frameForDisplay(display, shapeBase(), policyOf(m_Config));
            if (full.width != m_FullWidth || full.height != m_FullHeight) {
                log::info("[native] the display is now " + std::to_string(m_Capture->width()) +
                          "x" + std::to_string(m_Capture->height()) + " — the stream follows it: " +
                          std::to_string(m_FullWidth) + "x" + std::to_string(m_FullHeight) +
                          " -> " + std::to_string(full.width) + "x" + std::to_string(full.height));
                frameWidth = encode::EncodeLoadCap::scaled(full.width, m_LoadCap.percent());
                frameHeight = encode::EncodeLoadCap::scaled(full.height, m_LoadCap.percent());
            }
        }

        // buildPipeline() releases the old converter and encoder before it
        // builds the new ones — see there.
        if (!buildPipeline(frameWidth, frameHeight, error)) {
            if (frameWidth == m_Info.width && frameHeight == m_Info.height) return Restart::Failed;
            // The new shape would not build: the size that worked still does,
            // stretched as before this setting existed.
            log::warning("[native] cannot encode at " + std::to_string(frameWidth) + "x" +
                         std::to_string(frameHeight) + " (" + error + ") — staying at " +
                         std::to_string(m_Info.width) + "x" + std::to_string(m_Info.height));
            if (!buildPipeline(m_Info.width, m_Info.height, error)) return Restart::Failed;
        } else {
            // What the cap scales from from now on.
            m_FullWidth = full.width;
            m_FullHeight = full.height;
        }
        m_Info.width = m_Pipeline->outputWidth();
        m_Info.height = m_Pipeline->outputHeight();
        // The backend too: a restart may have fallen back to WGC, or left it.
        m_Info.capture = m_CaptureApi;
        noteDesktopRect();

        // Absolute mouse input is aimed at the display's rectangle on the
        // virtual desktop, and a resolution change is exactly what moves it.
        // Under the lock that guards inject(), which runs on the network thread.
        {
            const capture::DesktopRect& rect = m_Capture->desktopRect();
            std::lock_guard<std::mutex> lock(m_InputMutex);
            if (m_Input) m_Input->setDisplayRect(rect.left, rect.top, rect.right, rect.bottom);
        }

        // The desktop changed size while the frame did not, so the pointer's
        // scale just moved (see CursorUpdate::scale) — and DXGI will not mention
        // the pointer again until its SHAPE changes, which for a cursor sitting
        // still may be never.
        m_ResendCursor.store(true);

        log::info("[native] capture restarted at " + std::to_string(m_Capture->width()) + "x" +
                  std::to_string(m_Capture->height()) + ", streaming " +
                  std::to_string(m_Info.width) + "x" + std::to_string(m_Info.height));
        // Another mode, another backend, or the same display after a wait:
        // the presents' phase is not the one the lattice followed.
        armCadenceLock();
        logCadenceLock(" (a new lattice with the capture)");
        reportDisplayFormat();
        return Restart::Restarted;
    }

    /// Where the display this session streams is among its adapter's outputs
    /// NOW, into m_Target.outputIndex — before every open of the capture,
    /// because Windows moves that index when another display becomes primary
    /// (OutputMatch.h: a Mac's stream of Display 1 that jumped to the virtual
    /// display an iPhone had just started on, N95, 16/10/2026). False with
    /// @p error when the display is not there: the open fails, and a restart
    /// waits for it as it waits out a locked screen.
    bool locateOutput(std::string& error)
    {
        // Nothing to go by (a target built without the probe's display): the
        // probe's index, as before there was anything else.
        if (m_Target.displayKey.empty() && m_Target.displayOsName.empty()) return true;

        std::vector<capture::OutputIdentity> outputs;
        if (!platform::adapterOutputs(m_Target.captureAdapterHandle, outputs, error)) return false;
        const int index =
            capture::outputIndexOf(outputs, m_Target.displayKey, m_Target.displayOsName);
        if (index < 0) {
            error =
                "the display this stream shows is not connected (" +
                (m_Target.displayOsName.empty() ? m_Target.displayKey : m_Target.displayOsName) +
                ")";
            return false;
        }

        const unsigned now = static_cast<unsigned>(index);
        const std::string& name = outputs[now].gdiName;
        if (!m_OutputLocated) {
            log::info("[native] capture: " + name + ", output " + std::to_string(now) +
                      " of its adapter (" + m_Target.displayKey + ")");
        } else if (now != m_Target.outputIndex || name != m_Target.displayOsName) {
            log::info("[native] the display moved among its adapter's outputs: " +
                      m_Target.displayOsName + " at " + std::to_string(m_Target.outputIndex) +
                      " -> " + name + " at " + std::to_string(now) + " — the capture stays on it");
        }
        m_OutputLocated = true;
        m_Target.outputIndex = now;
        // The name as it stands, for findOutput() and as the stand-in should
        // the paths go missing. Matched by name, it is the same one; matched
        // by path, the path stays the key.
        if (!name.empty()) m_Target.displayOsName = name;
        return true;
    }

    /// The output this session captures, as DXGI enumerates it — for what the
    /// frames do not say about the display. Null when it cannot be found,
    /// which a display that just went away is allowed to be.
    Microsoft::WRL::ComPtr<IDXGIOutput6> findOutput() const
    {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;
             factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
             ++i) {
            DXGI_ADAPTER_DESC1 desc = {};
            if (FAILED(adapter->GetDesc1(&desc))) continue;
            const uint64_t luid =
                (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
                static_cast<uint64_t>(desc.AdapterLuid.LowPart);
            if (luid != m_Target.captureAdapterHandle) continue;
            // By the GDI name rather than the index: between Windows moving
            // the outputs and the restart that follows (locateOutput), the
            // index is another display's.
            const std::wstring name(m_Target.displayOsName.begin(),
                                    m_Target.displayOsName.end()); // ASCII, "\\.\DISPLAYn"
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            for (UINT j = 0;
                 adapter->EnumOutputs(j, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
                 ++j) {
                if (name.empty()) {
                    if (j != m_Target.outputIndex) continue;
                } else {
                    DXGI_OUTPUT_DESC outputDesc = {};
                    if (FAILED(output->GetDesc(&outputDesc)) ||
                        ::_wcsicmp(outputDesc.DeviceName, name.c_str()) != 0)
                        continue;
                }
                Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
                if (FAILED(output.As(&output6))) return nullptr;
                return output6;
            }
            return nullptr;
        }
        return nullptr;
    }

    /// Whether the display is in an HDR mode right now, asked of the output
    /// itself. Not read off the capture: the WGC fallback is 8-bit whatever
    /// the desktop is, so its frames never say.
    bool displayHdrActive() const
    {
        const Microsoft::WRL::ComPtr<IDXGIOutput6> output = findOutput();
        if (!output) return false;
        DXGI_OUTPUT_DESC1 desc1 = {};
        if (FAILED(output->GetDesc1(&desc1))) return false;
        return desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    }

    /// Where the desktop's SDR white sits in scRGB on this display: the "SDR
    /// content brightness" slider, as DISPLAYCONFIG_SDR_WHITE_LEVEL reports it
    /// — 1000 is 80 nits, scRGB 1.0. The frames do not carry it, and it is the
    /// one number that decides whether an SDR stream of an HDR desktop is right
    /// or blown out. 1.0 when it cannot be read, which is the 80-nit default.
    ///
    /// Joined to the output the way the probe joins its modes: by the source's
    /// GDI device name, which DXGI and QueryDisplayConfig both report.
    float readSdrWhite() const
    {
        const Microsoft::WRL::ComPtr<IDXGIOutput6> output = findOutput();
        if (!output) return 1.0f;
        DXGI_OUTPUT_DESC desc = {};
        if (FAILED(output->GetDesc(&desc))) return 1.0f;

        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) !=
            ERROR_SUCCESS)
            return 1.0f;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                                 modes.data(), nullptr) != ERROR_SUCCESS)
            return 1.0f;

        for (UINT32 i = 0; i < pathCount; ++i) {
            const DISPLAYCONFIG_PATH_INFO& path = paths[i];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id = path.sourceInfo.id;
            if (::DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
            if (::wcscmp(source.viewGdiDeviceName, desc.DeviceName) != 0) continue;

            // A cloned source has several targets; the first that answers is
            // the desktop's level, they all render the same framebuffer.
            DISPLAYCONFIG_SDR_WHITE_LEVEL white = {};
            white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            white.header.size = sizeof(white);
            white.header.adapterId = path.targetInfo.adapterId;
            white.header.id = path.targetInfo.id;
            if (::DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) continue;
            return static_cast<float>(white.SDRWhiteLevel) / 1000.0f;
        }
        return 1.0f;
    }

    /// Hand the converter a freshly read SDR white, and say so when it moved.
    /// Capture thread only.
    void applySdrWhite(float white)
    {
        if (white < 1.0f) white = 1.0f;
        if (white == m_SdrWhite) return;
        m_SdrWhite = white;
        m_Pipeline->setSdrWhite(white);
        char nits[16] = {};
        std::snprintf(nits, sizeof(nits), "%.0f", static_cast<double>(white) * 80.0);
        log::info("[native] the desktop's SDR white is " + std::string(nits) + " nits" +
                  (m_Pipeline->toneMapsToSdr() ? " — the tone map brings it to white"
                                               : " — the pointer is drawn at it"));
    }

    /// Tell the viewer what the display became, once the capture runs on it
    /// again — only when something it can act on moved. See DisplayFormat.
    void reportDisplayFormat()
    {
        DisplayFormat format;
        format.displayWidth = m_Capture->width();
        format.displayHeight = m_Capture->height();
        format.frameWidth = m_Info.width;
        format.frameHeight = m_Info.height;
        format.displayHdr = displayHdrActive();
        format.hdr = m_Info.hdr;
        format.hdrCapable = m_Info.hdrCapable;

        std::lock_guard<std::mutex> lock(m_FormatMutex);
        if (format.displayWidth == m_LastFormat.displayWidth &&
            format.displayHeight == m_LastFormat.displayHeight &&
            format.frameWidth == m_LastFormat.frameWidth &&
            format.frameHeight == m_LastFormat.frameHeight &&
            format.displayHdr == m_LastFormat.displayHdr && format.hdr == m_LastFormat.hdr)
            return;
        m_LastFormat = format;
        log::info("[native] display format: " + std::to_string(format.displayWidth) + "x" +
                  std::to_string(format.displayHeight) + (format.displayHdr ? " HDR" : " SDR") +
                  ", streaming " + std::to_string(format.frameWidth) + "x" +
                  std::to_string(format.frameHeight) + (format.hdr ? " HDR" : " SDR"));
        if (m_OnDisplayFormat) m_OnDisplayFormat(format);
    }

    /// The frame a display change is measured from — see restartCapture.
    FrameSize shapeBase() const
    {
        return m_Config.width > 0 && m_Config.height > 0
                   ? FrameSize{m_Config.width, m_Config.height}
                   : FrameSize{m_FullWidth, m_FullHeight};
    }

    /// The thread entry point. Nothing may escape it.
    ///
    /// An exception leaving a std::thread calls std::terminate, which aborts
    /// the whole worker PROCESS with no usable dump — 0xC0000409 raised from
    /// inside ucrtbase, past any handler. That is how a single bad frame took
    /// down a session and left nothing to read. Whatever goes wrong, it is
    /// turned into an ended session with a reason.
    void run() noexcept
    {
        // MMCSS "Games": the scheduler gives this thread the same standing as
        // a game's render thread, ahead of the desktop's background work, for
        // the whole life of the session. Refusal (a service session, a policy)
        // is logged once and costs nothing — the loop is the same either way.
        DWORD mmcssTask = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Games", &mmcssTask);
        if (mmcss)
            log::info("capture thread on MMCSS Games");
        else
            log::info("MMCSS Games refused for the capture thread (error " +
                      std::to_string(GetLastError()) + ") — ordinary priority");
        // On the input desktop from its first picture: a session started while
        // the secure desktop was up had its duplication opened there
        // (startCapture), and this thread reads it.
        platform::attachThread();
        try {
            runLoop();
            logCadence();
        } catch (const std::exception& e) {
            finish(std::string("the capture loop threw: ") + e.what());
        } catch (...) {
            finish("the capture loop threw an unknown exception");
        }
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }

    void runLoop()
    {
        // The capture timeout only bounds how long the loop sleeps when nothing
        // is presented; it is not a frame deadline. Short enough to notice a
        // stop() promptly, long enough that an idle desktop costs nothing.
        constexpr int kAcquireTimeoutMs = 100;

        // How long a perfectly still desktop may go without sending anything,
        // when the client has asked for nothing in particular.
        //
        // Desktop Duplication delivers frames on damage, so a screen where
        // nothing moves produces nothing at all — which is efficient and, to
        // the receiver, indistinguishable from a broken stream. The browser
        // declares starvation after 1000 ms and asks for a keyframe; that
        // request storm is then read as congestion and walks the quality ladder
        // all the way down, on a session that was never in trouble.
        //
        // So: a floor, comfortably inside that window. What goes out is the
        // frame already converted and unchanged, which an encoder turns into
        // almost nothing — a few hundred bytes of "everything is the same".
        //
        // This one only answers "is the stream alive", and it is deliberately
        // the CAPTURE's number rather than the client's: how long a still screen
        // may stay silent is a property of how exactly this platform reports
        // damage. On Desktop Duplication that report is exact — AcquireNextFrame
        // returns on the real present, 0.06 ms after it — so nothing is lost by
        // waiting here, and a client at a desk asks for no more. A platform
        // whose damage signal is vaguer raises this, and no client learns of it.
        //
        // A client that wants MORE than liveness — a game paused with the
        // pointer locked, where the picture is the only channel there is — asks
        // for a faster floor; see setFrameFloorFps.
        constexpr int64_t kIdleFloorUs = 500 * 1000;

        // ── Refining a picture that stopped moving ──────────────────────────
        //
        // The rate control is constant-bitrate and its budget is PER FRAME: a
        // frame may spend one VBV and no more. That bound is right while frames
        // keep coming, because the next one refines what this one could only
        // approximate — nobody ever sees the intermediate state.
        //
        // On a desktop that stops moving, nothing follows. The single frame that
        // carried the change IS the picture, quantized to fit one frame's
        // budget, and it stays that way on screen for as long as the user reads
        // it. That is the softness: not a wrong setting, a refinement that never
        // happened because the scene had no next frame to carry it.
        //
        // So the next frames are supplied, AND they are given something to spend.
        // Two halves, and the first alone does nothing:
        //
        //  - more frames. The converted texture is re-encoded at the stream's
        //    cadence for a short while after the last real capture, so the
        //    encoder codes the residual against its own reconstruction and each
        //    pass adds the detail the previous one had to drop;
        //  - a bigger budget, for exactly as long as that lasts. The measurement
        //    that mattered: after the VBV floor, a 1080p keyframe came out at
        //    112 KB against a 114 KB cap. Pinned, again. Handing the same
        //    ceiling to sixty more frames would have produced sixty more frames
        //    of the same softness — the encoder was never short of chances, it
        //    was short of bits. See encode/RateControl.h.
        //
        // Latency is bounded, not untouched. A VBV bounds how long a frame
        // occupies the link, which protects the frame AFTER it — and while the
        // screen is still, there is none. The moment something moves the
        // ordinary budget is restored BEFORE that frame is encoded. But the
        // passes already handed to the link are still in front of that frame,
        // and a burst sent as fast as the encoder produces it puts the WHOLE
        // burst there: a few hundred kilobytes, which on a 20 Mbps link is a
        // quarter of a second before the first frame of the movement arrives.
        // So a pass goes out only once the link has drained the previous one,
        // by the stream's own rate (LinkOccupancy) — the burst never runs more
        // than one pass ahead, and the boost is sized so that one pass is
        // 50 ms of link at most (kStillBoost). That is the price a badly timed
        // mouse pays, once.
        //
        // Three ways out, whichever comes first: the window, convergence, and
        // anything at all happening on screen.
        constexpr int64_t kRefineWindowUs = 1000 * 1000;
        // How still the screen must be before any of this starts. Without it,
        // the pause between two keystrokes counts as a still screen and the
        // budget is reconfigured twice per character typed. Short enough that a
        // screen someone is actually reading has settled long before they look.
        constexpr int64_t kRefineDelayUs = 150 * 1000;
        constexpr int kRefineMaxFps = 60;
        // "The encoder had nothing left to add" is decided by
        // encode::RefineConvergence — a tiny pass, a QP that stopped falling,
        // or the ceiling on passes — see there for why size alone was wrong.

        const int refineFps =
            (m_Config.fps > 0 && m_Config.fps < kRefineMaxFps) ? m_Config.fps : kRefineMaxFps;
        const int64_t refineIntervalUs = 1000000 / refineFps;
        // The acquire timeout is also the loop's sleep, so it has to be short
        // enough to let a refinement pass be due on time. Outside the window it
        // stays long: an idle desktop must not cost a wake-up every 16 ms.
        const int refineTimeoutMs = static_cast<int>(refineIntervalUs / 1000);

        uint32_t frameNumber = 0;
        // Pipelined, a job on the encode thread holds the counter above while
        // it runs: none may be running once the loop is gone.
        struct SettleOnExit
        {
            std::unique_ptr<WindowsVideoPipeline>& pipeline;
            ~SettleOnExit()
            {
                if (pipeline) pipeline->settle();
            }
        } settleOnExit{m_Pipeline};
        std::string error;
        int64_t lastSentUs = steadyNowUs();
        // The last frame that came from an actual capture — what the refinement
        // window is measured from.
        int64_t lastRealUs = lastSentUs;
        encode::RefineConvergence refineConv;
        bool refineDone = false;
        // The QP the picture started the burst at, for the log: first → last
        // is the sharpening the burst bought, in the encoder's own unit.
        int refineFirstQp = -1;
        int refinePasses = 0;
        // Wake-ups at which a pass was due but the link had not drained the
        // previous one. In the log next to the passes: the pair says whether
        // the burst was shaped by convergence or by the link.
        int refineHeld = 0;
        size_t refineBytes = 0;
        size_t refineFirstBytes = 0;
        int refineLogged = 0;

        // The client's floor, as an interval, clamped by the stream's own rate.
        //
        // Recomputed every iteration rather than cached: it changes when the
        // viewer switches mouse mode, and that arrives on another thread.
        //
        // The clamp is the important half. A client asking for 30 fps on a
        // stream the viewer set to 20 must get 20 — the setting is the user's
        // own words about what their link can carry, and a floor is a request
        // from a page. Only a lower interval than the liveness floor is ever
        // taken, so a client cannot ask to be sent LESS than the receiver needs
        // to tell this stream from a dead one.
        auto floorIntervalUs = [this, kIdleFloorUs]() -> int64_t {
            int fps = m_FloorFps.load(std::memory_order_relaxed);
            if (fps <= 0) return kIdleFloorUs;
            if (m_Config.fps > 0 && fps > m_Config.fps) fps = m_Config.fps;
            const int64_t interval = 1000000 / fps;
            return interval < kIdleFloorUs ? interval : kIdleFloorUs;
        };

        // The stream's own bitrate, which the quality ladder may move under us,
        // and whether the still-screen budget is currently in its place.
        // ── Three layers decide what the encoder is told ────────────────────
        //
        //  1. the viewer's CEILING (the setting, moved by the frontend's ladder
        //     through setTargetBitrate);
        //  2. what the LINK can take right now, under it — the governor, fed by
        //     the receiver's reports (encode::RateGovernor). That is `baseKbps`,
        //     the rate the wire really carries and the link model is fed with;
        //  3. the budget PER FRAME for the rate frames really come at
        //     (encode::EffectiveCadence), and the still-screen boost on top.
        //
        // Every change goes through applyBitrate(), so the three never disagree
        // about what the encoder holds.
        encode::RateGovernor governor;
        governor.start(m_Config.bitrateKbps, steadyNowUs() / 1000,
                       m_Config.tuning.linkGovernor == EncoderTuning::Choice::Off,
                       m_Config.governorFloorPercent);
        // retrcut= (plan Wi-Fi W2 B): SCTP's retransmissions as a reason to
        // cut — on a Mac in Wi-Fi, the browser's full socket, which the
        // receiver's delay rise never shows. The engine's own unless named.
        governor.setRetransCut(m_Config.tuning.retransCutPermille >= 0
                                   ? m_Config.tuning.retransCutPermille
                                   : encode::RateGovernor::kRetransCutPermille);
        if (governor.retransCut() > 0)
            log::info("[native] rate governor: also cuts at " +
                      std::to_string(governor.retransCut()) +
                      " SCTP chunks retransmitted in a thousand (retrcut=)");
        // A shed frame lowers the good rate: the product's rule since
        // 21/10/2026 (plan « Gels du lien », G; overflowLowersGoodByDefault),
        // ovfgood=0 the bench's "before".
        governor.setOverflowLowersGood(overflowLowersGoodByDefault(m_Config.tuning.overflowGood));
        if (governor.overflowLowersGood())
            log::info("[native] rate governor: a frame shed off the send buffer lowers the "
                      "link's last good rate (ovfgood=0 takes it out)");
        else
            log::info("[native] rate governor: shed frames move nothing (bench ovfgood=0)");
        // The session's account of the link, however the loop ends: what the
        // encoder was given on average, and why it was cut (the A/B of
        // ovfgood=; the moves themselves are logged ten at most, then every
        // tenth).
        struct GovernorSummary
        {
            const encode::RateGovernor& g;
            ~GovernorSummary()
            {
                if (g.changes() == 0) return;
                log::info("[native] rate governor this session: target " +
                          std::to_string(g.meanTargetKbps(steadyNowUs() / 1000)) +
                          " kbps on average of the " + std::to_string(g.settingKbps()) + " set, " +
                          std::to_string(g.changes()) + " moves, " + std::to_string(g.overuses()) +
                          " overuses (" + std::to_string(g.retransOveruses()) +
                          " on SCTP's retransmissions, " + std::to_string(g.overflows()) +
                          " on shed frames), " + std::to_string(g.silences()) +
                          " silences, last good rate " + std::to_string(g.goodKbps()) + " kbps");
            }
        } governorSummary{governor};
        if (m_Config.tuning.linkHoldMs > 0)
            log::info("[native] link hold: a picture waits, unencoded, once video has waited "
                      "outside usrsctp " +
                      std::to_string(m_Config.tuning.linkHoldMs) + " ms (bench linkhold=)");
        int baseKbps = governor.targetKbps();
        m_LinkKbps = baseKbps;
        bool boosted = false;
        // A pipeline rebuilt by the load cap with no desktop copy to fill it:
        // nothing is re-sent until a capture has converted a picture (see the
        // resize below).
        bool awaitingPicture = false;
        // When the SDR white level was last asked of the display, for the FP16
        // paths. buildPipeline read it once; this keeps up with the slider.
        int64_t lastSdrWhiteUs = steadyNowUs();
        encode::EffectiveCadence effective;
        effective.start(m_EncodeFps, steadyNowUs());
        // A refusal is said once: D3D12 Video Encode on the Arc refuses every
        // change (a new sequence or our own rate control, Phase 6), and the
        // still-screen boost asks twice per pause of the mouse.
        std::string bitrateRefused;
        auto applyBitrate = [&](int kbps) {
            if (kbps <= 0) return;
            const int held = effective.scaledKbps(kbps);
            if (m_Pipeline->setBitrate(held, error)) {
                m_EncoderKbps = held;
                bitrateRefused.clear();
            } else if (error != bitrateRefused) {
                bitrateRefused = error;
                log::warning("[native] bitrate change refused: " + error);
            }
        };
        int cadenceLogged = 0;
        int governorLogged = 0;
        // The governor moved the wire's rate: the link model and whatever the
        // encoder currently holds (boosted or not) follow.
        auto applyGovernor = [&](const char* why) {
            baseKbps = governor.targetKbps();
            m_LinkKbps = baseKbps;
            applyBitrate(boosted ? encode::stillBitrateKbps(baseKbps) : baseKbps);
            if (governorLogged < 10 || governor.changes() % 10 == 0) {
                governorLogged++;
                log::info("[native] link: " + std::string(why) + " — encoding at " +
                          std::to_string(baseKbps) + " kbps of the " +
                          std::to_string(governor.settingKbps()) + " set");
            }
        };

        // The first few bursts, then silence. These are the numbers that say
        // whether any of this worked: what one frame's budget bought, against
        // what the picture actually converged to, and how often the link — not
        // the encoder — set the pace. More than a handful would be noise:
        // bursts happen every time the screen settles.
        //
        // Every way out is logged, not only convergence. A burst that ran to
        // the end of its window without ever going quiet used to leave no
        // trace at all — which is how a 55 Mbps stream spent 3 MB on every
        // pause of the mouse for weeks without anyone knowing — and a log that
        // only reports success is not a log of what happened.
        auto closeBurst = [&](const char* how) {
            if (refinePasses == 0) return;
            if (refineLogged < 3) {
                refineLogged++;
                std::string qp;
                if (refineFirstQp >= 0 && m_LastEmitQp >= 0)
                    qp = ", QP " + std::to_string(refineFirstQp) + " -> " +
                         std::to_string(m_LastEmitQp);
                log::info(
                    "[native] still picture refined: " + std::to_string(refineFirstBytes / 1024) +
                    " KB + " + std::to_string(refineBytes / 1024) + " KB over " +
                    std::to_string(refinePasses) + " passes, " + std::to_string(refineHeld) +
                    " held for the link" + qp + " (" + how + ")");
            }
            refinePasses = 0;
        };

        // A new picture is on the wire: whatever burst was running is over, and
        // the next one starts from this picture's own figures.
        auto resetBurst = [&]() {
            refineConv.reset();
            refineDone = false;
            refinePasses = 0;
            refineHeld = 0;
            refineBytes = 0;
            refineFirstBytes = m_LastEmitBytes;
            refineFirstQp = m_LastEmitQp;
        };

        // A picture changed — a present, or the pointer moving over a still
        // desktop — and went out. The refinement window is measured from here.
        auto noteReal = [&]() {
            closeBurst("screen moved");
            lastSentUs = steadyNowUs();
            lastRealUs = lastSentUs;
            resetBurst();
        };

        // ── Holding the loop to the stream's rate ───────────────────────────
        //
        // The display presents at its own refresh rate and the stream has a
        // rate of its own; when the display is faster, only the FIRST present
        // at or after each tick of the stream's grid is encoded — the moment
        // it arrives, never later. The others are converted all the same (the
        // converter's output texture is what the still-screen paths re-send,
        // so it has to be the freshest picture) but not encoded. Nothing ever
        // waits on this thread for a tick: a picture held on the host is
        // latency the viewer feels. The reasoning is on FrameCadence.
        // A real picture went out: one more frame in this second's count. When
        // the second closes on a different rate, the encoder's budget follows
        // — through the same setBitrate() the boost and the ladder use, so the
        // three never disagree about what the encoder holds.
        auto countPicture = [&]() {
            m_Hold.carried();
            if (!effective.noteFrame(steadyNowUs())) return;
            applyBitrate(boosted ? encode::stillBitrateKbps(baseKbps) : baseKbps);
            if (cadenceLogged < 5 || effective.changes % 30 == 0) {
                cadenceLogged++;
                log::info("[native] frames arrive at " + std::to_string(effective.currentFps) +
                          " fps for a " + std::to_string(effective.configuredFps) +
                          " fps stream — encoder budget " +
                          (effective.scaling()
                               ? std::to_string(effective.scaledKbps(baseKbps)) +
                                     " kbps per second of frames (" + std::to_string(baseKbps) +
                                     " on the wire)"
                               : std::string("back to ") + std::to_string(baseKbps) + " kbps"));
            }
        };
        // cadence=host-guarded (design §33): a picture the gate admitted is
        // held back while the client says its decode queue is full, and the
        // one held — always the freshest, the converter keeps nothing older —
        // goes out the moment the credit is back, not at the next present.
        const bool guarded = m_Config.tuning.cadence == EncoderTuning::Cadence::HostGuarded;
        bool creditHeld = false;
        FrameStamps creditHeldStamps;
        // cadence=deadline: this turn's picture was taken at the instant the
        // client's grid named (DeadlineCadence.h). It is the one for its
        // refresh — the gate has nothing to say about it.
        const bool deadlineMode = m_Config.tuning.cadence == EncoderTuning::Cadence::Deadline;
        bool aimedNow = false;
        // A client that tears is sent each picture as it comes, through a gate
        // at its budget (DeadlineCadence.h) that skips and never holds. It
        // stands in for the stream's gate while that client's grid is fresh.
        FrameCadence tearGate{0};
        double tearGateFps = 0;
        int tearGateWhy = 0; // 1: the canvas tears, 2: the link is uneven
        FrameCadence* gateNow = &m_Cadence;
        struct TimerHandle
        {
            HANDLE h = nullptr;
            ~TimerHandle()
            {
                if (h) CloseHandle(h);
            }
        } deadlineTimer;
        if (deadlineMode)
            deadlineTimer.h = CreateWaitableTimerExW(
                nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        // linkhold= (plan Wi-Fi W2 C): the same hold, for the relay's send
        // queue instead of the client's decoder. Video waiting outside
        // usrsctp is a frame the link has not taken yet; a picture encoded
        // now would only wait behind it, so it is held, and the freshest
        // goes the moment the queue drained. No reference is ever missing:
        // a picture never encoded is no hole.
        const bool linkHold = m_Config.tuning.linkHoldMs > 0;
        bool heldForLink = false;
        m_PreGate = m_Config.tuning.preGate == 1;
        if (m_PreGate)
            log::info("[native] pregate=1: the stream's gate is asked before a present is "
                      "converted, and a present it skips is not");
        auto creditMissing = [&]() {
            return (guarded && m_DecodeCredit.missing(steadyNowUs())) || (linkHold && linkBusy());
        };
        auto withheld = [&](const FrameStamps& stamps) -> bool {
            if (guarded && m_DecodeCredit.missing(steadyNowUs())) {
                m_CreditSkips++;
                heldForLink = false;
            } else if (linkHold && linkBusy()) {
                m_LinkHolds++;
                heldForLink = true;
            } else {
                return false;
            }
            creditHeld = true;
            creditHeldStamps = stamps;
            return true;
        };
        // The gate's word on a picture, and for a @p present the admission
        // window after a press on top of it (InputAdmit.h). Not
        // for a pointer that moved alone nor for a re-send: a window spent on
        // the cursor while the player aims would leave none for the shot.
        //
        // The stream's gate locks on the display's presents where it may
        // (FrameCadence.h, PresentLattice.h): it then decides on the
        // picture's own time, a present's, a pointer's wake-up or a re-send's
        // now. Whether it could have is counted either way, for the bench's
        // arm without the lock to say so too.
        auto gateSays = [&](const FrameStamps& stamps, bool present) -> bool {
            bool grid = false;
            if (gateNow == &m_Cadence) {
                m_GateCalls++;
                const bool lockable =
                    m_Lattice.engaged() && m_Cadence.lockRatio(m_Lattice.periodNs()) > 0;
                if (lockable) m_LockableCalls++;
                m_Hold.gateCall(lockable);
                grid = m_LockArmed
                           ? m_Cadence.admit(stamps.convertedUs, stamps.presentUs, m_Lattice.view())
                           : m_Cadence.admit(stamps.convertedUs);
                if (m_Cadence.lastRule() == FrameCadence::Rule::Empty)
                    m_LockedAfterEmpty++;
                else if (m_Cadence.lastRule() == FrameCadence::Rule::Pause)
                    m_LockedAfterPause++;
            } else {
                grid = gateNow->admit(stamps.convertedUs);
            }
            if (!present) return grid;
            InputAdmit::Verdict verdict =
                grid ? InputAdmit::Verdict::Grid : InputAdmit::Verdict::Skip;
            // Not under a ceiling — every refresh already goes — nor under the
            // gate of a client that tears (cadence=deadline), nor, the
            // product's, while the client paints at its vsync.
            if (m_InputAdmitOn && gateNow == &m_Cadence && m_Cadence.enabled() &&
                !m_Cadence.isCeiling() &&
                InputAdmit::forClient(m_Config.tuning.inputAdmit, m_Config.clientTearingReal,
                                      m_ClientVsync.load())) {
                const int64_t nowUs = stamps.convertedUs;
                const int64_t intervalUs = m_Cadence.intervalUs();
                // An encode slower than a refresh of the display: the window's
                // pictures, a refresh apart, would push the grid's later.
                const int64_t refreshUs =
                    m_DisplayMilliHz > 0 ? 1000000000LL / m_DisplayMilliHz : 0;
                const bool encoderOk =
                    refreshUs == 0 || m_EncodeP95Us.load(std::memory_order_relaxed) <= refreshUs;
                // A link the governor has cut, or heard a queue on, in the last
                // two seconds; or one the stream's own estimate says is still
                // busy past what a window adds (a keyframe, a refinement pass).
                const bool linkOk =
                    governor.calm(nowUs / 1000, encode::RateGovernor::kHoldAfterCutMs) &&
                    m_Link.backlogUs(nowUs) <= (m_InputAdmit.maxExtra() + 1) * intervalUs;
                verdict = m_InputAdmit.admit(
                    grid, stamps.presentUs, nowUs,
                    m_InputAdmit.gapFor(m_ClientMilliHz.load(), m_ClientVsync.load(), intervalUs),
                    linkOk, encoderOk);
            }
            if (verdict != InputAdmit::Verdict::Skip && m_Config.tuning.clickTrace) {
                const bool locked = gateNow == &m_Cadence && m_Cadence.lastLocked();
                const int64_t latticeUs =
                    m_Lattice.started() ? m_Lattice.view().slotNs(stamps.presentUs * 1000) / 1000
                                        : 0;
                m_ClickTrace.admit(verdict == InputAdmit::Verdict::Window ? "window" : "grid",
                                   stamps.convertedUs, stamps.presentUs, locked ? 1 : 0, latticeUs);
            }
            return verdict != InputAdmit::Verdict::Skip;
        };
        // @p gated: the gate already said yes to this picture, before its
        // conversion (pregate=1 below); it is not asked twice.
        auto emitPicture = [&](const FrameStamps& stamps, bool present,
                               bool gated = false) -> bool {
            if (!aimedNow && !gated && !gateSays(stamps, present)) return true;
            if (withheld(stamps)) return true;
            creditHeld = false;
            if (!emit(frameNumber, stamps, error)) return false;
            noteReal();
            countPicture();
            return true;
        };
        // The same, pipelined (plan Phase 10): the picture goes to the encode
        // thread and the loop goes back to the capture; the cadence counts it
        // once it went out (absorbDelivered), the refinement window starts now.
        auto emitPictureLater = [&](const FrameStamps& stamps, bool gated = false) {
            if (!aimedNow && !gated && !gateSays(stamps, true)) return;
            if (withheld(stamps)) return;
            creditHeld = false;
            emitLater(frameNumber, stamps);
            noteReal();
        };

        m_LoopStartUs = steadyNowUs();
        m_LoadCap.start(m_LoopStartUs);
        m_Hold.start(m_LoopStartUs, m_CadenceFps);
        while (m_Running.load()) {
            // Pipelined: what the encode thread delivered since the last turn.
            if (!absorbDelivered()) return;
            for (; m_DeliveredUncounted > 0; --m_DeliveredUncounted)
                countPicture();

            if (m_PipelineLost) {
                // The D3D12 chain failed while streaming (see emit): back to
                // D3D11 over a capture opened again — whose first frame is
                // the whole desktop, so the viewer's next picture is a full
                // one — with what a lost display goes through below.
                switch (restartCapture(frameNumber, floorIntervalUs(), error)) {
                case Restart::Restarted: break;
                case Restart::Ended: return;
                case Restart::Stopped:
                    finish("the session was stopped while the video pipeline was rebuilt");
                    return;
                case Restart::Failed:
                    finish("the video pipeline could not go back to D3D11: " + error);
                    return;
                }
                m_ForceKeyframe.store(true);
                boosted = false;
                applyBitrate(baseKbps);
                closeBurst("D3D12 lost");
                lastRealUs = steadyNowUs();
                resetBurst();
                continue;
            }

            if (const int kbps = m_PendingBitrate.exchange(0); kbps > 0) {
                // The ladder moves the CEILING, not the still-screen budget.
                // Applying it while boosted would drop the burst back to normal
                // mid-refinement; the boost is recomputed from the new base
                // instead, and the base takes over when the burst ends.
                governor.setSetting(kbps);
                applyGovernor("ceiling moved");
            }

            // The client's screen changed — another monitor, another refresh,
            // tearing switched. The gate is re-chosen against it (§9.11); the
            // encoder keeps the rate it was built for and its per-frame budget
            // follows through the same setBitrate() everything else uses, at
            // once, so a gate that just went from 60 to 72 does not spend a
            // second handing out 60-sized frames 72 times.
            //
            // A step of "Auto"'s detection (CadenceStep.h) takes the same road:
            // the gate moves to the step's rate between two frames, and the
            // budget per frame follows, so the bitrate on the wire stays the
            // setting's.
            if (m_ClientRefreshDirty.exchange(false)) {
                FrameCadence chosen{0};
                std::string line;
                const int fps =
                    chooseCadence(m_ClientMilliHz.load(), m_ClientVsync.load(), chosen, line);
                const int step = m_ChosenStepFps;
                const bool stepMoved = step != m_AppliedStepFps;
                if (stepMoved) noteStep(step);
                if (chosen.intervalUs() != m_Cadence.intervalUs() || fps != m_CadenceFps) {
                    m_Cadence = chosen;
                    m_CadenceFps = fps;
                    m_Hold.setFps(fps);
                    m_InputAdmit.setIntervalUs(m_Cadence.intervalUs());
                    m_WindowFramesAtGate = m_InputAdmit.frames();
                    log::info(line + (!stepMoved ? " (client screen changed mid-session)"
                                      : step > 0 ? " (step asked by the client's detection)"
                                                 : " (back to the client's own rate)"));
                    // The lattice stays: it is the display's, not the gate's.
                    logCadenceLock(" (a new gate)");
                    if (effective.retarget(fps))
                        applyBitrate(boosted ? encode::stillBitrateKbps(baseKbps) : baseKbps);
                } else if (m_Config.tuning.cadence != EncoderTuning::Cadence::Default) {
                    // The host's rate does not move, but what the client said
                    // is the trial's evidence: its ceilings, logged unapplied.
                    log::info(line + " (the client changed mid-session)");
                }
            }

            // The receiver's word on the link, and the silence of it.
            {
                LinkFeedback fb;
                const int64_t nowMs = steadyNowUs() / 1000;
                if (takeLinkFeedback(fb)) {
                    if (governor.report(fb, nowMs))
                        applyGovernor(
                            fb.resumed ? "the receiver is back from the background"
                            : fb.gaps > 0 || fb.evictions > 0 ? "frames lost"
                            : fb.overflows > 0 && governor.overflowLowersGood()
                                ? "send buffer overflowed"
                            : fb.owdRiseMs >= encode::RateGovernor::kOveruseMs ? "delay rising"
                            : governor.retransCut() > 0 &&
                                    fb.retransPermille >= governor.retransCut()
                                ? "SCTP retransmitting"
                            : governor.lastRaiseFast() ? "quiet, back to the link's last good rate"
                                                       : "quiet, raising");
                } else if (governor.tick(nowMs)) {
                    applyGovernor("no report from the receiver");
                }
            }

            // Decided before the acquire because it also chooses how long the
            // acquire may sleep. `refineSoon` keeps the loop responsive through
            // the settling delay as well, so a pass is not up to 100 ms late.
            const int64_t sinceRealUs = steadyNowUs() - lastRealUs;
            const bool refineSoon = sinceRealUs < (kRefineDelayUs + kRefineWindowUs) &&
                                    !refineDone && m_Pipeline->outputWidth() != 0;
            const bool refining = refineSoon && sinceRealUs >= kRefineDelayUs;
            // The window ran out on a burst still going: say so, once.
            if (!refineSoon && !refineDone) closeBurst("window closed");

            // The acquire timeout is the loop's sleep, so a floor faster than
            // it would simply never be met: at 15 fps the frame is due every
            // 66 ms and a 100 ms sleep delivers 10. It only ever shortens the
            // wait — a client that asked for nothing still sleeps the full
            // 100 ms on an idle desktop.
            const int64_t idleIntervalUs = floorIntervalUs();
            const int idleTimeoutMs = static_cast<int>(idleIntervalUs / 1000) < kAcquireTimeoutMs
                                          ? static_cast<int>(idleIntervalUs / 1000)
                                          : kAcquireTimeoutMs;
            // A picture held for the decode credit or the link is looked at
            // every millisecond: it goes the moment the credit is back.
            const int timeoutMs = creditHeld ? 1 : refineSoon ? refineTimeoutMs : idleTimeoutMs;

            // Between frames, so the encoder is not holding anything.
            //
            // A new converter holds a zeroed picture, flat green once decoded
            // (Y = U = V = 0), and on a still screen no capture comes to fill
            // it: the resize keyframe and every re-send after it were that green
            // until the desktop next moved (issue #15, reproduced 15/09/2026 on
            // the Linux engine, which shares this loop's shape). The desktop
            // copy goes in first; without one, nothing is re-sent until a
            // capture has.
            if (m_PendingResize.exchange(false) && applyLoadCap()) {
                if (m_Pipeline->hasHeld()) {
                    if (!m_Pipeline->convertHeld(*m_Capture, pointerToDraw(), cursorDraw(),
                                                 error)) {
                        finish("colour conversion failed: " + error);
                        return;
                    }
                    awaitingPicture = false;
                    if (!emitPicture(resendStamps(steadyNowUs()), false)) return;
                } else {
                    awaitingPicture = true;
                }
            }

            // The SDR brightness slider is live, and it is the one thing about
            // an HDR desktop the frames do not carry. Once a second, and only
            // on the paths that read it.
            if (m_Pipeline->scRgbSource()) {
                const int64_t nowUs = steadyNowUs();
                if (nowUs - lastSdrWhiteUs >= kSdrWhitePollUs) {
                    lastSdrWhiteUs = nowUs;
                    applySdrWhite(readSdrWhite());
                }
            }

            // The pointer changed hands: the held desktop drawn again with it,
            // or without it, at once. Without a copy, either the picture had
            // no pointer of ours to take out, or one is wanted in, and the
            // capture hands the desktop over again for it (desktopWanted()),
            // for the acquire below to take.
            if (m_PointerChangedHands.exchange(false) && m_Pipeline->hasHeld()) {
                if (!m_Pipeline->convertHeld(*m_Capture, pointerToDraw(), cursorDraw(), error)) {
                    finish("colour conversion failed: " + error);
                    return;
                }
                if (!emit(frameNumber, resendStamps(steadyNowUs()), error)) return;
                lastSentUs = steadyNowUs();
                noteReal();
            }
            if (desktopWanted() && !retakeDesktop()) return;

            capture::CapturedFrame frame;
            // A duplication that turned out to paint the pointer in is left for
            // Windows.Graphics.Capture, through the ordinary restart: the same
            // road a mode change takes, with DDA skipped for the rest of the
            // session. Before the acquire, so no frame is held when it goes.
            if (m_CaptureApi == CaptureApi::DxgiDuplication && m_Capture->pointerPaintedIn() &&
                !m_DuplicationPaintsPointer &&
                m_Config.tuning.capture != EncoderTuning::Capture::Dda &&
                capture::WgcCapture::available()) {
                m_DuplicationPaintsPointer = true;
                log::info("[native] moving this display to Windows.Graphics.Capture, which leaves "
                          "the pointer out of the picture");
            }
            // The other way, the same road: WGC standing in for a duplication
            // that serves the display again (duplicationBack) hands it back.
            const bool leaveWgc = m_CaptureApi == CaptureApi::WindowsGraphicsCapture &&
                                  m_DdaMayReturn && duplicationBack();
            const bool leaveDda = m_CaptureApi == CaptureApi::DxgiDuplication &&
                                  (m_DuplicationPaintsPointer || ddaLostOnPurpose());
            // cadence=deadline with the client's grid fresh: nothing is taken
            // between two of its refreshes. The loop sleeps until the instant
            // of the next one's picture and takes what the display last
            // presented — Desktop Duplication folds every present in between
            // into it, unconverted. Nothing new, nothing sent. See
            // DeadlineCadence.h.
            aimedNow = false;
            gateNow = &m_Cadence;
            int acquireTimeoutMs = timeoutMs;
            if (deadlineMode && m_Deadline.asItComes(steadyNowUs())) {
                // A canvas that tears, or a link too uneven to aim through: no
                // aim, no wait; the budget's gate.
                const double budget = m_Deadline.budgetFps();
                const int why = m_Deadline.tearing() ? 1 : 2;
                // Rebuilt when the budget really moves: the client's refresh is
                // re-measured every grid and wanders by hundredths of a hertz.
                if (std::abs(budget - tearGateFps) > tearGateFps * 0.005 || why != tearGateWhy) {
                    tearGateWhy = why;
                    tearGateFps = budget;
                    const int displayHz = (m_DisplayMilliHz + 500) / 1000;
                    const auto intervalNs = static_cast<int64_t>(1e9 / budget);
                    tearGate = budget < displayHz
                                   ? FrameCadence::fromIntervalNs(intervalNs, displayHz)
                                   : FrameCadence::ceiling(intervalNs, displayHz);
                    log::info(std::string("[native] deadline: ") +
                              (why == 1 ? "the client tears"
                                        : "the client's frames arrive too unevenly to aim") +
                              " — each picture as it comes, at most " +
                              std::to_string(static_cast<int>(budget + 0.5)) + " a second");
                }
                gateNow = &tearGate;
                m_DeadlineAsItComes.store(true, std::memory_order_relaxed);
                m_DeadlineAimedAtUs.store(steadyNowUs(), std::memory_order_relaxed);
                m_DisplayPeriodUs.store(
                    m_DisplayMilliHz > 0 ? static_cast<int>(1000000000LL / m_DisplayMilliHz) : 0,
                    std::memory_order_relaxed);
            } else if (deadlineMode && !leaveDda && !leaveWgc) {
                m_DeadlineAsItComes.store(false, std::memory_order_relaxed);
                const DeadlineCadence::Aim aim = m_Deadline.next(steadyNowUs());
                if (aim.valid()) {
                    const int64_t sleepStartUs = steadyNowUs();
                    sleepUntilUs(deadlineTimer.h, aim.captureUs);
                    const int64_t wokeUs = steadyNowUs();
                    m_AcquireWaitUs += wokeUs - sleepStartUs;
                    m_Hold.waited(wokeUs - sleepStartUs);
                    const int64_t lateUs = wokeUs - aim.captureUs;
                    m_DeadlineLateUs += lateUs > 0 ? lateUs : 0;
                    if (lateUs > m_DeadlineLateMaxUs) m_DeadlineLateMaxUs = lateUs;
                    if (lateUs > 500) m_DeadlineLateWakes++;
                    m_Deadline.served(aim);
                    m_DeadlineAims++;
                    m_DeadlineAimedAtUs.store(wokeUs, std::memory_order_relaxed);
                    m_DisplayPeriodUs.store(m_DisplayMilliHz > 0
                                                ? static_cast<int>(1000000000LL / m_DisplayMilliHz)
                                                : 0,
                                            std::memory_order_relaxed);
                    aimedNow = true;
                    acquireTimeoutMs = 0;
                }
            }
            const int64_t acquireStartUs = steadyNowUs();
            const capture::AcquireStatus status = leaveDda || leaveWgc
                                                      ? capture::AcquireStatus::Lost
                                                      : m_Capture->acquire(acquireTimeoutMs, frame);
            const int64_t acquiredUs = steadyNowUs();
            m_AcquireWaitUs += acquiredUs - acquireStartUs;
            m_Hold.waited(acquiredUs - acquireStartUs);
            if (m_Config.tuning.clickTrace) traceCapture(status, frame, acquireStartUs, acquiredUs);
            if (aimedNow && status != capture::AcquireStatus::Ok) m_DeadlineNothingNew++;
            if (status == capture::AcquireStatus::PointerOnly)
                m_PointerWakes++;
            else if (status == capture::AcquireStatus::Timeout)
                m_TimeoutWakes++;
            countPresents(status == capture::AcquireStatus::Ok && !frame.retaken);
            if (m_RetakePending) noteRetake(status, frame);

            // Anything but a timeout means the screen is alive again, and the
            // frame about to be encoded is a moving one. Restore the ordinary
            // budget BEFORE it is encoded, never after: that ordering is the
            // whole reason the boost costs no latency.
            if (status != capture::AcquireStatus::Timeout && boosted) {
                boosted = false;
                applyBitrate(baseKbps);
            }

            // Before the status is acted on, and on EVERY status. A client that
            // has just taken over drawing needs to be told what the pointer
            // looks like — including "there is none here" — and on a still
            // screen with the mouse on another display every single wake-up is
            // a timeout, so anything gated behind a frame would never run.
            reportCursor();
            reportCursorPosition();
            recentrePointerIfAway();
            keepPointerShown();

            if (status == capture::AcquireStatus::Timeout && creditHeld && !creditMissing()) {
                // The client's decoder caught up, or the link took what was
                // waiting, and nothing newer came: the picture held goes now.
                creditHeld = false;
                (heldForLink ? m_LinkFlushes : m_CreditFlushes)++;
                if (!emit(frameNumber, creditHeldStamps, error)) return;
                noteReal();
                countPicture();
                continue;
            }

            if (status == capture::AcquireStatus::Timeout) {
                // Nothing moved on the desktop — but the pointer we draw onto it
                // is about to be drawn at a different size, and nothing else in
                // this loop would ever notice. A viewer pinch-zooming a still
                // screen is exactly that: no present, no pointer motion, and a
                // cursor that has to resize anyway.
                if (m_CursorDirty.exchange(false) && m_CompositeCursor.load() &&
                    m_Pipeline->hasHeld()) {
                    if (!m_Pipeline->convertHeld(*m_Capture, pointerToDraw(), cursorDraw(),
                                                 error)) {
                        finish("colour conversion failed: " + error);
                        return;
                    }
                    if (!emit(frameNumber, resendStamps(steadyNowUs()), error)) return;
                    noteReal();
                    continue;
                }

                // Nothing moved. Two reasons to send anyway: the refinement
                // passes above, and — once those are done — the floor, which is
                // either "prove the stream is alive" or whatever faster rate the
                // client asked to keep settling at. Both re-encode what is
                // already converted, so this costs an encode and not a capture.
                if (!m_Pipeline->outputWidth() || awaitingPicture) continue;

                // A keyframe the receiver asked for goes out NOW, not at the
                // next floor tick. On a still screen the floor is the only
                // clock, and it beats every 500 ms — so a browser recovering
                // from a decoder error, or one that just opened its video
                // channel, sat looking at nothing for up to half a second for
                // no reason: the picture is converted and waiting, and this
                // engine, unlike Sunshine, can encode it whenever it likes.
                // Same picture as the floor would have sent, one encode.
                if (m_ForceKeyframe.load(std::memory_order_relaxed)) {
                    if (!emit(frameNumber, resendStamps(steadyNowUs()), error)) return;
                    lastSentUs = steadyNowUs();
                    continue;
                }

                if (steadyNowUs() - lastSentUs < (refining ? refineIntervalUs : idleIntervalUs))
                    continue;

                // A pass waits for the link, never the other way round: the
                // previous pass — or the last frames of the movement that just
                // ended — must have left before another few hundred kilobytes
                // are put behind them. Held, not skipped: the window keeps
                // running and the next wake-up asks again. The liveness floor
                // is not gated (a still frame at the ordinary budget is a few
                // hundred bytes), and a boosted pass is at most 50 ms of link,
                // so the hold can never reach the floor's 500 ms.
                if (refining && !m_Link.drainedAt(steadyNowUs())) {
                    refineHeld++;
                    continue;
                }

                // The budget goes up before the first pass, not after it: the
                // whole point is that this frame is the one that gets to spend.
                if (refining && !boosted) {
                    boosted = true;
                    applyBitrate(encode::stillBitrateKbps(baseKbps));
                }

                if (!emit(frameNumber, resendStamps(steadyNowUs()), error)) return;
                lastSentUs = steadyNowUs();

                if (!refining) continue;
                refinePasses++;
                refineBytes += m_LastEmitBytes;
                switch (refineConv.notePass(m_LastEmitBytes, m_LastEmitQp)) {
                case encode::RefineConvergence::Verdict::Continue: break;
                case encode::RefineConvergence::Verdict::Converged:
                    refineDone = true;
                    closeBurst("converged");
                    break;
                case encode::RefineConvergence::Verdict::Capped:
                    refineDone = true;
                    closeBurst("pass cap");
                    break;
                }
                continue;
            }

            // Only the pointer moved. The desktop is untouched, so there is no
            // new texture.
            //
            // When the CLIENT draws the pointer there is nothing to do at all:
            // report the shape if it changed, and send not one byte of video.
            // That is the whole win of the out-of-band cursor — moving the mouse
            // over a still screen costs nothing, and the pointer moves at the
            // viewer's refresh rate instead of the stream's.
            //
            // When we composite, the picture HAS changed even though the desktop
            // did not, so it is re-converted from our own copy with the cursor
            // at its new place.
            if (status == capture::AcquireStatus::PointerOnly) {
                if (!m_CompositeCursor.load()) continue;
                if (!m_Pipeline->hasHeld()) continue;
                // A pointer Windows paints into the desktop itself moves with
                // the desktop, not apart from it: there is nothing of ours to
                // redraw, and our copy is the picture from before it moved.
                if (m_Capture->cursor().inImage) continue;
                m_CursorDirty.store(false);
                const int64_t submittedUs = steadyNowUs();
                if (!m_Pipeline->convertHeld(*m_Capture, m_Capture->cursor(), cursorDraw(),
                                             error)) {
                    finish("colour conversion failed: " + error);
                    return;
                }
                // No present of its own — the picture changed when the pointer
                // did, so that is its t₀. A picture like any other from here:
                // gated by the cadence if one is running, and when it goes out
                // a new refinement window opens, because a pointer that stops
                // moving leaves a composited frame that deserves sharpening
                // exactly like any other.
                if (!emitPicture(FrameStamps{submittedUs, submittedUs, submittedUs, steadyNowUs()},
                                 false))
                    return;
                continue;
            }

            if (status == capture::AcquireStatus::Lost) {
                // A mode change, a resolution change, a desktop switch, a
                // locked screen: the whole chain behind the duplication goes
                // with it, and the loop lives inside restartCapture until the
                // display is back (see there).
                switch (restartCapture(frameNumber, floorIntervalUs(), error)) {
                case Restart::Restarted: break;
                case Restart::Ended: return;
                case Restart::Stopped:
                    finish("the session was stopped while the display was away");
                    return;
                case Restart::Failed: finish("capture could not be restarted: " + error); return;
                }
                // Whatever the client had is now stale: the encoder is a new one
                // and has no reference frames, and the picture it is about to
                // send is a different desktop.
                m_ForceKeyframe.store(true);
                // The bitrate lives in the encoder that was just replaced, so
                // the ladder's last word has to be said again — and the still
                // boost, if it was in place, went with the old encoder.
                boosted = false;
                applyBitrate(baseKbps);
                // A new picture deserves its own refinement window rather than
                // whatever the old one had reached.
                closeBurst("display lost");
                lastRealUs = steadyNowUs();
                resetBurst();
                continue;
            }

            if (status != capture::AcquireStatus::Ok) {
                finish("capture failed");
                return;
            }

            // A picture handed over again (retakeDesktop) is the desktop as it
            // stood, not a present: nothing for the lattice to learn, nothing
            // for the gate to weigh before it is converted.
            const bool retaken = frame.retaken;
            if (!retaken) {
                m_PresentsSeen++;
                // Before the gate decides on it, skipped or not.
                noteLattice(frame.presentUs);
            }
            const int64_t submittedUs = steadyNowUs();

            // pregate=1 (plan « Gels du lien », 21/10/2026): the gate's word
            // before the conversion. A present it skips was converted all
            // the same, and the next conversion then waited for that one on
            // the CPU (D3d12VideoPipeline::beginList) — the whole GPU queue
            // under another program's load, 5 to 20 ms on the N95's iGPU,
            // five or six times a second with a browser's video plane and a
            // second window presenting together. The gate decides on the
            // time the picture would be ready, taken as now: the conversion
            // adds a millisecond or two, under the gate's quarter-interval
            // windows. The held-for-credit path keeps needing a converted
            // picture, so only the gate moves up, never withheld().
            bool gated = false;
            if (m_PreGate && !aimedNow && !retaken) {
                const FrameStamps early{frame.presentUs, frame.capturedUs, submittedUs,
                                        submittedUs};
                if (!gateSays(early, true)) {
                    m_Pipeline->beforeCaptureRelease();
                    m_Capture->release();
                    m_PreGateSkips++;
                    continue;
                }
                gated = true;
            }

            // See pointerToDraw(): an empty state draws nothing, which is how
            // the client-drawn mode keeps the picture clean.
            const bool composite = m_CompositeCursor.load();
            m_CursorDirty.store(false);
            // Keep a copy of the desktop for the pointer-only path above — only
            // when compositing, and only when the pointer is on this display.
            // When the client draws its own, or the pointer is hidden or on
            // another screen (a fullscreen game, the usual latency-critical
            // case), no pointer-only frame can ever need it: the copy is skipped
            // entirely and the frame path is exactly what it was before — and
            // an older copy goes, being now an older desktop than the client's
            // (desktopWanted() has the capture hand the desktop over again
            // when the pointer needs one).
            //
            // Also on the CPU encoder, whatever the pointer: that is the tier
            // the load cap resizes, and a rebuilt converter needs a picture to
            // start from on a still screen (see the resize above).
            const bool retain = (composite && m_Capture->cursor().visible) ||
                                m_Target.encoder == EncoderApi::Software;
            if (!m_Pipeline->convert(*m_Capture, frame.texture, pointerToDraw(), cursorDraw(),
                                     retain, error)) {
                m_Pipeline->beforeCaptureRelease();
                m_Capture->release();
                finish("colour conversion failed: " + error);
                return;
            }
            awaitingPicture = false;
            // Handed over for a copy the conversion could not keep: the next
            // retake would end the same way.
            if (retaken && desktopWanted() && !m_RetakeOff) {
                m_RetakeOff = true;
                log::info("[native] cursor: the desktop handed over again was not kept for the "
                          "pointer — no more retakes this session (once per session)");
            }

            // Released before encoding: Desktop Duplication refuses the next
            // acquire while a frame is held, and the conversion has already
            // copied what it needs into the NV12 texture.
            m_Pipeline->beforeCaptureRelease();
            const int64_t heldUs = steadyNowUs();
            m_Capture->release();

            // t₂ once the capture is given back, so the stage reads as the
            // whole of what stands between the acquire and the encoder.
            //
            // An aimed picture's t₀ is the instant it was taken: its own present
            // can be a game frame from well before, and the client's lead is
            // counted from the taking — see DeadlineCadence.h. So is a retaken
            // one's, whose present may be seconds old.
            const FrameStamps stamps{aimedNow || retaken ? frame.capturedUs : frame.presentUs,
                                     frame.capturedUs,
                                     submittedUs,
                                     steadyNowUs(),
                                     false,
                                     heldUs};
            // A retaken picture goes as a pointer-only one does: now, through
            // the gate as a picture without a present of its own.
            if (retaken) {
                if (!emitPicture(stamps, false)) return;
            } else if (m_Pipeline->pipelined()) {
                emitPictureLater(stamps, gated);
            } else if (!emitPicture(stamps, true, gated)) {
                return;
            }
        }
    }

    /// Encode whatever the converter currently holds and hand it to the
    /// consumer. @p stamps are the picture's own t₀..t₂ — a capture's, or
    /// "now" for a re-send (idle floor, refinement) that has no present of
    /// its own; see resendStamps.
    ///
    /// Returns false when the session must end; the reason has been reported.
    /// Frames the receiver said it never got, told to the encoder before the
    /// next picture is predicted (design §9.2). An encoder that cannot do it —
    /// or a frame it refuses — falls back to a keyframe, so the receiver always
    /// gets SOME repair. On whichever thread owns the encoder.
    void invalidateLost()
    {
        std::vector<uint32_t> lost;
        {
            std::lock_guard<std::mutex> lock(m_InvalidateMutex);
            lost.swap(m_PendingInvalidations);
        }
        for (uint32_t n : lost) {
            std::string why;
            if (m_Pipeline->invalidateReference(n, why)) {
                m_Invalidated++;
                if (m_Invalidated <= 5 || m_Invalidated % 50 == 0)
                    log::info("[native] reference invalidated: frame " + std::to_string(n) +
                              " never reached the receiver, healing with a delta (" +
                              std::to_string(m_Invalidated) + " so far)");
            } else {
                if (m_InvalidateFallbacks++ < 3)
                    log::info("[native] reference invalidation refused (" + why +
                              ") — keyframe instead");
                m_ForceKeyframe.store(true);
            }
        }
    }

    /// emit(), pipelined (plan Phase 10): the encode and the delivery on the
    /// encode thread, now if it is idle; what follows the delivery on this
    /// thread, at absorbDelivered(). @p frameNumber is the loop's: a job holds
    /// it while it runs, and the loop settles the pipeline before it is gone.
    void emitLater(uint32_t& frameNumber, const FrameStamps& stamps)
    {
        const int encoderKbps = m_EncoderKbps;
        const int linkKbps = m_LinkKbps;
        m_Pipeline->encodeLater([this, &frameNumber, stamps, encoderKbps, linkKbps](
                                    const WindowsVideoPipeline::EncodePicture& encodePicture) {
            Delivered d;
            d.stamps = stamps;
            d.linkKbps = linkKbps;
            invalidateLost();
            const bool forceKeyframe = m_ForceKeyframe.exchange(false);
            encode::EncoderOutput encoded;
            d.result = encodePicture(forceKeyframe, frameNumber, encoded, d.error);
            d.encodedUs = steadyNowUs();
            if (d.result == WindowsVideoPipeline::EncodeResult::Ok) {
                noteFirstKeyframe(encoded);
                d.bytes = encoded.size;
                d.qp = encoded.avgQp;
                if (encoded.data && encoded.size > 0 && m_Callbacks.onVideo) {
                    d.frame = encodedFrame(encoded, frameNumber++, stamps, encoderKbps);
                    m_Callbacks.onVideo(d.frame);
                    d.frame.data = nullptr;
                    d.sent = true;
                }
                m_Pipeline->releaseOutput();
            }
            std::lock_guard<std::mutex> lock(m_DeliveredMutex);
            m_Delivered.push_back(std::move(d));
        });
    }

    /// The stream's interval without a step of the detection — the gate's,
    /// or the rate's at the display's own refresh, where the gate is only a
    /// ceiling a fiftieth shorter.
    int64_t baseIntervalUs() const
    {
        if (m_BaseCadence.enabled() && !m_BaseCadence.isCeiling())
            return m_BaseCadence.intervalUs();
        return m_BaseCadenceFps > 0 ? 1000000 / m_BaseCadenceFps : 0;
    }

    /// How long a new picture took the encoder, for the p95 a step is weighed
    /// against (CadenceStep.h). Deltas only: a keyframe or a re-send says
    /// nothing about the rate the loop holds.
    void noteEncodeTail(const EncodedFrame& out, const FrameStamps& stamps)
    {
        if (out.keyframe || stamps.resend) return;
        if (out.encodedUs <= out.convertedUs) return;
        if (m_EncodeTail.note(out.encodedUs - out.convertedUs, out.encodedUs)) {
            m_EncodeP95Us.store(m_EncodeTail.p95Us(), std::memory_order_relaxed);
            m_EncoderOnePicture.store(m_Target.encoder == EncoderApi::Software,
                                      std::memory_order_relaxed);
        }
    }

    /// What emit() does once a picture went out, for each one the encode
    /// thread delivered since the last call; the cadence counts them in the
    /// loop (m_DeliveredUncounted). A D3D12 chain lost there is taken as emit()
    /// takes it. False, the session finished, when an encode failed for good.
    bool absorbDelivered()
    {
        std::vector<Delivered> delivered;
        {
            std::lock_guard<std::mutex> lock(m_DeliveredMutex);
            delivered.swap(m_Delivered);
        }
        for (const Delivered& d : delivered) {
            if (d.result == WindowsVideoPipeline::EncodeResult::Lost) {
                pipelineLost(d.error);
                continue;
            }
            if (d.result != WindowsVideoPipeline::EncodeResult::Ok) {
                finish("encode failed: " + d.error);
                return false;
            }
            m_LastEmitBytes = d.bytes;
            m_LastEmitQp = d.qp;
            m_Link.sent(d.encodedUs, d.bytes, d.linkKbps);
            if (d.sent) {
                noteEncodeLoad(d.frame, d.stamps);
                noteScalerLoad(d.frame, d.stamps);
                noteEncodeTail(d.frame, d.stamps);
            }
            m_DeliveredUncounted++;
        }
        return true;
    }

    /// The D3D12 chain failed while streaming — a device gone, a fence past
    /// its deadline, an encoder error, the header guard. The session goes
    /// back to D3D11 for good (plan §3.3): the loop rebuilds before its next
    /// capture.
    void pipelineLost(const std::string& error)
    {
        if (!m_D3d12Failed)
            log::warning("[native] video pipeline: D3D12 lost (" + error +
                         ") — back to D3D11 for the rest of the session");
        m_D3d12Failed = true;
        m_D3d12FailedWhy = error;
        m_PipelineLost = true;
    }

    /// The size of the first keyframe, once. It is the number that says
    /// whether a still picture will look right: nothing follows it to refine
    /// it, so on a static screen it IS the picture. Cheap, and it turns "it
    /// looks soft" into a figure that can be compared across settings.
    void noteFirstKeyframe(const encode::EncoderOutput& encoded)
    {
        if (!encoded.keyframe || m_LoggedFirstKeyframe) return;
        m_LoggedFirstKeyframe = true;
        log::info("[native] first keyframe: " + std::to_string(encoded.size / 1024) + " KB (" +
                  std::to_string(m_Info.width) + "x" + std::to_string(m_Info.height) + ")");
    }

    /// What goes to onVideo for @p encoded, numbered @p frameNumber.
    static EncodedFrame encodedFrame(const encode::EncoderOutput& encoded, uint32_t frameNumber,
                                     const FrameStamps& stamps, int encoderKbps)
    {
        EncodedFrame out;
        out.data = encoded.data;
        out.size = encoded.size;
        out.keyframe = encoded.keyframe;
        out.frameNumber = frameNumber;
        out.avgQp = encoded.avgQp;
        out.presentUs = stamps.presentUs;
        out.capturedUs = stamps.capturedUs;
        out.submittedUs = stamps.submittedUs;
        out.convertedUs = stamps.convertedUs;
        out.encodedUs = steadyNowUs();
        out.gpuConvertUs = encoded.gpuConvertUs;
        out.gpuEncodeUs = encoded.gpuEncodeUs;
        out.encoderKbps = encoderKbps;
        return out;
    }

    /// MWSR's zones of the picture just encoded, onto its frame (plan B §12 c,
    /// SessionConfig::mwsrZones): its statistics went to the GPU ahead of its
    /// conversion, which the encoder had to finish, so they are back by now or
    /// nearly, and the read waits for the rest; the decision is the CPU's,
    /// here. Statistics unreadable: the picture before's zones. None yet, or
    /// none on this build: the frame goes
    /// without, and the client keeps its law for it.
    void attachMwsrZones(EncodedFrame& out)
    {
        if (!m_Config.mwsrZones) return;
        ++m_MwsrAskedFrames;
        const int64_t startUs = steadyNowUs();
        float scale = 0.0f;
        int zonesX = 0, zonesY = 0;
        if (!m_Pipeline->takeMwsrZones(m_MwsrZones, zonesX, zonesY, scale)) return;
        const int64_t spentUs = steadyNowUs() - startUs;
        ++m_MwsrZoneFrames;
        m_MwsrDecideUs += spentUs;
        if (spentUs > m_MwsrDecideMaxUs) m_MwsrDecideMaxUs = spentUs;
        out.mwsrZones = m_MwsrZones;
        out.mwsrZonesX = zonesX;
        out.mwsrZonesY = zonesY;
        out.mwsrScale = scale;
    }

    bool emit(uint32_t& frameNumber, const FrameStamps& stamps, std::string& error)
    {
        // Pipelined, the encoder is this thread's once the encode thread holds
        // nothing, and what it delivered comes first.
        if (m_Pipeline->pipelined()) {
            m_Pipeline->settle();
            if (!absorbDelivered()) return false;
        }

        invalidateLost();

        const bool forceKeyframe = m_ForceKeyframe.exchange(false);
        encode::EncoderOutput encoded;
        const int64_t encodeStartUs = steadyNowUs();
        const WindowsVideoPipeline::EncodeResult result =
            m_Pipeline->encode(forceKeyframe, frameNumber, encoded, error);
        if (result == WindowsVideoPipeline::EncodeResult::Lost) {
            // D3D12 only, never D3D11: nothing goes out for this picture.
            pipelineLost(error);
            return true;
        }
        if (result != WindowsVideoPipeline::EncodeResult::Ok) {
            finish("encode failed: " + error);
            return false;
        }

        noteFirstKeyframe(encoded);
        m_LastEmitBytes = encoded.size;
        m_LastEmitQp = encoded.avgQp;
        m_Link.sent(steadyNowUs(), encoded.size, m_LinkKbps);

        if (encoded.data && encoded.size > 0 && m_Callbacks.onVideo) {
            EncodedFrame out = encodedFrame(encoded, frameNumber++, stamps, m_EncoderKbps);
            attachMwsrZones(out);
            // Before the send, which is not the picture's own cost.
            if (m_Config.tuning.clickTrace && !stamps.resend)
                traceFrame(out, stamps, encodeStartUs, encoded);
            // Delivered on this thread, and the consumer sends it before
            // returning. The buffer is unlocked immediately after, which is
            // what keeps the GPU→CPU copy at exactly one per frame.
            m_Callbacks.onVideo(out);
            noteEncodeLoad(out, stamps);
            noteScalerLoad(out, stamps);
            noteEncodeTail(out, stamps);
        }
        m_Pipeline->releaseOutput();
        return true;
    }

    /// How long the encoder took, given to the cap — on the CPU tier only.
    ///
    /// Silent on every hardware encoder: a few milliseconds against a frame
    /// interval is never the problem there, and E4 and the link governor own
    /// that space. It is the machine that fell all the way to OpenH264 where
    /// the encode duration IS the latency, and where trading pixels for it is
    /// the right bargain (EncodeLoadCap.h says why that way round).
    ///
    /// Only new pictures encoded as deltas count — see LinuxSession::noteEncodeLoad.
    void noteEncodeLoad(const EncodedFrame& out, const FrameStamps& stamps)
    {
        if (m_Target.encoder != EncoderApi::Software) return;
        if (out.keyframe || stamps.resend) return;
        const int64_t convertedUs = out.convertedUs;
        const int64_t encodedUs = out.encodedUs;
        if (encodedUs <= convertedUs) return;
        // The STREAM's interval, not the gate's: at the display's own rate the
        // gate is only a ceiling a fiftieth shorter than the interval — see
        // LinuxSession::noteEncodeLoad for why the stream's rate is the budget.
        // Without a step of the detection: a trial is not a new budget.
        const int64_t intervalUs = baseIntervalUs();
        if (m_LoadCap.note(encodedUs - convertedUs, intervalUs, encodedUs))
            m_PendingResize.store(true);
    }

    /// The resample pass priced against the frame interval — on the hardware
    /// encoders, the ones that get Lanczos-2.
    ///
    /// Measured on bench-intel (N95, 16-EU UHD Graphics, 21/09/2026), 1440p
    /// desktop to a 1080p60 stream, oneVPL: 24 fps with Lanczos-2 against 46
    /// with bilinear, alternated three times over. The two 1-D passes cost the
    /// iGPU ~12 ms a frame on top of a 3D engine the desktop already keeps 60 %
    /// busy, and the encoder waits for them: submit-to-encoded was 29 ms, so the
    /// loop, which encodes one picture at a time, captured one present in two.
    /// A sharper picture at half the frames is the wrong trade for a stream —
    /// so once a window of frames shows the conversion and the encode together
    /// eating most of the interval, the resample goes, for this pipeline.
    /// The discrete GPUs it was chosen on spend a millisecond or two there and
    /// never come near the threshold.
    ///
    /// That rule is about throughput, and latency goes first. The pass is also
    /// timed on the GPU during the stream's first frames (ResampleCost): on the
    /// two-CU iGPU of DualRTX it was 4.5 ms of every frame's latency, under
    /// that threshold on a still desktop and so kept for as long as nothing
    /// moved. Above ResampleCost::kBudgetUs the resample goes at once.
    void noteScalerLoad(const EncodedFrame& out, const FrameStamps& stamps)
    {
        // A window of deltas, a second at 60 fps: long enough that a keyframe
        // or a scheduler hiccup does not decide it, short enough that the
        // viewer has barely seen the half-rate stream.
        constexpr int kWindowFrames = 60;
        // Most of the interval rather than all of it: past this the loop has
        // no room left for the acquire, and presents start being skipped.
        constexpr int64_t kBudgetPercent = 75;

        if (m_Target.encoder == EncoderApi::Software) return;
        if (m_Pipeline->scaleFilter() == convert::ColorConvert::ScaleFilter::Bilinear) return;

        int64_t costUs = 0;
        if (m_Pipeline->takeResampleCost(costUs)) {
            char ms[16], budget[16];
            std::snprintf(ms, sizeof(ms), "%.1f", costUs / 1000.0);
            std::snprintf(budget, sizeof(budget), "%.1f",
                          convert::ResampleCost::kBudgetUs / 1000.0);
            const bool affordable = convert::ResampleCost::affordable(costUs);
            if (!affordable && !m_ScalerPinned && m_Pipeline->dropResample()) {
                log::info(std::string("[native] resample dropped: Lanczos-2 costs ") + ms +
                          " ms of GPU a frame here, over the " + budget +
                          " ms it may add to every frame — the stream goes on scaled bilinear "
                          "(MW_SCALER=lanczos2 keeps it)");
                return;
            }
            log::info(std::string("[native] resample: Lanczos-2 costs ") + ms +
                      " ms of GPU a frame here — kept" +
                      (affordable       ? ""
                       : m_ScalerPinned ? " (MW_SCALER=lanczos2)"
                                        : " (letterboxed: bilinear would stretch the picture)"));
        }

        if (m_ScalerPinned) return;
        if (out.keyframe || stamps.resend) return;
        if (out.encodedUs <= out.submittedUs) return;
        // A step of the detection is a trial, not the stream's rate: Lanczos-2
        // is not given up for good over a few seconds at 240.
        const int64_t intervalUs = baseIntervalUs();
        if (intervalUs <= 0) return;

        m_ScalerWindowUs += out.encodedUs - out.submittedUs;
        if (++m_ScalerWindowFrames < kWindowFrames) return;
        const int64_t meanUs = m_ScalerWindowUs / m_ScalerWindowFrames;
        m_ScalerWindowUs = 0;
        m_ScalerWindowFrames = 0;
        if (meanUs * 100 <= intervalUs * kBudgetPercent) return;

        if (m_Pipeline->dropResample())
            log::info("[native] resample dropped: conversion + encode took " +
                      std::to_string(meanUs / 1000) + " ms a frame against a " +
                      std::to_string(intervalUs / 1000) +
                      " ms interval — this GPU cannot afford Lanczos-2 at this rate, the "
                      "stream goes on scaled bilinear (MW_SCALER=lanczos2 keeps it)");
    }

    /// Rebuild at the size the cap now asks for. Between frames, never inside
    /// emit(): the encoder there is still holding a bitstream.
    ///
    /// True when the converter was rebuilt — at the new size, or back at the old
    /// one — and so holds no picture yet. The desktop copy survives it: the
    /// capture, and the device the copy lives on, are not rebuilt here.
    bool applyLoadCap()
    {
        const int width = encode::EncodeLoadCap::scaled(m_FullWidth, m_LoadCap.percent());
        const int height = encode::EncodeLoadCap::scaled(m_FullHeight, m_LoadCap.percent());
        if (width == m_Info.width && height == m_Info.height) return false;

        std::string error;
        const int wasWidth = m_Info.width;
        const int wasHeight = m_Info.height;
        // The held desktop survives the rebuild: same capture, same picture.
        if (!buildPipeline(width, height, error, /*keepHeld=*/true)) {
            // Keep streaming at the size that worked rather than ending the
            // session over an optimisation.
            log::warning("[native] cpu cap: cannot encode at " + std::to_string(width) + "x" +
                         std::to_string(height) + " (" + error + ") — staying at " +
                         std::to_string(wasWidth) + "x" + std::to_string(wasHeight));
            if (!buildPipeline(wasWidth, wasHeight, error, /*keepHeld=*/true)) {
                finish("encoder restart failed: " + error);
                return false;
            }
            return true;
        }
        m_Info.width = m_Pipeline->outputWidth();
        m_Info.height = m_Pipeline->outputHeight();
        m_ForceKeyframe.store(true);
        log::info("[native] cpu cap: " + std::to_string(wasWidth) + "x" +
                  std::to_string(wasHeight) + " -> " + std::to_string(m_Info.width) + "x" +
                  std::to_string(m_Info.height) + " (" + std::to_string(m_LoadCap.percent()) +
                  "% of the display) — the CPU encoder sets the latency, so pixels give way "
                  "before frames");
        return true;
    }

    /// One wake-up of the capture into the click trace (clicktrace=1): what it
    /// brought, the OS's stamps on it, and DWM's timing read right after —
    /// its last vblank, its period, its last composition. Capture thread only.
    void traceCapture(capture::AcquireStatus status, const capture::CapturedFrame& frame,
                      int64_t startUs, int64_t doneUs)
    {
        ClickTrace::Row row;
        row.kind = ClickTrace::Kind::Capture;
        row.us = doneUs;
        row.startUs = startUs;
        row.status = frame.retaken ? "retaken" : acquireStatusName(status);
        if (status == capture::AcquireStatus::Ok) {
            row.presentUs = frame.presentUs;
            row.presentRawUs = frame.presentRawUs;
            row.mouseUs = frame.mouseUs;
            row.accumulated = frame.accumulated;
        }
        // hwnd null: the desktop's own composition, the only form Windows 8.1
        // and later accept.
        DWM_TIMING_INFO timing = {};
        timing.cbSize = sizeof(timing);
        const HRESULT hr = ::DwmGetCompositionTimingInfo(nullptr, &timing);
        if (SUCCEEDED(hr)) {
            row.vblankUs = qpcToSteadyUs(static_cast<int64_t>(timing.qpcVBlank));
            row.periodUs = qpcToSteadyUs(static_cast<int64_t>(timing.qpcRefreshPeriod));
            row.composeUs = qpcToSteadyUs(static_cast<int64_t>(timing.qpcCompose));
            row.composedFrames = static_cast<int64_t>(timing.cFrame);
        } else if (!m_DwmTimingRefused) {
            m_DwmTimingRefused = true;
            char code[16];
            std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
            log::warning(std::string("[native] click trace: DWM gives no composition timing (") +
                         code + ") — the trace goes without it");
        }
        m_ClickTrace.add(row);
    }

    /// A frame row of the click trace (core/ClickTrace.h): the picture's way
    /// from the converter to the bitstream, with the D3D12 chain's own steps
    /// where it keeps them. One picture at a time only: the pipelined chain
    /// encodes on a thread of its own, and traces nothing.
    void traceFrame(const EncodedFrame& out, const FrameStamps& stamps, int64_t encodeStartUs,
                    const encode::EncoderOutput& encoded)
    {
        ClickTrace::Row row;
        row.kind = ClickTrace::Kind::Frame;
        row.us = out.encodedUs;
        row.startUs = stamps.submittedUs;
        row.presentUs = stamps.presentUs;
        row.frame = out.frameNumber;
        WindowsVideoPipeline::ConvertTrace trace;
        if (m_Pipeline->lastConvertTrace(trace)) {
            row.signalledUs = trace.signalledUs;
            row.listUs = trace.listUs;
            row.recordedUs = trace.recordedUs;
            row.executedUs = trace.executedUs;
        }
        row.heldUs = stamps.heldUs;
        row.releasedUs = stamps.convertedUs;
        row.encodeUs = encodeStartUs;
        row.gpuStartUs = encoded.gpuConvertStartUs;
        row.gpuEndUs = encoded.gpuConvertEndUs;
        row.gpuEncodeUs = encoded.gpuEncodeUs;
        m_ClickTrace.add(row);
    }

    /// Tell the client what the pointer looks like, when the client is the one
    /// drawing it.
    ///
    /// Sent on change only — shape, or appearing/disappearing. A pointer being
    /// moved around keeps one shape for thousands of frames, so in the case
    /// this feature exists for, this sends nothing at all.
    ///
    /// Position is deliberately NOT sent here. A client with a pointer device
    /// knows where its own pointer is, better and sooner than we could tell it;
    /// sending ours would only give it something to disagree with. The one that
    /// has no pointer device gets it apart — see reportCursorPosition.
    void reportCursor()
    {
        if (!m_Callbacks.onCursor || m_CompositeCursor.load()) return;

        const capture::CursorState& cursor = m_Capture->cursor();
        const bool forced = m_ResendCursor.exchange(false);
        // A pointer the picture already carries is not the client's to draw:
        // it would end up beside the real one, in the shape the drag started
        // with. "Not visible" is exactly right — there is nothing for the
        // client to put on screen. See CursorState::inImage.
        const bool visible = cursor.visible && !cursor.inImage;
        // The KIND is checked too, not just the shape version. An application
        // can swap between two standard cursors without DXGI ever handing over
        // a new bitmap — it caches shapes it has already sent — so a client
        // following the name alone would never see the change.
        const char* kind = currentCursorKind();
        const bool kindChanged = std::strcmp(kind, m_ReportedKind.c_str()) != 0;
        // And the scale, which changes without the shape doing anything at all:
        // the host switching display mode resizes the desktop under a pointer
        // that keeps its bitmap. See CursorUpdate::scale.
        const float scale = cursorScale();
        const bool scaleChanged = scale != m_ReportedScale;
        if (!forced && !kindChanged && !scaleChanged && cursor.shapeVersion == m_ReportedShape &&
            visible == m_ReportedVisible)
            return;

        m_ReportedShape = cursor.shapeVersion;
        m_ReportedVisible = visible;
        m_ReportedKind = kind;
        m_ReportedScale = scale;

        CursorUpdate update;
        update.visible = visible;
        update.kind = kind;
        update.width = cursor.width;
        update.height = cursor.height;
        update.scale = scale;
        // The capture stores the image's top-left, having already subtracted
        // the hotspot; the client needs the offset itself to place the image
        // against its own pointer.
        update.hotspotX = m_Capture->cursorHotspotX();
        update.hotspotY = m_Capture->cursorHotspotY();

        // Inverting pixels are flattened here rather than in the capture,
        // because the composited path genuinely inverts and must keep the
        // information. See CursorUpdate::pixels for why white-on-black.
        if (!cursor.pixels.empty() && cursor.width > 0 && cursor.height > 0) {
            m_CursorScratch = cursor.pixels;
            flattenInvert(cursor);
            update.pixels = m_CursorScratch.data();
        }

        m_Callbacks.onCursor(update);
    }

    /// Where the pointer is, for a client that draws it without a pointer device
    /// of its own to know. Every loop iteration, throttled by the gate: a sweep
    /// of the mouse is a PointerOnly per event on a still screen, and the
    /// client needs a correction, not a replay. See CursorUpdate::positionOnly.
    void reportCursorPosition()
    {
        if (!m_Callbacks.onCursor) return;
        if (m_CompositeCursor.load()) {
            // Nothing to say while we draw it; and the client that takes it
            // over next has seen nothing, so the first position must go out.
            m_PositionGate.reset();
            return;
        }
        const capture::CursorState& cursor = m_Capture->cursor();
        const float scale = cursorScale();
        // Same rule as reportCursor: a pointer the picture already carries is
        // not one the client should be drawing anywhere.
        const bool visible = cursor.visible && !cursor.inImage;
        // The capture keeps the image's corner; the client wants the hotspot.
        const float fx = static_cast<float>(cursor.x + m_Capture->cursorHotspotX()) * scale;
        const float fy = static_cast<float>(cursor.y + m_Capture->cursorHotspotY()) * scale;
        if (!m_PositionGate.due(visible, static_cast<int>(fx), static_cast<int>(fy), steadyNowUs()))
            return;
        CursorUpdate update;
        update.positionOnly = true;
        update.visible = visible;
        update.x = fx;
        update.y = fy;
        m_Callbacks.onCursor(update);
    }

    /// Bring the pointer onto the streamed display if it is elsewhere when the
    /// stream starts — but only while WE draw it into the picture.
    ///
    /// At the start only: once the pointer has been seen on this display, the
    /// person in front of the host may take it to their other screen, and it is
    /// theirs to take. Pulling it back all session long (18/09 to 05/10) put it
    /// in the middle of the streamed display every time they tried (Bruno,
    /// 05/10: "seulement au démarrage partout"). Seen here in desktop mode
    /// counts too: the client put it there, the start is over.
    ///
    /// The drawing condition. When the client draws its own pointer it
    /// has one on screen wherever the host's is, and its next move places the
    /// host's under it; nothing is lost and nothing should be moved behind the
    /// viewer's back. When the pointer is drawn into the frame instead —
    /// gaming mode, or a phone's trackpad — the streamed display is all the
    /// viewer can see, and a pointer on the host's OTHER monitor is a pointer
    /// they are steering blind: relative motion still moves it, clicks still
    /// land, and none of it shows. The middle of the streamed display is the
    /// one place they can be sure to find it again.
    ///
    /// The guests' shared feed draws the pointer for every guest, whatever
    /// mode each is in, and so brings it over at its start whatever they
    /// are in. Until 11/10 it did only for a guest with pointer lock, which a
    /// guest in desktop mode found missing (S9.2).
    ///
    /// Windows' own word rather than the capture's: Desktop Duplication says
    /// "not visible" for a pointer on another display and for one an
    /// application hid, and only the first is ours to fix. Every kMs, which is
    /// slow enough to cost nothing and quick enough that the pointer is back
    /// before a second move.
    void recentrePointerIfAway()
    {
        static constexpr int64_t kIntervalUs = 200000;
        if (m_RecentreDone || !m_Capture) return;
        const int64_t nowUs = steadyNowUs();
        if (nowUs - m_LastRecentreCheckUs < kIntervalUs) return;
        m_LastRecentreCheckUs = nowUs;

        CURSORINFO info = {};
        info.cbSize = sizeof(info);
        if (!::GetCursorInfo(&info) || (info.flags & CURSOR_SHOWING) == 0) return;
        const capture::DesktopRect& rect = m_Capture->desktopRect();
        if (!rect.valid()) return;
        if (info.ptScreenPos.x >= rect.left && info.ptScreenPos.x < rect.right &&
            info.ptScreenPos.y >= rect.top && info.ptScreenPos.y < rect.bottom) {
            // Where it belongs, whatever the mode: the start is over.
            m_RecentreDone = true;
            return;
        }
        if (!m_CompositeCursor.load()) return;

        // An application can be holding the pointer on the other display and
        // pulling it back every frame — Counter-Strike left running on the
        // primary screen does exactly that. We would lose that tug of war,
        // at five warps a second, and the pointer would flicker between the
        // two screens. A few attempts say what can be said; after that the
        // pointer is not free to move, and the start is over all the same.
        if (++m_RecentreTries > kMaxRecentreTries) {
            m_RecentreDone = true;
            return;
        }

        // Through the input sink like any other position, so the display
        // rectangle, the DPI virtualization and the recentring detector are all
        // applied exactly once, where they already live. A reference square
        // rather than the display's own size: the middle of anything is the
        // middle, and the fields are 16-bit.
        InputEvent event;
        event.type = InputEvent::Type::MouseMoveAbsolute;
        event.positionX = 1000;
        event.positionY = 1000;
        event.referenceWidth = 2000;
        event.referenceHeight = 2000;
        {
            std::lock_guard<std::mutex> lock(m_InputMutex);
            if (!m_Input) return;
            m_Input->inject(event);
        }
        if (!m_LoggedRecentre) {
            m_LoggedRecentre = true;
            log::info("[native] cursor: the pointer was on another display at the start while we "
                      "draw it into the picture — put in the middle (once per session)");
        }
    }

    /// Mouse Keys on while this host has no pointing device, so that Windows
    /// draws the pointer at all — see PointerPresence. Every loop wake-up; it
    /// looks once a second, and on a host with a mouse it never does more than
    /// look. Not while the input's own mouse shows the pointer (SystemMouse.h):
    /// Mouse Keys are the fallback, for a host where it could not be made.
    void keepPointerShown()
    {
        bool ownMouse = false;
        {
            std::lock_guard<std::mutex> lock(m_InputMutex);
            ownMouse = m_Input && m_Input->showsPointer();
        }
        if (ownMouse) {
            // Turned on while Windows had not listed it yet: they go back.
            if (m_PointerPresence.engaged()) releasePointerKeys();
            return;
        }
        PointerPresence::Step step = m_PointerPresence.poll(steadyNowUs());
        if (step == PointerPresence::Step::Lost) {
            log::info(
                "[native] pointer: Mouse Keys turned off elsewhere during the stream (another "
                "stream ended, or the user)");
            if (m_OnMouseKeys) m_OnMouseKeys(false, MouseKeysState{});
            step = m_PointerPresence.poll(steadyNowUs());
        }
        if (step == PointerPresence::Step::TurnedOn) {
            log::info("[native] pointer: no pointing device on this host — Mouse Keys turned on "
                      "for the stream, so Windows draws the pointer (put back at the end)");
            if (m_OnMouseKeys) m_OnMouseKeys(true, m_PointerPresence.saved());
        } else if (step == PointerPresence::Step::Failed) {
            log::warning("[native] pointer: no pointing device on this host, and Mouse Keys could "
                         "not be turned on (error " +
                         std::to_string(::GetLastError()) +
                         ") — Windows shows no pointer (once per session)");
        }
    }

    /// Mouse Keys put back as they were, if this session turned them on and
    /// they are still as it left them.
    void releasePointerKeys()
    {
        switch (m_PointerPresence.release()) {
        case PointerPresence::Step::Restored:
            log::info("[native] pointer: Mouse Keys put back as they were");
            break;
        case PointerPresence::Step::LeftAlone:
            log::info("[native] pointer: Mouse Keys changed during the stream — left as they are");
            break;
        case PointerPresence::Step::Failed:
            // Not reported as let go: the parent tries again once this
            // process is gone, and either way they go at sign-out.
            log::warning("[native] pointer: Mouse Keys could not be put back (error " +
                         std::to_string(::GetLastError()) + ")");
            return;
        default: return;
        }
        if (m_OnMouseKeys) m_OnMouseKeys(false, MouseKeysState{});
    }

    /// The pointer is to be drawn into the picture, it is on this display, and
    /// no copy of the desktop is held to draw it on: right after the switch
    /// from desktop mode, which keeps none, or once the pointer is back from
    /// another display, the copy having gone with the first picture converted
    /// without it. On a still screen nothing else brings a desktop before the
    /// screen changes, and the pointer stayed out of the picture until then —
    /// thirty-six moves of it without a picture after a switch to gaming mode
    /// (click trace of 21/10/2026, plan « Gels du lien »). Keeping the copy in
    /// desktop mode too would cost every picture 0.9 ms on the N95 (bench
    /// §8y.5); Bruno chose the retake, 23/10.
    bool desktopWanted() const
    {
        if (!m_CompositeCursor.load() || m_Pipeline->hasHeld()) return false;
        const capture::CursorState& cursor = m_Capture->cursor();
        return cursor.visible && !cursor.inImage;
    }

    /// Have the capture hand the desktop over again (IWindowsCapture::retake),
    /// for desktopWanted(): the next acquire takes it, and its conversion keeps
    /// the copy like any other picture with the pointer on it. Once a second
    /// at most; given up for the session, said once, when the capture cannot,
    /// hands nothing over, or the copy is not kept. False only when the session
    /// ends.
    bool retakeDesktop()
    {
        const int64_t askedUs = steadyNowUs();
        if (m_RetakeOff || m_RetakePending || askedUs < m_NextRetakeUs) return true;
        m_NextRetakeUs = askedUs + kRetakeIntervalUs;
        // Nothing in flight and nothing kept of the surfaces about to go, as
        // before a restart.
        if (m_Pipeline->pipelined()) {
            m_Pipeline->settle();
            if (!absorbDelivered()) return false;
        }
        m_Pipeline->forgetCaptured();
        // DXGI duplicates the desktop of the calling thread: see openCapture().
        platform::attachThread();
        std::string why;
        const int64_t openUs = steadyNowUs();
        if (!m_Capture->retake(why)) {
            // The capture says Lost at the next acquire if it let go of what it
            // had: the ordinary restart follows.
            m_RetakeOff = true;
            log::info("[native] cursor: the capture could not hand the desktop over again (" + why +
                      ") — the pointer waits for the screen to change (once per session)");
            return true;
        }
        m_RetakeOpenUs = steadyNowUs() - openUs;
        m_RetakeAskedUs = askedUs;
        m_RetakePending = true;
        return true;
    }

    /// What came of retakeDesktop(), at each acquire until it is settled: its
    /// picture, timed for the log, or kRetakeWaitUs without one, and the
    /// session gives up on retakes.
    void noteRetake(capture::AcquireStatus status, const capture::CapturedFrame& frame)
    {
        if (status == capture::AcquireStatus::Ok && frame.retaken) {
            m_RetakePending = false;
            const int64_t tookUs = frame.capturedUs - m_RetakeAskedUs;
            m_Retakes++;
            m_RetakeTotalUs += tookUs;
            if (tookUs > m_RetakeMaxUs) m_RetakeMaxUs = tookUs;
            if (m_Retakes <= 3) {
                char took[64];
                std::snprintf(took, sizeof(took), "%.1f ms (%.1f to open the capture again)",
                              tookUs / 1000.0, m_RetakeOpenUs / 1000.0);
                log::info(std::string("[native] cursor: no desktop held to draw the pointer on — "
                                      "handed over again in ") +
                          took);
            }
            return;
        }
        if (steadyNowUs() - m_RetakeAskedUs < kRetakeWaitUs) return;
        m_RetakePending = false;
        m_RetakeOff = true;
        log::info("[native] cursor: the capture opened again handed no desktop over within " +
                  std::to_string(kRetakeWaitUs / 1000) +
                  " ms — the pointer waits for the screen to change (once per session)");
    }

    /// How much the converter shrinks (or stretches) the desktop on its way into
    /// the frame — which is exactly what the pointer bitmap must be multiplied
    /// by. See CursorUpdate::scale.
    ///
    /// Width only, matching the client's own convention: the two rectangles
    /// share an aspect ratio in every case a client can ask for, and taking one
    /// axis avoids a pointer that is a different shape from the one the host is
    /// showing.
    float cursorScale() const
    {
        const int captured = m_Capture ? m_Capture->width() : 0;
        const int framed = m_Pipeline ? m_Pipeline->outputWidth() : 0;
        if (captured <= 0 || framed <= 0) return 1.0f;
        return static_cast<float>(framed) / static_cast<float>(captured);
    }

    /// How much bigger than life to draw the composited pointer, so it comes out
    /// the width the client asked for.
    ///
    /// The client asks in frame pixels because that is the only unit both sides
    /// can compute — it knows how many of them fit across the viewer's screen,
    /// we know how many of them the pointer currently covers. Nobody has to
    /// agree on a cursor size, a DPI or a phone.
    ///
    /// Never below 1: the request exists to make a pointer visible on a small
    /// screen, and a viewer zoomed in far enough that the natural size already
    /// exceeds the target wants the natural size, not a shrunken one. Capped
    /// because this is a 32-pixel bitmap being stretched — past 4× it stops
    /// looking like a pointer and starts looking like a bug.
    float cursorMagnification() const
    {
        const int wanted = m_CursorFramePx.load();
        if (wanted <= 0 || !m_Capture) return 1.0f;
        // The drawn extent, not the canvas: Windows pads the same arrow into
        // 32, 48 or 64-pixel buffers depending on where it came from, and sizing
        // on the buffer made the pointer change size every time the shape
        // changed hands. The LONGER side of the ink, so a tall I-beam and a
        // wide resize arrow both come out the size the client asked for rather
        // than the narrow one being blown up. See CursorState::inkWidth.
        const capture::CursorState& shape = m_Capture->cursor();
        const int ink = shape.inkWidth > shape.inkHeight ? shape.inkWidth : shape.inkHeight;
        const int shapeWidth = ink > 0 ? ink : shape.width;
        if (shapeWidth <= 0) return 1.0f;
        const float natural = static_cast<float>(shapeWidth) * cursorScale();
        if (!(natural > 0.0f)) return 1.0f;
        const float magnify = static_cast<float>(wanted) / natural;
        if (!(magnify > 1.0f)) return 1.0f;
        return magnify > kMaxCursorMagnify ? kMaxCursorMagnify : magnify;
    }

    /// The pointer the converter should draw over the desktop.
    ///
    /// None when the client draws its own, which is what keeps the picture
    /// clean in desktop mode. And none either when the capture says the
    /// picture already carries the pointer — Windows paints it in for the whole
    /// of a title-bar drag, and the shape we hold is the one from before the
    /// drag, so drawing it would put a second, stale pointer beside the real
    /// one. See CursorState::inImage.
    const capture::CursorState& pointerToDraw() const
    {
        static const capture::CursorState kNoPointer;
        if (!m_CompositeCursor.load() || !m_Capture) return kNoPointer;
        const capture::CursorState& cursor = m_Capture->cursor();
        return cursor.inImage ? kNoPointer : cursor;
    }

    /// Everything the converter needs about the pointer that the capture does
    /// not already tell it.
    convert::CursorDraw cursorDraw() const
    {
        convert::CursorDraw draw;
        draw.magnify = cursorMagnification();
        if (m_Capture) {
            draw.hotspotX = m_Capture->cursorHotspotX();
            draw.hotspotY = m_Capture->cursorHotspotY();
        }
        return draw;
    }

    /// Turn the inverting pixels in m_CursorScratch into something a client can
    /// draw: white fill, black outline.
    ///
    /// An inverting pixel says "show the opposite of whatever is behind me",
    /// which is how one bare stroke stays legible on a white page and on a dark
    /// text field. No image format can say that, so it has to be resolved to
    /// fixed colours — and a single colour cannot work on both backgrounds,
    /// which is what made a black I-beam disappear into a dark input.
    ///
    /// The outline is traced only into pixels the cursor left fully transparent,
    /// so an ordinary coloured cursor that happens to carry a few inverting
    /// pixels keeps its own artwork intact. Nothing is written outside the
    /// bitmap: an outline pixel that would fall off the edge is simply not
    /// drawn, which costs a sliver of a shape that already reaches the border.
    void flattenInvert(const capture::CursorState& cursor)
    {
        const int w = cursor.width;
        const int h = cursor.height;
        if (cursor.invert.size() != static_cast<size_t>(w) * static_cast<size_t>(h)) return;

        auto paint = [this](size_t i, uint8_t v) {
            m_CursorScratch[i * 4 + 0] = v;
            m_CursorScratch[i * 4 + 1] = v;
            m_CursorScratch[i * 4 + 2] = v;
            m_CursorScratch[i * 4 + 3] = 0xFF;
        };

        // The outline reads the ORIGINAL alpha, so it must be traced before the
        // fill overwrites it — hence two passes over the same buffer rather than
        // one that would outline the pixels it just painted.
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (cursor.invert[i]) continue;
                if (m_CursorScratch[i * 4 + 3] != 0) continue; // the cursor's own pixel

                bool touches = false;
                for (int dy = -1; dy <= 1 && !touches; ++dy) {
                    for (int dx = -1; dx <= 1 && !touches; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        touches = cursor.invert[static_cast<size_t>(ny) * w + nx] != 0;
                    }
                }
                if (touches) paint(i, 0x00);
            }
        }

        for (size_t i = 0; i < cursor.invert.size(); ++i)
            if (cursor.invert[i]) paint(i, 0xFF);
    }

    /// Choose the loop's gate for the viewer's setting against the client's
    /// screen — at start, and again whenever the client's screen changes.
    /// The rules are CadenceChoice.h's; this gathers what they read. Returns
    /// the cadence's rate, fills @p cadence, and writes the log line that
    /// says why.
    ///
    /// The same choice without the step in force is kept too (m_BaseCadence):
    /// it is what a step is measured against, and what the load checks budget
    /// against — a trial at 240 must not cost the session its resample.
    int chooseCadence(int clientMilliHz, bool clientVsync, FrameCadence& cadence, std::string& line)
    {
        CadenceInputs in;
        in.settingFps = m_Config.fps;
        in.maxFps = m_Config.maxFps;
        in.clientCapFps = m_ClientFpsCap.load();
        in.displayMilliHz = m_DisplayMilliHz;
        in.clientMilliHz = clientMilliHz;
        in.clientVsync = clientVsync;
        in.mode = m_Config.tuning.cadence;
        const CadenceChoice base = mw::native::chooseCadence(in);
        m_BaseCadence = base.gate;
        m_BaseCadenceFps = base.fps;
        m_BaseFps.store(base.fps);
        in.stepFps = m_StepFps.load();
        m_ChosenStepFps = in.stepFps;
        CadenceChoice chosen = in.stepFps > 0 ? mw::native::chooseCadence(in) : base;
        cadence = chosen.gate;
        line = std::move(chosen.line);
        return chosen.fps;
    }

    /// How fast the content changes, for the client's detection
    /// (CadenceStep.h): the present an acquire returned (@p present) and those
    /// Desktop Duplication folded into it, whatever the gate then does with
    /// them. Every other turn of the loop only moves the clock — a retaken
    /// picture's included. Capture thread.
    void countPresents(bool present)
    {
        const int64_t nowUs = steadyNowUs();
        if (present) {
            int64_t presents = 1;
            const int64_t folded = m_Capture ? m_Capture->foldedPresents() : -1;
            if (folded >= 0) {
                // A capture opened again counts from zero.
                if (folded >= m_FoldedSeen) presents += folded - m_FoldedSeen;
                m_FoldedSeen = folded;
            }
            m_PresentRate.note(presents, nowUs);
            m_Hold.presents(presents, presents - 1);
        } else {
            m_PresentRate.tick(nowUs);
        }
        m_Hold.tick(nowUs);
        m_PresentsPerSecond.store(m_PresentRate.perSecond(), std::memory_order_relaxed);
    }

    /// A new lattice of the display's presents for the capture just opened,
    /// and whether the stream's gate may lock on it (PresentLattice.h,
    /// FrameCadence.h): at start, then at each restart of the capture. Its
    /// period is the mode the duplication read, else the session's refresh,
    /// which "Match my screen" leaves in whole hertz (239 for 239.757).
    void armCadenceLock()
    {
        int64_t periodNs = m_Capture ? m_Capture->refreshPeriodNs() : 0;
        if (periodNs < kLatticeMinPeriodNs || periodNs > kLatticeMaxPeriodNs)
            periodNs = PresentLattice::periodNsFromMilliHz(m_DisplayMilliHz);
        m_Lattice.restart(periodNs);
        m_LockArmed = cadenceLockArmed(m_Config.tuning.cadenceLock, productCadence(),
                                       m_CaptureApi == CaptureApi::DxgiDuplication, periodNs);
    }

    bool productCadence() const
    {
        return m_Config.tuning.cadence == EncoderTuning::Cadence::Default;
    }

    /// Whether the gate may lock on the display's presents, on how many
    /// periods a picture, or why not: at start, at a new gate, at a new
    /// capture (@p why).
    void logCadenceLock(const char* why)
    {
        log::info("[native] cadence lock: " +
                  cadenceLockText(m_Config.tuning.cadenceLock, productCadence(),
                                  m_CaptureApi == CaptureApi::DxgiDuplication, m_Lattice.periodNs(),
                                  m_Cadence) +
                  why);
    }

    /// A real present's time, for the lattice, before the gate decides on
    /// it. What it changed is said, the first five times and then one in
    /// twenty. Capture thread.
    void noteLattice(int64_t presentUs)
    {
        const PresentLattice::Change change = m_Lattice.note(presentUs * 1000);
        if (change == PresentLattice::Change::None) return;
        const int64_t said = m_LatticeLines++;
        if (said >= 5 && said % 20 != 0) return;
        if (change == PresentLattice::Change::Resynced) {
            log::info("[native] cadence lock: the lattice's phase starts again on a present, "
                      "after a pause");
            return;
        }
        char offset[64];
        std::snprintf(offset, sizeof(offset), "mean offset %.3f of a period, %lld µs",
                      m_Lattice.sharpness(),
                      static_cast<long long>(m_Lattice.meanOffsetNs() / 1000));
        const bool locks = m_LockArmed && m_Cadence.lockRatio(m_Lattice.periodNs()) > 0;
        if (change == PresentLattice::Change::Engaged)
            log::info(std::string("[native] cadence lock: engaged, the display's presents keep to "
                                  "its period (") +
                      offset + ")" +
                      (locks ? "; the gate locks on them" : "; the gate does not lock on them"));
        else
            log::info(std::string("[native] cadence lock: let go, the display's presents stray "
                                  "from its period (") +
                      offset + ")" + (locks ? "; the gate is today's again" : ""));
    }

    /// A step of "Auto"'s detection came into force, or went (0): counted for
    /// the session's last cadence line. Capture thread.
    void noteStep(int step)
    {
        const int64_t nowUs = steadyNowUs();
        if (m_AppliedStepFps > 0) m_SteppedUs += nowUs - m_SteppedSinceUs;
        if (step > 0) {
            m_SteppedSinceUs = nowUs;
            m_StepsApplied++;
        }
        m_AppliedStepFps = step;
    }

    /// Report an unrecoverable end once, from the loop thread.
    ///
    /// noexcept because it is called from run()'s catch block: a throw here
    /// would re-enter termination with the original cause already lost.
    /// One line at the end: what the display produced against what the stream
    /// carried. The number that says whether the cadence did its job — and,
    /// on a display faster than the stream, how much the wire was spared.
    void logCadence()
    {
        if (m_PresentsSeen == 0) return;
        const double seconds = (steadyNowUs() - m_LoopStartUs) / 1e6;
        char span[32];
        std::snprintf(span, sizeof(span), "%.1f", seconds);
        std::string line = "[native] cadence: " + hzString(m_DisplayMilliHz) + " Hz display, " +
                           std::to_string(m_CadenceFps) + " fps stream — " +
                           std::to_string(m_PresentsSeen) + " presents in " + span + " s";
        if (m_Cadence.enabled()) {
            // What the gate skipped and the window after a press sent all the
            // same was carried.
            int64_t skipped = m_Cadence.skipped() - (m_InputAdmit.frames() - m_WindowFramesAtGate);
            if (skipped < 0) skipped = 0;
            const int perSecond =
                seconds > 0 ? static_cast<int>(static_cast<double>(skipped) / seconds + 0.5) : 0;
            line += ", " + std::to_string(skipped) + " not carried (" + std::to_string(perSecond) +
                    "/s)";
        } else {
            line += ", every one carried";
        }
        log::info(line);

        // The lattice of the display's presents (plan « glissade »). Its
        // period and the gate's interval come first, exact: the bench's
        // replay reads them (gatesim.py). Then what the gate decided on it —
        // or would have, in the bench's arm without the lock.
        if (m_Lattice.taken() > 0) {
            const int64_t periodNs = m_Lattice.periodNs();
            const int64_t n = m_Cadence.lockRatio(periodNs);
            char share[96];
            std::snprintf(share, sizeof(share), "%.1f %% of the gate's %lld calls %s",
                          m_GateCalls > 0 ? 100.0 * static_cast<double>(m_LockableCalls) /
                                                static_cast<double>(m_GateCalls)
                                          : 0.0,
                          static_cast<long long>(m_GateCalls),
                          m_LockArmed ? "locked" : "would have been locked");
            char engaged[64] = "never engaged";
            if (m_Lattice.firstEngagedNs() >= 0)
                std::snprintf(
                    engaged, sizeof(engaged), "engaged %.2f s after the first present",
                    static_cast<double>(m_Lattice.firstEngagedNs() - m_Lattice.firstNs()) / 1e9);
            char sharp[64] = "no sharpness yet";
            if (m_Lattice.sharpness() >= 0)
                std::snprintf(sharp, sizeof(sharp), "sharpness %.3f (mean offset %lld µs)",
                              m_Lattice.sharpness(),
                              static_cast<long long>(m_Lattice.meanOffsetNs() / 1000));
            log::info("[native] cadence lock: " + cadenceLockHead(periodNs, m_Cadence) + ", " +
                      (n > 0 ? std::to_string(n) + " periods a picture"
                             : std::string("no whole number of periods a picture")) +
                      "; " +
                      cadenceLockState(m_Config.tuning.cadenceLock, productCadence(),
                                       m_CaptureApi == CaptureApi::DxgiDuplication, periodNs) +
                      ": " + share + ", " + std::to_string(m_LockedAfterEmpty) +
                      " after an empty window, " + std::to_string(m_LockedAfterPause) +
                      " after a pause; " + engaged + "; " +
                      std::to_string(m_Lattice.engagements()) + " engaged, " +
                      std::to_string(m_Lattice.releases()) + " let go, " +
                      std::to_string(m_Lattice.resyncs()) + " resyncs, " +
                      std::to_string(m_Lattice.restarts()) + " new lattices; " + sharp);
        }

        // The admission window after a press: the pictures it added, and what
        // kept it from adding more.
        if (m_InputAdmitOn) {
            const InputAdmit& a = m_InputAdmit;
            char rate[48];
            std::snprintf(rate, sizeof(rate), "%.2f/s, %.2f a window",
                          seconds > 0 ? static_cast<double>(a.frames()) / seconds : 0.0,
                          a.windows() > 0 ? static_cast<double>(a.frames()) / a.windows() : 0.0);
            log::info("[native] input admit: " + std::to_string(a.presses()) + " presses, " +
                      std::to_string(a.windows()) + " windows (" + std::to_string(a.carried()) +
                      " carried on by a press, " + std::to_string(a.refused()) +
                      " refused by the bucket, " + std::to_string(a.late()) + " too late), " +
                      std::to_string(a.frames()) + " pictures more (" + rate + "); held back: " +
                      std::to_string(a.capped()) + " by the cap, " + std::to_string(a.gapped()) +
                      " by the gap, " + std::to_string(a.linkRefused()) + " by the link, " +
                      std::to_string(a.encoderRefused()) + " by the encoder");
        }

        // The turns of the loop that had no present to carry, against the ones
        // that did: what the pointer alone costs the capture thread.
        if (seconds > 0) {
            const auto perSecond = [seconds](int64_t n) {
                return std::to_string(static_cast<int>(static_cast<double>(n) / seconds + 0.5));
            };
            log::info("[native] capture wake-ups: " + std::to_string(m_PresentsSeen) +
                      " presents (" + perSecond(m_PresentsSeen) + "/s), " +
                      std::to_string(m_PointerWakes) + " pointer-only (" +
                      perSecond(m_PointerWakes) + "/s), " + std::to_string(m_TimeoutWakes) +
                      " timeouts (" + perSecond(m_TimeoutWakes) + "/s)");
            // The desktop handed over again for the pointer (retakeDesktop).
            if (m_Retakes > 0) {
                char cost[64];
                std::snprintf(cost, sizeof(cost), "%.1f ms on average, %.1f at most",
                              static_cast<double>(m_RetakeTotalUs) / m_Retakes / 1000.0,
                              m_RetakeMaxUs / 1000.0);
                log::info("[native] cursor: the desktop handed over again " +
                          std::to_string(m_Retakes) + " time" + (m_Retakes > 1 ? "s" : "") +
                          " for the pointer, " + cost);
            }
            // Presents that happened while the loop was elsewhere — converting,
            // encoding, sending — and that the next acquire folded into one.
            // Against the time the loop spent waiting in the acquire, it says
            // whether frames are lost to a busy loop or never presented at all.
            const int64_t folded = m_Capture ? m_Capture->foldedPresents() : -1;
            char waited[32];
            std::snprintf(waited, sizeof(waited), "%.0f",
                          100.0 * static_cast<double>(m_AcquireWaitUs) / (seconds * 1e6));
            log::info("[native] capture loop: " +
                      (folded >= 0
                           ? std::to_string(folded) + " presents folded between acquires (" +
                                 perSecond(folded) + "/s), "
                           : std::string()) +
                      waited + " % of the time waiting in the acquire");
            if (m_PreGate)
                log::info("[native] pregate: " + std::to_string(m_PreGateSkips) +
                          " presents the gate skipped were not converted (" +
                          perSecond(m_PreGateSkips) + "/s)");
        }

        // Whether the capture held this display over its dense content, for
        // the server's `vddmatch` (CaptureHold.h), which acts on it only above
        // 240 Hz.
        m_Hold.tick(steadyNowUs());
        const CaptureHold::Verdict hold = m_Hold.verdict();
        m_HoldStatus.judged = hold.judged;
        m_HoldStatus.held = hold.held;
        m_HoldStatus.displayMilliHz = m_DisplayMilliHz;
        m_HoldStatus.gpu = m_Info.gpuName;
        m_HoldStatus.denseSeconds = hold.denseSeconds;
        m_HoldStatus.locked = hold.locked;
        m_HoldStatus.carried = hold.carried;
        m_HoldStatus.folded = hold.folded;
        m_HoldStatus.busy = hold.busy;
        m_HoldStatus.why = hold.why;
        if (hold.judged) {
            char figures[160];
            std::snprintf(figures, sizeof(figures),
                          "lattice %.1f %%, carried %.1f %%, folded %.2f %%, busy %.0f %%",
                          hold.locked >= 0 ? 100.0 * hold.locked : 0.0, 100.0 * hold.carried,
                          100.0 * hold.folded, 100.0 * hold.busy);
            log::info("[native] capture hold at " + hzString(m_DisplayMilliHz) + " Hz: " +
                      (hold.held ? std::string("held") : "not held (" + hold.why + ")") + " over " +
                      std::to_string(hold.denseSeconds) + " s of dense content — " + figures);
        } else {
            log::info("[native] capture hold at " + hzString(m_DisplayMilliHz) +
                      " Hz: not judged, " + std::to_string(hold.denseSeconds) +
                      " s of dense content (" + std::to_string(CaptureHold::kMinDenseSeconds) +
                      " needed)");
        }

        // cadence=deadline: the client's refreshes aimed at, and how on time.
        if (m_Config.tuning.cadence == EncoderTuning::Cadence::Deadline && seconds > 0) {
            const auto perSecond = [seconds](int64_t n) {
                return std::to_string(static_cast<int>(static_cast<double>(n) / seconds + 0.5));
            };
            char grid[96];
            std::snprintf(grid, sizeof(grid), "last period %.1f µs, lead %.1f ms",
                          m_Deadline.periodUs(), static_cast<double>(m_Deadline.leadUs()) / 1000.0);
            char late[96];
            std::snprintf(late, sizeof(late),
                          "woke %.0f µs late on average, %lld times over 0.5 ms (max %lld)",
                          m_DeadlineAims ? static_cast<double>(m_DeadlineLateUs) / m_DeadlineAims
                                         : 0.0,
                          static_cast<long long>(m_DeadlineLateWakes),
                          static_cast<long long>(m_DeadlineLateMaxUs));
            log::info("[native] deadline: " + std::to_string(m_DeadlineAims) +
                      " client refreshes aimed at (" + perSecond(m_DeadlineAims) + "/s), " +
                      std::to_string(m_DeadlineNothingNew) + " with nothing new; " + late + "; " +
                      std::to_string(m_Deadline.grids()) + " grids heard (" +
                      std::to_string(m_DeadlineRefused.load()) + " refused), " + grid +
                      (m_Deadline.budgetFps() > 0 && m_DeadlineAsItComes.load()
                           ? std::string(m_Deadline.tearing() ? "; the client tears"
                                                              : "; the client's link is uneven") +
                                 ", budget " +
                                 std::to_string(static_cast<int>(m_Deadline.budgetFps() + 0.5)) +
                                 " fps"
                           : std::string()));
        }

        // "Auto"'s detection: the steps the client asked for, and how long the
        // stream ran above the client's own rate.
        if (const int64_t asked = m_StepsAsked.load(); asked > 0) {
            int64_t steppedUs = m_SteppedUs;
            if (m_AppliedStepFps > 0) steppedUs += steadyNowUs() - m_SteppedSinceUs;
            char above[32];
            std::snprintf(above, sizeof(above), "%.1f", static_cast<double>(steppedUs) / 1e6);
            log::info("[native] cadence steps: " + std::to_string(asked) + " asked, " +
                      std::to_string(m_StepsApplied) + " applied, " +
                      std::to_string(m_StepsRefused.load()) + " refused; " + above +
                      " s above the client's rate");
        }

        // cadence=host-guarded: what the client's decode queue held back.
        if (m_Config.tuning.cadence == EncoderTuning::Cadence::HostGuarded && seconds > 0) {
            const auto perSecond = [seconds](int64_t n) {
                return std::to_string(static_cast<int>(static_cast<double>(n) / seconds + 0.5));
            };
            log::info("[native] decode credit: " + std::to_string(m_CreditSkips) +
                      " presents held back (" + perSecond(m_CreditSkips) + "/s), " +
                      std::to_string(m_CreditFlushes) +
                      " sent when the credit came back, nothing newer having come; " +
                      std::to_string(m_DecodeCredit.signals()) + " words from the client");
        }

        // linkhold=: what the relay's send queue held back.
        if (m_Config.tuning.linkHoldMs > 0 && seconds > 0) {
            const auto perSecond = [seconds](int64_t n) {
                return std::to_string(static_cast<int>(static_cast<double>(n) / seconds + 0.5));
            };
            log::info("[native] link hold: " + std::to_string(m_LinkHolds) +
                      " presents held back (" + perSecond(m_LinkHolds) + "/s), " +
                      std::to_string(m_LinkFlushes) +
                      " sent once the link drained, nothing newer having come");
        }

        // MWSR's zones, when the client read them: how many frames carried
        // their grid, and what the decision cost this thread.
        if (m_MwsrAskedFrames > 0) {
            char mean[16], peak[16];
            std::snprintf(mean, sizeof(mean), "%.2f",
                          m_MwsrZoneFrames > 0
                              ? static_cast<double>(m_MwsrDecideUs) / m_MwsrZoneFrames / 1000.0
                              : 0.0);
            std::snprintf(peak, sizeof(peak), "%.2f", m_MwsrDecideMaxUs / 1000.0);
            log::info("[native] MWSR zones: " + std::to_string(m_MwsrZoneFrames) + " of " +
                      std::to_string(m_MwsrAskedFrames) +
                      " frames carried their grid; the decision took " + mean + " ms mean, " +
                      peak + " ms max");
        }

        // The pipeline's own figures: what the cross-GPU bridge cost, when
        // there was one.
        if (m_Pipeline) m_Pipeline->logEndOfSession();
    }

    void finish(const std::string& reason) noexcept
    {
        m_Running.store(false);
        try {
            log::warning("[native] session ended: " + reason);
            if (m_Callbacks.onEnded) m_Callbacks.onEnded(reason);
        } catch (...) {
            // Nothing left to report it to.
        }
    }

    SessionConfig m_Config;
    /// The Selector's decision. Read, never re-derived.
    ResolvedTarget m_Target;
    SessionCallbacks m_Callbacks;
    SessionInfo m_Info;

    /// The rate the encoder's budget is dimensioned for — the setting, or the
    /// display's own refresh when the setting is 0. See start().
    int m_EncodeFps = 0;
    int m_DisplayMilliHz = 0;
    /// Holds the loop to the stream's rate; disabled at fps 0. Owned by the
    /// capture thread once the loop runs.
    FrameCadence m_Cadence{0};
    /// The rate the gate runs at: m_EncodeFps at start, re-chosen when the
    /// client's screen changes (chooseCadence). The encoder never follows it
    /// — its budget does, through EffectiveCadence::retarget.
    int m_CadenceFps = 0;
    /// The display's presents as a lattice of its refresh (PresentLattice.h),
    /// fed by the loop, a new one with each capture (armCadenceLock), and
    /// whether the gate may lock on it. For the log: the gate's calls, those
    /// it decided locked — or would have, the key off — the locked pictures
    /// after an empty window and after a pause, the lattice's lines said.
    /// Capture thread once the loop runs.
    PresentLattice m_Lattice;
    bool m_LockArmed = false;
    int64_t m_GateCalls = 0;
    int64_t m_LockableCalls = 0;
    int64_t m_LockedAfterEmpty = 0;
    int64_t m_LockedAfterPause = 0;
    int64_t m_LatticeLines = 0;
    /// The periods a display's mode is taken at for the lattice: 1000 Hz to
    /// 1 Hz. Anything else is a mode DXGI does not know.
    static constexpr int64_t kLatticeMinPeriodNs = 1000000;
    static constexpr int64_t kLatticeMaxPeriodNs = 1000000000;
    /// The admission window after a press (InputAdmit.h) may serve this
    /// session's client; set once in the constructor. Its presses come in on
    /// the input thread (sendInput), its pictures are the capture thread's.
    bool m_InputAdmitOn = false;
    InputAdmit m_InputAdmit;
    // Whether the capture held the display (CaptureHold.h); its verdict once
    // the loop ended, for captureHold().
    CaptureHold m_Hold;
    CaptureHoldStatus m_HoldStatus;
    /// The window's pictures when the gate was last replaced: those of the
    /// gate the last cadence line counts.
    int64_t m_WindowFramesAtGate = 0;
    /// The client's screen as last reported — SessionConfig at start, then
    /// setClientRefresh. The dirty flag wakes the loop's re-choice.
    std::atomic<int> m_ClientMilliHz{0};
    std::atomic<bool> m_ClientVsync{false};
    std::atomic<bool> m_ClientRefreshDirty{false};
    /// Frames per second the client asked not to exceed, 0 for none — see
    /// setClientFpsCap.
    std::atomic<int> m_ClientFpsCap{0};
    /// "Auto"'s detection (CadenceStep.h): the step the client was granted —
    /// written by setClientFpsStep, read by the loop's re-choice — and what
    /// the session tells it (cadenceStatus): the stream's rate without a step,
    /// the captured display's presents a second, its refresh, and the
    /// encoder's p95 a step is weighed against.
    std::atomic<int> m_StepFps{0};
    std::atomic<int> m_BaseFps{0};
    std::atomic<int> m_PresentsPerSecond{0};
    std::atomic<int> m_DisplayMilliHzShared{0};
    std::atomic<int64_t> m_EncodeP95Us{0};
    std::atomic<bool> m_EncoderOnePicture{false};
    std::atomic<int64_t> m_StepsAsked{0};
    std::atomic<int64_t> m_StepsRefused{0};
    /// The loop's own: the cadence without the step, the counters behind the
    /// figures above, and the time spent stepped, for the log.
    FrameCadence m_BaseCadence{0};
    int m_BaseCadenceFps = 0;
    /// The step the last choice was made with; the one the loop counts.
    int m_ChosenStepFps = 0;
    PresentRate m_PresentRate;
    EncodeTail m_EncodeTail;
    int64_t m_FoldedSeen = 0;
    int m_AppliedStepFps = 0;
    int64_t m_StepsApplied = 0;
    int64_t m_SteppedSinceUs = 0;
    int64_t m_SteppedUs = 0;
    /// The client's decode queue, read under cadence=host-guarded — see
    /// setClientDecodeQueue. Presents it held back, and the held pictures
    /// sent when it came back with nothing newer, for the log.
    DecodeCredit m_DecodeCredit;
    int64_t m_CreditSkips = 0;
    int64_t m_CreditFlushes = 0;
    /// The relay's send queue, read under linkhold= — see setLinkBusyProbe.
    /// Presents held back for it, and the held pictures sent once it drained
    /// with nothing newer, for the log.
    std::mutex m_LinkBusyMutex;
    LinkBusyProbe m_LinkBusyProbe;
    int64_t m_LinkHolds = 0;
    int64_t m_LinkFlushes = 0;
    /// The client's refresh grid, read under cadence=deadline — see
    /// setClientVsyncGrid. When the loop last aimed at it and the display's
    /// period, for the pong (vsyncGridStatus); the rest for the log.
    DeadlineCadence m_Deadline;
    std::atomic<int64_t> m_DeadlineRefused{0};
    std::atomic<int64_t> m_DeadlineAimedAtUs{0};
    std::atomic<bool> m_DeadlineAsItComes{false};
    std::atomic<int> m_DisplayPeriodUs{0};
    int64_t m_DeadlineAims = 0;
    int64_t m_DeadlineNothingNew = 0;
    int64_t m_DeadlineLateUs = 0;
    int64_t m_DeadlineLateMaxUs = 0;
    int64_t m_DeadlineLateWakes = 0;
    /// Presents the display delivered (AcquireStatus::Ok), for the log.
    int64_t m_PresentsSeen = 0;
    int64_t m_AcquireWaitUs = 0;
    /// Turns of the loop that carried no present: the pointer alone moved, or
    /// nothing did. A mouse swept at its report rate wakes the capture a
    /// thousand times a second between the presents — the log says how often.
    int64_t m_PointerWakes = 0;
    int64_t m_TimeoutWakes = 0;
    int64_t m_LoopStartUs = 0;
    /// pregate=1 (EncoderTuning::preGate): the gate asked before a present is
    /// converted, and how many presents it then spared the converter.
    bool m_PreGate = false;
    int64_t m_PreGateSkips = 0;

    std::unique_ptr<capture::IWindowsCapture> m_Capture;
    /// Which backend actually answered. Settled by openCapture(), reported in
    /// SessionInfo, and re-decided on every restart: a display that gains a
    /// duplication back (a driver restart, a mode change) should stop paying
    /// for the fallback.
    CaptureApi m_CaptureApi = CaptureApi::DxgiDuplication;
    /// Desktop Duplication was found to paint the pointer into the picture on
    /// this display: every (re)open goes straight to WGC. Capture thread only.
    bool m_DuplicationPaintsPointer = false;
    /// locateOutput() has run once: from then on it says when the display
    /// moved, rather than where it is. Capture thread only, after start().
    bool m_OutputLocated = false;
    /// Windows.Graphics.Capture stands in for a duplication refused for a
    /// reason that may pass (DxgiDuplication::refusalMayPass): the loop looks
    /// for it to come back — duplicationBack(), when to look next, and how
    /// many tries have failed. Capture thread only.
    bool m_DdaMayReturn = false;
    int64_t m_NextDdaLookUs = 0;
    int m_DdaTries = 0;
    static constexpr int64_t kDdaLookUs = 500 * 1000;
    static constexpr int64_t kDdaTryUs = 1000 * 1000;
    static constexpr int64_t kDdaTryMaxUs = 30 * 1000 * 1000;
    /// MW_DDA_REFUSE: when the variable was read (-1 before), and when Desktop
    /// Duplication is refused on purpose — none without it.
    int64_t m_RefuseDdaReadUs = -1;
    int64_t m_RefuseDdaFromUs = 0;
    int64_t m_RefuseDdaUntilUs = 0;
    bool m_DdaLostOnPurpose = false;
    /// retakeDesktop(): when it may run next; the picture it asked for, still
    /// to come, and when it asked; how long the capture took to say yes; and
    /// this session's giving up on it, said once. Capture thread only.
    int64_t m_NextRetakeUs = 0;
    bool m_RetakePending = false;
    int64_t m_RetakeAskedUs = 0;
    int64_t m_RetakeOpenUs = 0;
    bool m_RetakeOff = false;
    /// For the log: the pictures retakes brought, and what they cost the loop
    /// from the question to the picture.
    int64_t m_Retakes = 0;
    int64_t m_RetakeTotalUs = 0;
    int64_t m_RetakeMaxUs = 0;
    static constexpr int64_t kRetakeIntervalUs = 1000 * 1000;
    static constexpr int64_t kRetakeWaitUs = 500 * 1000;
    /// Held from start() to stop(); see StreamPriority.
    StreamPriority m_Priority;
    /// Everything between the captured picture and the bitstream: the
    /// converter, the encoder, the held desktop, and the cross-GPU bridge when
    /// there is one. Held by interface: the loop below neither knows nor needs
    /// to know which pipeline this is. See WindowsVideoPipeline.
    std::unique_ptr<WindowsVideoPipeline> m_Pipeline;
    /// Which chain the last build runs, and why (VideoPipelineChoice).
    VideoPipelineChoice m_PipelineChoice;
    /// The D3D12 chain failed while streaming: this session stays on D3D11,
    /// and says why (plan pipeline-video-d3d12-v2 §3.3).
    bool m_D3d12Failed = false;
    std::string m_D3d12FailedWhy;
    /// A D3D12 build came back without the intra-refresh the stream requires
    /// (SessionConfig::intraRefreshRequired): the builds after it choose
    /// D3D11 from the start (VideoPipelineFacts::d3d12IntraRefresh).
    bool m_D3d12NoIntraRefresh = false;
    /// The pictures come from the guests' shared feed (VideoSource::External):
    /// input and audio only, no capture, no pipeline, no loop. Written once
    /// by start(), before anything reads it.
    bool m_External = false;
    /// The chain answered Lost: the loop goes back to D3D11 before its next
    /// capture. Capture thread only.
    bool m_PipelineLost = false;
    /// The SDR white the converter holds, in scRGB; 0 until a pipeline has
    /// read one, so the first read after a rebuild is always applied and
    /// logged. Capture thread only — see applySdrWhite.
    float m_SdrWhite = 0.0f;

    /// Optional: a session with no input sink still streams, view-only. Guarded
    /// because it is created and destroyed on the session's thread but used on
    /// the network thread.
    std::mutex m_InputMutex;
    /// What is held, to tell a press from a repeat (InputAdmit). Guarded by
    /// m_InputMutex.
    PressFilter m_PressFilter;
    /// The bench's click trace (clicktrace=1). Ahead of the sink, which writes
    /// into it: it is destroyed after it.
    ClickTrace m_ClickTrace;
    /// DWM refused its timing once: said once, then left out.
    bool m_DwmTimingRefused = false;
    std::unique_ptr<input::IInputSink> m_Input;
    /// See setInputGateCallback. Kept here so a listener registered before the
    /// sink exists is not lost. Guarded by m_InputMutex.
    InputGateCallback m_OnInputGate;

    /// See setDisplayFormatCallback, and the last format reported — or the one
    /// the session started on — so only a change is said. Guarded by
    /// m_FormatMutex: set on the consumer's thread, read on the capture thread.
    std::mutex m_FormatMutex;
    DisplayFormatCallback m_OnDisplayFormat;
    DisplayFormat m_LastFormat;

    /// Optional too: the host's playback, captured and encoded on its own
    /// thread. Null when the consumer asked for none or no device could open.
    std::unique_ptr<audio::WasapiLoopback> m_Audio;
    audio::HostMute m_HostMute;

    std::thread m_Thread;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_ForceKeyframe{true};
    /// True — the default — draws the pointer into the picture. False reports
    /// its shape to the client, which draws it itself at its own refresh rate.
    std::atomic<bool> m_CompositeCursor{true};
    /// How wide the client wants the composited pointer, in frame pixels; 0 for
    /// the size it has on the desktop. See Session::setCompositeCursor.
    std::atomic<int> m_CursorFramePx{0};
    /// The rate the client wants kept up on a screen that is not moving, in
    /// fps; 0 for the loop's own liveness floor. See setFrameFloorFps.
    std::atomic<int> m_FloorFps{0};
    /// The composited pointer must be redrawn although nothing on the desktop
    /// moved — its requested size changed under a still screen.
    std::atomic<bool> m_CursorDirty{false};
    /// The pointer went from the client to the picture or back
    /// (setCompositeCursor): the loop draws the held desktop again.
    std::atomic<bool> m_PointerChangedHands{false};
    /// Forces one cursor report even though DXGI says the shape is unchanged.
    std::atomic<bool> m_ResendCursor{false};
    /// The shape the client has been told about, so an unchanged pointer is not
    /// re-sent on every frame.
    uint64_t m_ReportedShape = 0;
    bool m_ReportedVisible = false;
    /// See recentrePointerIfAway(): when Windows was last asked, whether the
    /// start is over, and whether the one line has been said. Capture-thread
    /// only.
    int64_t m_LastRecentreCheckUs = 0;
    bool m_RecentreDone = false;
    int m_RecentreTries = 0;
    static constexpr int kMaxRecentreTries = 5;
    bool m_LoggedRecentre = false;
    /// See keepPointerShown(): start(), then the capture thread, then stop(),
    /// one after the other — never two at once.
    Win32PointerSystem m_PointerSystem;
    PointerPresence m_PointerPresence{m_PointerSystem};
    MouseKeysCallback m_OnMouseKeys;
    std::string m_ReportedKind;
    /// Deliberately not 1: the first report must go out whatever the scale is,
    /// and a sentinel that no ratio can equal is what guarantees it.
    float m_ReportedScale = 0.0f;
    /// When the pointer's position last went out to a self-drawing client.
    CursorPositionGate m_PositionGate;
    bool m_LoggedFirstKeyframe = false;
    /// Bytes the last emit() produced. The refinement loop reads it to know
    /// when a still picture has stopped improving.
    size_t m_LastEmitBytes = 0;
    /// The average QP the encoder reported for the last emit(), -1 when it
    /// reports none. The refinement loop's second witness of convergence.
    int m_LastEmitQp = -1;
    /// MWSR's zones of the frame being delivered (attachMwsrZones), and what
    /// the end of the session says of them: frames that asked, frames that
    /// carried a grid, the decision's time in all and at its longest.
    uint8_t m_MwsrZones[convert::mwsrzones::kZones] = {};
    int64_t m_MwsrAskedFrames = 0;
    int64_t m_MwsrZoneFrames = 0;
    int64_t m_MwsrDecideUs = 0;
    int64_t m_MwsrDecideMaxUs = 0;

    /// A picture the encode thread encoded (pipelined=1, emitLater), for what
    /// emit() does after the delivery — on the capture thread, which owns it
    /// (absorbDelivered).
    struct Delivered
    {
        WindowsVideoPipeline::EncodeResult result = WindowsVideoPipeline::EncodeResult::Ok;
        std::string error;
        size_t bytes = 0;
        int qp = -1;
        int64_t encodedUs = 0;
        int linkKbps = 0;
        /// Handed to onVideo: the frame, its data gone with the encoder's buffer.
        bool sent = false;
        EncodedFrame frame;
        FrameStamps stamps;
    };
    std::mutex m_DeliveredMutex;
    std::vector<Delivered> m_Delivered;
    /// Pictures absorbed but not yet counted by the loop's cadence.
    int m_DeliveredUncounted = 0;
    /// The flattened image handed to the client. Reused so a shape change does
    /// not allocate on the capture thread.
    std::vector<uint8_t> m_CursorScratch;
    /// Zero means "no change pending". Exchanged by the loop each iteration.
    std::atomic<int> m_PendingBitrate{0};

    /// The size the session was opened at, which the cap scales FROM — never
    /// from the current one, or a run of reductions would compound.
    int m_FullWidth = 0;
    int m_FullHeight = 0;

    /// The GDI device whose mode this session changed ("Match my screen"),
    /// to put back at stop(), and the refresh of the mode it was put in.
    /// Empty / 0 when none was.
    std::wstring m_ModeChangedDevice;
    int m_ModeChangedHz = 0;
    /// That mode, set or taken over, in one word (DisplayModeHold.h): what
    /// setClientModeAgain() puts back. 0 when none.
    uint64_t m_ModeSet = 0;
    /// Whether that mode is still this session's to put back (DisplayModeHold.h).
    std::unique_ptr<platform::DisplayModeHold> m_ModeHold;
    encode::EncodeLoadCap m_LoadCap;
    std::atomic<bool> m_PendingResize{false};
    /// The resample guard (noteScalerLoad): GPU time of the frames seen so far
    /// in the current window, and whether MW_SCALER pinned the filter, which
    /// the guard then leaves alone — an A/B is not to be overruled.
    int64_t m_ScalerWindowUs = 0;
    int m_ScalerWindowFrames = 0;
    bool m_ScalerPinned = false;
    /// Frames the receiver reported lost, waiting for the capture thread to
    /// tell the encoder — see invalidateReference(). Bounded: past the DPB's
    /// reach a keyframe is the honest answer.
    static constexpr size_t kMaxPendingInvalidations = 16;
    std::mutex m_InvalidateMutex;
    std::vector<uint32_t> m_PendingInvalidations;
    int m_Invalidated = 0;
    int m_InvalidateFallbacks = 0;
    /// The receiver's latest report on the link, folded until the loop takes
    /// it — see reportLink().
    std::mutex m_LinkMutex;
    LinkFeedback m_LinkFeedback;
    bool m_LinkPending = false;
    /// What the link is still carrying, by the stream's own rate. Fed by every
    /// emit(); read by the refinement burst, which does not send a pass into a
    /// link that has not finished with the previous one. See RateControl.h.
    encode::LinkOccupancy m_Link;
    /// The rate the link is modelled at: the stream's bitrate, never the
    /// still-screen boost — that one is what is being paced, not the pipe.
    int m_LinkKbps = 0;
    /// The rate the encoder holds: what it was built with, then every
    /// setBitrate() it took — scaled to the cadence frames arrive at, boost
    /// included. Stamped on each frame (EncodedFrame::encoderKbps).
    int m_EncoderKbps = 0;
};

} // namespace

namespace detail {

std::unique_ptr<Session> createPlatformSession(const SessionConfig& config,
                                               const ResolvedTarget& target,
                                               const SessionCallbacks& callbacks,
                                               std::string& error)
{
    // A session of the guests' shared feed encodes nothing: its pictures come
    // from the feed's process, and only it may go without a video callback.
    if (!callbacks.onVideo && config.videoSource != VideoSource::External) {
        error = "a session without a video callback would encode into nothing";
        return nullptr;
    }
    // Which encoder to build is decided in start(), from target.encoder — the
    // Selector's choice, not a guess made here.
    return std::make_unique<WindowsSession>(config, target, callbacks);
}

} // namespace detail
} // namespace mw::native
