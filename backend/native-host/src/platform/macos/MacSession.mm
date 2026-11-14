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

#include "FrameFit.h"
#include "MacDisplays.h"

#include "../../audio/PacedOpusSink.h"
#include "../../audio/macos/HostMute.h"
#include "../../capture/macos/SckCapture.h"
#include "../../convert/CursorBlend.h"
#include "../../core/CadenceAlign.h"
#include "../../core/ClickTrace.h"
#include "../../core/CursorPositionGate.h"
#include "../../core/FrameCadence.h"
#include "../../core/Log.h"
#include "../../core/RestartBackoff.h"
#include "../../core/Selector.h"
#include "../../core/Session.h"
#include "../../encode/RateControl.h"
#include "../../encode/RateGovernor.h"
#include "../../encode/macos/VtEncoder.h"
#include "../../input/macos/CgInput.h"

#import <AppKit/AppKit.h>

#include <CoreVideo/CoreVideo.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The macOS capture → encode → deliver pipeline.
//
// A port of WindowsSession/LinuxSession, stage for stage: the same single
// thread with no queue, the same cadence gate, the same still-screen floor and
// refinement burst, the same three-layer bitrate, the same restart on a lost
// display. Where it differs it is because the platform does:
//
//  - there is NO conversion stage: ScreenCaptureKit writes the encoder's NV12
//    directly (SckCapture.h) — or its 10-bit BT.2020 PQ for HDR, which the
//    Apple Video Encoder takes as HEVC Main10 — so a frame goes capture →
//    encoder in place;
//  - the pointer in the picture is the compositor's (showsCursor), at its own
//    size. When a small screen asks for a MAGNIFIED pointer the capture is
//    told to leave it out and the engine blends AppKit's account of the
//    system cursor into the compositor's buffer before encoding
//    (convert/CursorBlend.h), undoing its own drawing before re-encoding the
//    same buffer. The shape for a client that draws its own comes from the
//    same AppKit source, polled, not from the capture;
//  - a pointer-only path exists only in that magnified mode: with the pointer
//    left out of the capture, a move on a still screen makes no frame, so the
//    loop looks at the pointer at the stream's rate and re-encodes the held
//    picture when it moved;
//  - the sound arrives as a second output of the SAME ScreenCaptureKit stream
//    (macOS has no loopback device to open), so it is set up on the capture
//    and paced into Opus by the platform-neutral sink;
//  - no reference invalidation, no intra-refresh (VideoToolbox has neither),
//    no virtual gamepad.
//
// See §20 of docs/design/native-capture-encoder.md.

namespace mw::native {
namespace {

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct FrameStamps
{
    int64_t presentUs = 0;
    int64_t capturedUs = 0;
    int64_t submittedUs = 0;
    int64_t convertedUs = 0;
};

FrameStamps resendStamps(int64_t nowUs)
{
    return FrameStamps{nowUs, nowUs, nowUs, nowUs};
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

/// mach_absolute_time ticks → microseconds (1:1 on Apple Silicon, 125:3 on
/// Intel), as SckCapture converts.
int64_t machToUs(uint64_t ticks)
{
    static mach_timebase_info_data_t info = [] {
        mach_timebase_info_data_t i;
        mach_timebase_info(&i);
        return i;
    }();
    return static_cast<int64_t>(ticks * info.numer / info.denom / 1000);
}

// The captured display's refresh, for the bench's click trace (clicktrace=1):
// macOS has no DWM timing to read after a capture, and the nearest is a
// CVDisplayLink on that display — the vsync it last reported, its period, and
// how many it reported. Nothing of the window server's composition: no API
// says when it composed.
//
// CVDisplayLink is deprecated since macOS 15 for the display links of NSView
// and NSScreen, which want a view or a screen object; this one only wants the
// display's id, which is what the session has. Bench only.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
class DisplayClock
{
public:
    DisplayClock() = default;
    DisplayClock(const DisplayClock&) = delete;
    DisplayClock& operator=(const DisplayClock&) = delete;
    ~DisplayClock() { stop(); }

    uint32_t display() const { return m_Display; }

    void start(uint32_t display)
    {
        stop();
        m_Display = display;
        m_Count.store(0);
        if (CVDisplayLinkCreateWithCGDisplay(display, &m_Link) != kCVReturnSuccess || !m_Link) {
            m_Link = nullptr;
            log::warning("[native] click trace: no CVDisplayLink on display " +
                         std::to_string(display) + " — the trace goes without its refresh");
            return;
        }
        CVDisplayLinkSetOutputCallback(m_Link, &DisplayClock::tick, this);
        if (CVDisplayLinkStart(m_Link) != kCVReturnSuccess) {
            log::warning("[native] click trace: the CVDisplayLink of display " +
                         std::to_string(display) + " would not start");
            stop();
            m_Display = display; // not retried on every wake-up
        }
    }

    void stop()
    {
        if (m_Link) {
            CVDisplayLinkStop(m_Link);
            CVDisplayLinkRelease(m_Link);
            m_Link = nullptr;
        }
        m_Display = 0;
    }

    /// The last vsync reported, on the steady clock, the refresh period and
    /// how many vsyncs since start(); false before the first.
    bool read(int64_t& vsyncUs, int64_t& periodUs, int64_t& count) const
    {
        count = m_Count.load();
        if (count == 0) return false;
        vsyncUs = m_VsyncUs.load();
        periodUs = m_PeriodUs.load();
        return true;
    }

private:
    static CVReturn tick(CVDisplayLinkRef, const CVTimeStamp* now, const CVTimeStamp*,
                         CVOptionFlags, CVOptionFlags*, void* context)
    {
        auto* self = static_cast<DisplayClock*>(context);
        // Mach time onto the steady clock through "how long ago", as
        // SckCapture does: the two tick together.
        const int64_t agoUs = machToUs(mach_absolute_time()) - machToUs(now->hostTime);
        self->m_VsyncUs.store(steadyNowUs() - agoUs);
        if (now->videoTimeScale > 0 && now->videoRefreshPeriod > 0)
            self->m_PeriodUs.store(static_cast<int64_t>(now->videoRefreshPeriod) * 1000000 /
                                   now->videoTimeScale);
        self->m_Count.fetch_add(1);
        return kCVReturnSuccess;
    }

    CVDisplayLinkRef m_Link = nullptr;
    uint32_t m_Display = 0;
    std::atomic<int64_t> m_VsyncUs{0};
    std::atomic<int64_t> m_PeriodUs{0};
    std::atomic<int64_t> m_Count{0};
};
#pragma clang diagnostic pop

std::string hzString(int milliHz)
{
    return std::to_string((milliHz + 500) / 1000);
}

constexpr int kMaxFloorFps = 480;
/// How often the system pointer's shape is looked at, at most.
constexpr int64_t kCursorPollUs = 50 * 1000;
/// The most a pointer is grown for a small screen — the same bound as the
/// Windows converter, and for the same reason: past it the pointer is a
/// blurred thumbnail, not a bigger pointer.
constexpr float kMaxCursorMagnify = 2.5f;

uint64_t fnv1a(const uint8_t* data, size_t size)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

/// The system pointer as a BGRA bitmap at the display's own pixel scale, plus
/// the hotspot in those pixels. AppKit's account of what is on screen — the
/// one process-independent source macOS offers.
struct CursorShape
{
    int width = 0;
    int height = 0;
    int hotspotX = 0;
    int hotspotY = 0;
    std::vector<uint8_t> pixels;
    uint64_t hash = 0;
};

bool rasterize(NSCursor* cursor, double scale, CursorShape& shape)
{
    if (!cursor) return false;
    NSImage* image = cursor.image;
    if (!image) return false;
    const NSSize points = image.size;
    if (points.width <= 0 || points.height <= 0) return false;

    NSAffineTransform* transform = [NSAffineTransform transform];
    [transform scaleBy:scale];
    NSRect rect = NSMakeRect(0, 0, points.width, points.height);
    CGImageRef cg = [image CGImageForProposedRect:&rect
                                          context:nil
                                            hints:@{NSImageHintCTM : transform}];
    if (!cg) return false;
    const size_t w = CGImageGetWidth(cg);
    const size_t h = CGImageGetHeight(cg);
    if (w == 0 || h == 0 || w > 512 || h > 512) return false;

    shape.width = static_cast<int>(w);
    shape.height = static_cast<int>(h);
    shape.pixels.assign(w * h * 4, 0);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx =
        CGBitmapContextCreate(shape.pixels.data(), w, h, 8, w * 4, space,
                              kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(space);
    if (!ctx) return false;
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cg);
    CGContextRelease(ctx);

    // The hotspot is in points of the image; the bitmap is in its pixels.
    const NSPoint hot = cursor.hotSpot;
    shape.hotspotX = static_cast<int>(hot.x * (static_cast<double>(w) / points.width));
    shape.hotspotY = static_cast<int>(hot.y * (static_cast<double>(h) / points.height));
    shape.hash = fnv1a(shape.pixels.data(), shape.pixels.size());
    return true;
}

class MacSession final : public Session
{
public:
    MacSession(const SessionConfig& config, const ResolvedTarget& target,
               const SessionCallbacks& callbacks)
        : m_Config(config)
        , m_Target(target)
        , m_Callbacks(callbacks)
    {}

    ~MacSession() override { stop(); }

    bool start(std::string& error) override
    {
        if (m_Running.load()) return true;

        // The Selector hands over the GPU (its Metal registry id) and the
        // display's index among that GPU's displays; resolved back to the
        // CGDirectDisplayID from the same list the probe numbered.
        {
            unsigned index = 0;
            bool found = false;
            for (const platform::MacDisplay& d : platform::listDisplays()) {
                if (d.gpuRegistryId != m_Target.captureAdapterHandle) continue;
                if (index == m_Target.outputIndex) {
                    m_Display = d;
                    found = true;
                    break;
                }
                ++index;
            }
            if (!found) {
                error = "the display is no longer attached";
                return false;
            }
        }
        m_DisplayMilliHz = m_Display.refreshMilliHz;

        // A dark panel presents nothing to capture. Wake it the way a touch
        // of the trackpad would, and keep it awake for as long as the session
        // runs — a viewer streaming a Mac is using it, whatever its idle
        // timer thinks. Both are power-management assertions; neither needs
        // a privilege.
        {
            wakeDisplay();
            if (IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,
                                            kIOPMAssertionLevelOn,
                                            CFSTR("MoonlightWeb is streaming this display"),
                                            &m_KeepAwake) != kIOReturnSuccess)
                m_KeepAwake = kIOPMNullAssertionID;
            if (m_Display.isAsleep) log::info("[native] display was asleep — woken for the stream");
        }

        // The process has no window of its own, so macOS may nap it: timers
        // coalesced, the capture and encode threads put on efficiency cores
        // and slowed down. The same opt-out a game takes, for the session's
        // life — MW_APPNAP=keep leaves it to the OS for A/B benches.
        {
            const char* appNap = std::getenv("MW_APPNAP");
            if (appNap && std::strcmp(appNap, "keep") == 0) {
                log::info("[native] priority: MW_APPNAP=keep — macOS may still nap this process");
            } else if (!m_Activity) {
                m_Activity = [[NSProcessInfo processInfo]
                    beginActivityWithOptions:(NSActivityUserInitiated | NSActivityLatencyCritical)
                                      reason:@"MoonlightWeb is streaming this display"];
                log::info("[native] priority: out of App Nap, latency-critical");
            }
        }

        // The client's screen as /start said it, for the loop's re-choices: a
        // decoder that asks for fewer frames re-chooses too (setClientFpsCap),
        // and found an unknown screen that tears instead.
        m_ClientMilliHz.store(m_Config.clientRefreshMilliHz);
        m_ClientVsync.store(m_Config.clientVsync);
        {
            std::string line;
            m_EncodeFps =
                chooseCadence(m_Config.clientRefreshMilliHz, m_Config.clientVsync, m_Cadence, line);
            m_CadenceFps = m_EncodeFps;
            log::info(line);
        }

        // Audio, on the same terms as input: wanted only when the consumer
        // gave us somewhere to put it, and never a reason to fail the session.
        // Started BEFORE the capture, because the capture is what feeds it.
        if (m_Callbacks.onAudio) {
            // Silence the speakers first, on the same terms as Windows: only
            // when someone is listening at the other end, best effort, and
            // said in the log either way. Unlike Windows the order carries no
            // meaning here — the tap is not on the output device's path
            // (HostMute.h) — but the two platforms stay readable side by side.
            if (m_Config.muteHostAudio) {
                std::string how;
                m_HostMute.engage(how);
                log::info(std::string("[native] audio: ") + how);
                // What it achieved is read back into m_Info below: this
                // function clears m_Info AFTER this point (unlike the Windows
                // one, which clears it before), so setting the flag here would
                // be quietly wiped.
            }
            auto sink = std::make_unique<audio::PacedOpusSink>(m_Callbacks.onAudio,
                                                               m_Config.tuning.audioFrameSamples());
            std::string audioError;
            if (sink->start("ScreenCaptureKit, 48 kHz stereo", audioError))
                m_Audio = std::move(sink);
            else
                log::warning("[native] audio unavailable, streaming silent: " + audioError);
        }

        // "Match my screen": the display in the client's own mode, before the
        // capture opens so the first frame is already the right size. Same
        // policy as Windows (applyClientMode there); no mode to switch to and
        // the session becomes what Auto would have been.
        const bool modeChanged = applyClientMode();
        const bool fellBack = !modeChanged && fallBackFromMatch(m_Config);
        if (fellBack)
            log::info("[native] \"Match my screen\" falls back to Auto: fitting " +
                      std::to_string(m_Config.width) + "x" + std::to_string(m_Config.height));
        if (modeChanged || fellBack) {
            // Fitted against what the client ASKED once the mode has changed —
            // the Selector's frame was shaped to the old mode (see Windows).
            const FrameSize box = modeChanged
                                      ? FrameSize{m_Config.requestedWidth, m_Config.requestedHeight}
                                      : FrameSize{m_Config.width, m_Config.height};
            const FrameSize frame = frameForDisplay({m_Display.pixelWidth, m_Display.pixelHeight},
                                                    box, policyOf(m_Config));
            log::info(std::string("[native] the frame follows ") +
                      (modeChanged ? "the display's new mode: " : "the Auto box: ") +
                      std::to_string(m_Config.width) + "x" + std::to_string(m_Config.height) +
                      " -> " + std::to_string(frame.width) + "x" + std::to_string(frame.height));
            m_Config.width = frame.width;
            m_Config.height = frame.height;
        }

        if (!openCapture(m_Config.width, m_Config.height, error)) return false;
        if (m_Audio && !m_Capture->audioActive()) {
            // The stream took no audio tap (macOS 12). Nothing will ever push,
            // so the sink would tick out pure silence for the whole session.
            m_Audio.reset();
        }
        if (!buildEncoder(error)) return false;

        {
            std::lock_guard<std::mutex> lock(m_InputMutex);
            auto sink = std::make_unique<input::CgInput>();
            const capture::DesktopRect rect = m_Capture->desktopRect();
            sink->setDisplayRect(rect.left, rect.top, rect.right, rect.bottom);
            if (m_Config.tuning.clickTrace) sink->setClickTrace(&m_ClickTrace);
            std::string inputError;
            if (sink->start(inputError))
                m_Input = std::move(sink);
            else
                log::warning("[native] input: keyboard and mouse unavailable — " + inputError);
            log::info("[native] input: no virtual gamepad on macOS (a signed DriverKit "
                      "extension would be needed)");
        }

        m_Info = SessionInfo{};
        m_Info.displayId = m_Target.displayId;
        m_Info.width = m_Capture->width();
        m_Info.height = m_Capture->height();
        m_Info.fps = m_Config.fps;
        m_Info.codec = m_Target.codec;
        m_Info.encoder = m_Target.encoder;
        m_Info.capture = CaptureApi::ScreenCaptureKit;
        m_Info.gpuName = m_Target.encodeGpuName;
        // The Selector granted it only with an EDR panel, macOS 15 and HEVC;
        // the capture and the encoder were opened on it above.
        m_Info.hdr = m_Target.hdr;
        // What the viewer weighs its own screen against (SessionInfo). macOS has
        // no HDR switch — an EDR panel always has its headroom — but a mode
        // change is reported through reportDisplayFormat. hdrCapable already
        // carries the macOS 15 capture the probe requires.
        m_Info.displayWidth = m_Display.pixelWidth;
        m_Info.displayHeight = m_Display.pixelHeight;
        m_Info.hdrCapable = m_Target.hdrCapable;
        m_Info.displayHdr = m_Display.hdr && m_Target.hdrCapable;
        m_ModePixelWidth = m_Display.pixelWidth;
        m_ModePixelHeight = m_Display.pixelHeight;
        {
            std::lock_guard<std::mutex> lock(m_FormatMutex);
            m_LastFormat = DisplayFormat{m_Info.displayWidth, m_Info.displayHeight, m_Info.width,
                                         m_Info.height,       m_Info.displayHdr,    m_Info.hdr,
                                         m_Info.hdrCapable};
        }
        m_Info.yuv444 = false;
        m_Info.intraRefresh = false;
        m_Info.intraRefreshFrames = 0;
        m_Info.referenceInvalidation = false;
        // The compositor writes NV12 the encoder reads in place: one copy
        // remains, the bitstream leaving VRAM.
        m_Info.copiesPerFrame = 1;
        m_Info.crossGpuCopy = false;
        m_Info.audio = static_cast<bool>(m_Audio);
        m_Info.hostMuted = m_HostMute.strategy() != audio::HostMute::Strategy::None;

        log::info(std::string("[native] session: ") + m_Display.name + " " +
                  std::to_string(m_Info.width) + "x" + std::to_string(m_Info.height) + "@" +
                  std::to_string(m_EncodeFps) + " " + toString(m_Info.codec) +
                  (m_Info.hdr ? " HDR (Main10, BT.2020 PQ)" : "") + " via VideoToolbox on " +
                  m_Info.gpuName +
                  " — ScreenCaptureKit → VideoToolbox, no conversion stage, 1 copy (the "
                  "bitstream)");

        m_Running.store(true);
        m_Thread = std::thread([this] { run(); });
        return true;
    }

    void stop() override
    {
        const bool wasRunning = m_Running.exchange(false);
        if (m_Thread.joinable()) {
            if (std::this_thread::get_id() == m_Thread.get_id())
                m_Thread.detach();
            else
                m_Thread.join();
        }
        {
            std::lock_guard<std::mutex> lock(m_InputMutex);
            if (m_Input) m_Input->stop();
            m_Input.reset();
        }
        m_DisplayClock.stop();
        if (m_KeepAwake != kIOPMNullAssertionID) {
            IOPMAssertionRelease(m_KeepAwake);
            m_KeepAwake = kIOPMNullAssertionID;
        }
        if (m_Wake != kIOPMNullAssertionID) {
            IOPMAssertionRelease(m_Wake);
            m_Wake = kIOPMNullAssertionID;
        }
        if (m_Activity) {
            [[NSProcessInfo processInfo] endActivity:m_Activity];
            m_Activity = nil;
        }
        if (!wasRunning && !m_Encoder && !m_Capture && !m_Audio) {
            // Nothing was streaming, but a mute may still have been engaged by
            // a start() that failed after it. The speakers come back either way,
            // and so does a display mode changed before the capture refused.
            m_HostMute.release();
            restoreDisplayMode();
            return;
        }
        m_Encoder.reset();
        // The capture goes first: stopping it is what guarantees no audio
        // callback is still in flight when the sink it points at is freed.
        m_Capture.reset();
        m_Audio.reset();
        // After the capture is closed: the speakers come back.
        m_HostMute.release();
        // And the display its own mode.
        restoreDisplayMode();
    }

    // ── "Match my screen" ────────────────────────────────────────────────
    //
    // The client wants its own pixels, one for one: a display mode of the
    // client screen's size in PIXELS. macOS lists each size twice on a Retina
    // panel — 1:1 and HiDPI (twice the pixels of its points) — so a size is
    // matched on pixels, and between two candidates of the same pixels the one
    // with the scale the display already has wins: the desktop keeps the look
    // its user chose. The exact size is taken at the highest refresh; otherwise
    // the largest listed mode of the client's shape (within 0.5%) that fits
    // inside it; otherwise nothing changes. kCGConfigureForAppOnly: the change
    // is the session's — undone at stop(), and by macOS if the process dies.

    /// @returns true when the display's mode was changed for this session.
    bool applyClientMode()
    {
        if (!m_Config.matchClientDisplay || !m_Config.fitRequestedBox) return false;
        const int wantW = m_Config.requestedWidth, wantH = m_Config.requestedHeight;
        if (wantW <= 0 || wantH <= 0) return false;
        if (m_Display.pixelWidth == wantW && m_Display.pixelHeight == wantH) return false;

        const CGDirectDisplayID id = m_Display.displayId;
        CGDisplayModeRef current = CGDisplayCopyDisplayMode(id);
        if (!current) return false;
        const bool currentHiDpi =
            CGDisplayModeGetPixelWidth(current) > CGDisplayModeGetWidth(current);
        const double wantAspect = static_cast<double>(wantW) / wantH;

        NSDictionary* options =
            @{(__bridge NSString*)kCGDisplayShowDuplicateLowResolutionModes : @YES};
        CFArrayRef modes = CGDisplayCopyAllDisplayModes(id, (__bridge CFDictionaryRef)options);
        CGDisplayModeRef best = nullptr;
        bool exact = false;
        long long bestArea = 0;
        // Ranks two modes of the same pixel size: the display's own scale,
        // then the higher refresh.
        const auto better = [currentHiDpi](CGDisplayModeRef a, CGDisplayModeRef b) {
            const bool aSame =
                (CGDisplayModeGetPixelWidth(a) > CGDisplayModeGetWidth(a)) == currentHiDpi;
            const bool bSame =
                (CGDisplayModeGetPixelWidth(b) > CGDisplayModeGetWidth(b)) == currentHiDpi;
            if (aSame != bSame) return aSame;
            return CGDisplayModeGetRefreshRate(a) > CGDisplayModeGetRefreshRate(b);
        };
        const CFIndex count = modes ? CFArrayGetCount(modes) : 0;
        for (CFIndex i = 0; i < count; ++i) {
            auto mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
            if (!CGDisplayModeIsUsableForDesktopGUI(mode)) continue;
            const int w = static_cast<int>(CGDisplayModeGetPixelWidth(mode));
            const int h = static_cast<int>(CGDisplayModeGetPixelHeight(mode));
            if (w <= 0 || h <= 0) continue;
            if (w == wantW && h == wantH) {
                if (!exact || better(mode, best)) best = mode;
                exact = true;
                continue;
            }
            if (exact || w > wantW || h > wantH) continue;
            const double aspect = static_cast<double>(w) / h;
            if (std::abs(aspect - wantAspect) / wantAspect > 0.005) continue;
            const long long area = static_cast<long long>(w) * h;
            if (area > bestArea || (area == bestArea && better(mode, best))) {
                best = mode;
                bestArea = area;
            }
        }
        if (!best) {
            log::info("[native] " + m_Display.name + " lists no mode of " + std::to_string(wantW) +
                      "x" + std::to_string(wantH) + " or of its shape — the display keeps " +
                      std::to_string(m_Display.pixelWidth) + "x" +
                      std::to_string(m_Display.pixelHeight));
            if (modes) CFRelease(modes);
            CGDisplayModeRelease(current);
            return false;
        }

        const int newW = static_cast<int>(CGDisplayModeGetPixelWidth(best));
        const int newH = static_cast<int>(CGDisplayModeGetPixelHeight(best));
        const double newHz = CGDisplayModeGetRefreshRate(best);
        CGDisplayConfigRef config = nullptr;
        CGError err = CGBeginDisplayConfiguration(&config);
        if (err == kCGErrorSuccess)
            err = CGConfigureDisplayWithDisplayMode(config, id, best, nullptr);
        if (err == kCGErrorSuccess)
            err = CGCompleteDisplayConfiguration(config, kCGConfigureForAppOnly);
        else if (config)
            CGCancelDisplayConfiguration(config);
        if (modes) CFRelease(modes);
        if (err != kCGErrorSuccess) {
            log::warning("[native] " + m_Display.name + " refused " + std::to_string(newW) + "x" +
                         std::to_string(newH) + " (CGError " + std::to_string(err) +
                         ") — the display keeps its mode");
            CGDisplayModeRelease(current);
            return false;
        }
        m_SavedMode = current; // released by restoreDisplayMode()
        m_SavedModeDisplay = id;

        const int wasW = m_Display.pixelWidth, wasH = m_Display.pixelHeight;
        // Read back rather than assumed: the size, the refresh and the rest of
        // what the session keys on come from the same list the probe reads.
        for (const platform::MacDisplay& d : platform::listDisplays())
            if (d.displayId == id) m_Display = d;
        m_DisplayMilliHz = m_Display.refreshMilliHz;
        log::info("[native] " + m_Display.name + " put in " + std::to_string(newW) + "x" +
                  std::to_string(newH) + " @ " + std::to_string(static_cast<int>(newHz + 0.5)) +
                  " Hz for the session (" +
                  (exact ? "the client's own size" : "the largest of its shape that fits") +
                  ", was " + std::to_string(wasW) + "x" + std::to_string(wasH) + ")");
        return true;
    }

    /// The mode the session changed, put back. Idempotent.
    void restoreDisplayMode()
    {
        if (!m_SavedMode) return;
        CGDisplayConfigRef config = nullptr;
        CGError err = CGBeginDisplayConfiguration(&config);
        if (err == kCGErrorSuccess)
            err =
                CGConfigureDisplayWithDisplayMode(config, m_SavedModeDisplay, m_SavedMode, nullptr);
        if (err == kCGErrorSuccess)
            err = CGCompleteDisplayConfiguration(config, kCGConfigureForAppOnly);
        else if (config)
            CGCancelDisplayConfiguration(config);
        if (err == kCGErrorSuccess)
            log::info("[native] the display's own mode is back");
        else
            log::warning("[native] the display's own mode could not be put back (CGError " +
                         std::to_string(err) + ")");
        CGDisplayModeRelease(m_SavedMode);
        m_SavedMode = nullptr;
    }

    const SessionInfo& info() const override { return m_Info; }

    void sendInput(const InputEvent& event) override
    {
        std::lock_guard<std::mutex> lock(m_InputMutex);
        if (!m_Input) return;
        if (event.type == InputEvent::Type::MouseMoveAbsolute) {
            InputEvent aimed = event;
            unletterbox(aimed);
            m_Input->inject(aimed);
            return;
        }
        m_Input->inject(event);
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
        // Three ways to show the pointer: the compositor draws it (composite,
        // no size asked), the ENGINE draws it magnified (composite + a size —
        // the small screen), or the client draws its own (not composite).
        const int wanted = cursorFramePx > 0 ? cursorFramePx : 0;
        const bool wasComposite = m_CompositeCursor.load();
        const bool wasSelf = wasComposite && m_CursorFramePx.load() > 0;
        m_CursorFramePx.store(wanted);
        m_CompositeCursor.store(composite);
        const bool isSelf = composite && wanted > 0;
        if (wasComposite != composite)
            log::info(composite ? "[native] cursor: drawn into the picture (gaming)"
                                : "[native] cursor: handed to the client to draw (desktop)");
        if (isSelf && !wasSelf)
            log::info("[native] cursor: a " + std::to_string(wanted) +
                      " px pointer asked — ScreenCaptureKit leaves it out, the engine draws it");
        if (wasComposite != composite || wasSelf != isSelf) {
            // Who draws changed: the capture has to be told, the client that
            // now draws needs a shape, and the last picture the client holds
            // shows the wrong pointer.
            m_CursorModeDirty.store(true);
            m_ResendCursor.store(true);
        } else if (isSelf) {
            // Same mode, another size — a viewer pinch-zooming a still
            // desktop. Nothing else in the loop would ever notice.
            m_PointerDirty.store(true);
        }
    }

    void setFrameFloorFps(int fps) override
    {
        if (fps < 0) fps = 0;
        if (fps > kMaxFloorFps) fps = kMaxFloorFps;
        if (m_FloorFps.exchange(fps) == fps) return;
        log::info("[native] still-screen floor: " +
                  (fps > 0 ? std::to_string(fps) + " fps" : std::string("the engine's own")));
    }

    void requestKeyframe() override { m_ForceKeyframe.store(true); }

    void invalidateReference(uint32_t frameNumber) override
    {
        // VideoToolbox has no reference invalidation: the receiver's lost
        // frame costs a keyframe, which is what SessionInfo promised it.
        (void)frameNumber;
        m_ForceKeyframe.store(true);
    }

    void setTargetBitrate(int kbps) override { m_PendingBitrate.store(kbps); }

    void reportLink(const LinkFeedback& feedback) override
    {
        std::lock_guard<std::mutex> lock(m_LinkMutex);
        if (m_LinkPending) {
            if (feedback.owdRiseMs > m_LinkFeedback.owdRiseMs)
                m_LinkFeedback.owdRiseMs = feedback.owdRiseMs;
            m_LinkFeedback.gaps += feedback.gaps;
            m_LinkFeedback.evictions += feedback.evictions;
            m_LinkFeedback.receivedFps = feedback.receivedFps;
        } else {
            m_LinkFeedback = feedback;
            m_LinkPending = true;
        }
    }

    void setDisplayFormatCallback(DisplayFormatCallback callback) override
    {
        std::lock_guard<std::mutex> lock(m_FormatMutex);
        m_OnDisplayFormat = std::move(callback);
    }

    void setClientRefresh(int milliHz, bool vsync) override
    {
        if (!storeClientRefresh(m_ClientMilliHz, m_ClientVsync, milliHz, vsync)) return;
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
        // Same road as a client screen that changed: the loop re-chooses the
        // gate between two frames.
        m_ClientRefreshDirty.store(true);
    }

    void setSendBacklogProbe(SendBacklogProbe probe) override
    {
        std::lock_guard<std::mutex> lk(m_BacklogProbeMutex);
        m_SendBacklogProbe = std::move(probe);
    }

private:
    /// The relay's send buffer, for the governor (see setSendBacklogProbe):
    /// how long it has stayed backed up, 0 when drained or unknown.
    int64_t sendBacklogMs()
    {
        std::lock_guard<std::mutex> lk(m_BacklogProbeMutex);
        return m_SendBacklogProbe ? m_SendBacklogProbe() : 0;
    }

    bool takeLinkFeedback(LinkFeedback& out)
    {
        std::lock_guard<std::mutex> lock(m_LinkMutex);
        if (!m_LinkPending) return false;
        out = m_LinkFeedback;
        m_LinkPending = false;
        return true;
    }

    /// Take the letterbox back out of a position aimed at the picture.
    ///
    /// The client points at what it is shown — the whole frame, bars included
    /// — and the input sink stretches the whole reference onto the whole
    /// desktop. Between the two sits the fit ScreenCaptureKit applied
    /// (FrameFit): without this the desktop is spread across the bars as well,
    /// and a point lands short of what it was aimed at by up to the width of
    /// one bar, worst at the edges. The bars themselves are no part of the
    /// desktop, so a point on one clamps to the edge it is nearest.
    void unletterbox(InputEvent& event) const
    {
        const platform::FrameFit fit =
            platform::frameFit(m_Display.pixelWidth, m_Display.pixelHeight, event.referenceWidth,
                               event.referenceHeight);
        int x = 0;
        int y = 0;
        if (!platform::unletterboxPoint(fit, event.referenceWidth, event.referenceHeight,
                                        event.positionX, event.positionY, x, y))
            return;
        event.positionX = static_cast<int16_t>(x);
        event.positionY = static_cast<int16_t>(y);
    }

    /// Wake a dark panel, the way a touch of the trackpad would. The assertion
    /// is KEPT, not released at once: measured 05/09/2026, declaring the
    /// activity and releasing it in the same breath left the panel asleep —
    /// ScreenCaptureKit then delivered black frames for ten seconds and lost
    /// the display altogether ("Failed to find any displays", -3815). Called
    /// again on every restart attempt, since that is where a sleeping panel
    /// shows up: a display SCK cannot share is, first of all, a dark one.
    void wakeDisplay()
    {
        if (m_Wake != kIOPMNullAssertionID) IOPMAssertionRelease(m_Wake);
        m_Wake = kIOPMNullAssertionID;
        IOPMAssertionDeclareUserActivity(CFSTR("MoonlightWeb is streaming this display"),
                                         kIOPMUserActiveLocal, &m_Wake);
    }

    /// The engine, not the compositor, draws the pointer: composite mode with
    /// a size asked for it (the small screen). Read on the capture thread.
    bool selfDrawn() const { return m_CompositeCursor.load() && m_CursorFramePx.load() > 0; }

    /// One wake-up of the capture into the click trace (clicktrace=1): what it
    /// brought, ScreenCaptureKit's stamps on it, and the captured display's
    /// last refresh read right after (DisplayClock). Capture thread only.
    void traceCapture(capture::AcquireStatus status, const capture::SckFrame& frame,
                      int64_t startUs, int64_t doneUs)
    {
        // Started on the first wake-up, and again when the capture moved to
        // another display (a restart on the virtual display's successor).
        if (m_DisplayClock.display() != m_Display.displayId)
            m_DisplayClock.start(m_Display.displayId);
        ClickTrace::Row row;
        row.kind = ClickTrace::Kind::Capture;
        row.us = doneUs;
        row.startUs = startUs;
        row.status = acquireStatusName(status);
        if (status == capture::AcquireStatus::Ok) {
            row.presentUs = frame.presentUs;
            row.presentRawUs = frame.presentRawUs;
            row.accumulated = frame.accumulated;
            row.deliveredUs = frame.capturedUs;
        }
        int64_t vsyncUs = 0, periodUs = 0, vsyncs = 0;
        if (m_DisplayClock.read(vsyncUs, periodUs, vsyncs)) {
            row.vblankUs = vsyncUs;
            row.periodUs = periodUs;
            row.composedFrames = vsyncs;
        }
        m_ClickTrace.add(row);
    }

    bool openCapture(int outputWidth, int outputHeight, std::string& error)
    {
        m_Capture = std::make_unique<capture::SckCapture>(
            m_Display.displayId, outputWidth, outputHeight, m_DisplayMilliHz,
            m_CompositeCursor.load() && !selfDrawn(), m_Target.hdr);
        m_Capture->setBenchQueue(m_Config.tuning.sckQueueDepth, m_Config.tuning.sckMinIntervalUs);
        m_Patch = convert::PlanePatch{};
        // Set on every open, restarts included: the sink outlives the capture,
        // and while a display is away the pacer keeps the wire fed with silence.
        if (m_Audio) {
            auto* sink = m_Audio.get();
            m_Capture->setAudioSink(
                [sink](const float* pcm, size_t frames) { sink->push(pcm, frames); });
        }
        return m_Capture->start(error);
    }

    bool buildEncoder(std::string& error)
    {
        m_Encoder.reset();
        m_Encoder = std::make_unique<encode::VtEncoder>();
        return m_Encoder->init(m_Target.codec, m_Capture->width(), m_Capture->height(), m_EncodeFps,
                               m_Config.bitrateKbps, m_Target.hdr, m_Config.tuning, error);
    }

    enum class Restart
    {
        Restarted,
        Stopped,
        Failed,
    };

    Restart restartCapture(std::string& error)
    {
        int failures = 0;
        for (;;) {
            if (!m_Running.load()) return Restart::Stopped;
            // The display may have been renumbered by whatever took it away.
            for (const platform::MacDisplay& d : platform::listDisplays())
                if (d.displayId == m_Display.displayId) m_Display = d;
            m_DisplayMilliHz = m_Display.refreshMilliHz;
            wakeDisplay();
            // The frame keeps its size — ScreenCaptureKit letterboxes a panel of
            // another shape into it — unless the viewer follows the display's
            // shape (SessionConfig::followDisplayShape): then the same height
            // at the panel's new shape, as on Windows and Linux. Followed or
            // not, it is never larger than the panel (frameForDisplay).
            FrameSize frame{m_Info.width, m_Info.height};
            const FrameSize panel{m_Display.pixelWidth, m_Display.pixelHeight};
            const bool overPanel = frame.width > panel.width || frame.height > panel.height;
            if ((m_Config.followDisplayShape || overPanel) && m_Config.width > 0 &&
                m_Config.height > 0)
                frame =
                    frameForDisplay(panel, {m_Config.width, m_Config.height}, policyOf(m_Config));
            if (failures == 0 && (frame.width != m_Info.width || frame.height != m_Info.height))
                log::info("[native] the display is now " + std::to_string(m_Display.pixelWidth) +
                          "x" + std::to_string(m_Display.pixelHeight) +
                          " — the stream follows it: " + std::to_string(m_Info.width) + "x" +
                          std::to_string(m_Info.height) + " -> " + std::to_string(frame.width) +
                          "x" + std::to_string(frame.height));
            if (openCapture(frame.width, frame.height, error)) break;
            failures++;
            if (failures == 1)
                log::info("[native] display is away (reconfiguring, or off), waiting for it: " +
                          error);
            std::this_thread::sleep_for(std::chrono::milliseconds(restartRetryDelayMs(failures)));
        }
        if (failures > 0)
            log::info("[native] display is back after " + std::to_string(failures) + " attempt" +
                      (failures > 1 ? "s" : ""));
        if (!buildEncoder(error)) return Restart::Failed;
        m_Info.width = m_Capture->width();
        m_Info.height = m_Capture->height();
        {
            const capture::DesktopRect rect = m_Capture->desktopRect();
            std::lock_guard<std::mutex> lock(m_InputMutex);
            if (m_Input) m_Input->setDisplayRect(rect.left, rect.top, rect.right, rect.bottom);
        }
        m_ResendCursor.store(true);
        m_ModePixelWidth = m_Display.pixelWidth;
        m_ModePixelHeight = m_Display.pixelHeight;
        reportDisplayFormat();
        return Restart::Restarted;
    }

    /// Whether the display's mode changed since the capture was opened on it.
    ///
    /// ScreenCaptureKit does not say: a resolution change keeps the stream
    /// running and letterboxes the new desktop into the old frame, so nothing
    /// ever reaches the loop as a loss. Asked of CoreGraphics instead — a mode
    /// copy, cheap enough for the loop to ask twice a second — rather than
    /// through a reconfiguration callback, which is delivered on a run loop
    /// this capture thread does not have.
    bool displayModeChanged() const
    {
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(m_Display.displayId);
        if (!mode) return false; // away: the capture reports that itself
        const int width = static_cast<int>(CGDisplayModeGetPixelWidth(mode));
        const int height = static_cast<int>(CGDisplayModeGetPixelHeight(mode));
        CGDisplayModeRelease(mode);
        return width > 0 && height > 0 &&
               (width != m_ModePixelWidth || height != m_ModePixelHeight);
    }

    /// Tell the viewer what the display became — only when something it can
    /// act on moved. See DisplayFormat.
    void reportDisplayFormat()
    {
        const bool displayHdr = m_Display.hdr && m_Target.hdrCapable;
        const DisplayFormat format{
            m_Display.pixelWidth, m_Display.pixelHeight, m_Info.width, m_Info.height, displayHdr,
            m_Info.hdr,           m_Info.hdrCapable};
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

    void run() noexcept
    {
        try {
            runLoop();
            logCadence();
        } catch (const std::exception& e) {
            finish(std::string("the capture loop threw: ") + e.what());
        } catch (...) {
            finish("the capture loop threw an unknown exception");
        }
    }

    void runLoop()
    {
        // The reasoning for every constant here is in WindowsSession::runLoop;
        // the values are the same because the receiver is the same.
        constexpr int kAcquireTimeoutMs = 100;
        constexpr int64_t kIdleFloorUs = 500 * 1000;
        constexpr int64_t kRefineWindowUs = 1000 * 1000;
        constexpr int64_t kRefineDelayUs = 150 * 1000;
        constexpr int kRefineMaxFps = 60;

        const int refineFps =
            (m_Config.fps > 0 && m_Config.fps < kRefineMaxFps) ? m_Config.fps : kRefineMaxFps;
        const int64_t refineIntervalUs = 1000000 / refineFps;
        const int refineTimeoutMs = static_cast<int>(refineIntervalUs / 1000);

        uint32_t frameNumber = 0;
        std::string error;
        int64_t lastSentUs = steadyNowUs();
        int64_t lastRealUs = lastSentUs;
        encode::RefineConvergence refineConv;
        bool refineDone = false;
        int refinePasses = 0;
        int refineHeld = 0;
        size_t refineBytes = 0;
        size_t refineFirstBytes = 0;
        int refineLogged = 0;
        bool haveFrame = false;
        constexpr int64_t kModeCheckUs = 500 * 1000;
        int64_t nextModeCheckUs = steadyNowUs() + kModeCheckUs;

        auto floorIntervalUs = [this]() -> int64_t {
            int fps = m_FloorFps.load(std::memory_order_relaxed);
            if (fps <= 0) return kIdleFloorUs;
            if (m_Config.fps > 0 && fps > m_Config.fps) fps = m_Config.fps;
            const int64_t interval = 1000000 / fps;
            return interval < kIdleFloorUs ? interval : kIdleFloorUs;
        };

        encode::RateGovernor governor;
        governor.start(m_Config.bitrateKbps, steadyNowUs() / 1000,
                       m_Config.tuning.linkGovernor == EncoderTuning::Choice::Off,
                       m_Config.governorFloorPercent);
        // The bench's retrcut= (plan Wi-Fi W2 B): SCTP's retransmissions as a
        // reason to cut. The Windows host's own since 03/10/2026; here it is
        // off unless named, until it is measured on this host.
        governor.setRetransCut(
            m_Config.tuning.retransCutPermille > 0 ? m_Config.tuning.retransCutPermille : 0);
        if (governor.retransCut() > 0)
            log::info("[native] rate governor: also cuts at " +
                      std::to_string(governor.retransCut()) +
                      " SCTP chunks retransmitted in a thousand (bench retrcut=)");
        // The relay's shed frames, the climb near the rate that overflowed and
        // the host's own backlog: the Windows host's rule since 21/10/2026,
        // here too since DebitRemonte3 (14/11/2026) — a 5G link overflows
        // without retransmitting, and only the shed frames say so. ovfgood=0
        // the bench's "before".
        governor.setOverflowLowersGood(overflowLowersGoodByDefault(m_Config.tuning.overflowGood));
        if (governor.overflowLowersGood())
            log::info("[native] rate governor: a frame shed off the send buffer lowers the "
                      "link's last good rate, the climb near the rate that overflowed goes one "
                      "step every few seconds, and a send buffer backed up past a second cuts "
                      "without a report (ovfgood=0 takes all three out)");
        else
            log::info("[native] rate governor: shed frames move nothing (bench ovfgood=0)");
        // The session's account of the link, however the loop ends.
        struct GovernorSummary
        {
            const encode::RateGovernor& g;
            ~GovernorSummary()
            {
                if (g.changes() == 0) return;
                log::info("[native] rate governor this session: " +
                          g.summaryLine(steadyNowUs() / 1000));
            }
        } governorSummary{governor};
        int baseKbps = governor.targetKbps();
        m_LinkKbps = baseKbps;
        bool boosted = false;
        encode::EffectiveCadence effective;
        effective.start(m_EncodeFps, steadyNowUs());
        auto applyBitrate = [&](int kbps) {
            if (kbps <= 0) return;
            if (!m_Encoder->setBitrate(effective.scaledKbps(kbps), error))
                log::warning("[native] bitrate change refused: " + error);
        };
        int cadenceLogged = 0;
        int governorLogged = 0;
        // Every cut is written, the raises thinned out: the first ten, then
        // one move in ten (DebitRemonte2, as on Windows).
        auto applyGovernor = [&](const char* why) {
            const bool cut = governor.targetKbps() < baseKbps;
            baseKbps = governor.targetKbps();
            m_LinkKbps = baseKbps;
            applyBitrate(boosted ? encode::stillBitrateKbps(baseKbps) : baseKbps);
            if (cut || governorLogged < 10 || governor.changes() % 10 == 0) {
                if (!cut) governorLogged++;
                log::info("[native] link: " + std::string(why) + " — encoding at " +
                          std::to_string(baseKbps) + " kbps of the " +
                          std::to_string(governor.settingKbps()) + " set" +
                          (cut ? governor.overflowNote() : std::string()));
            }
        };
        auto closeBurst = [&](const char* how) {
            if (refinePasses == 0) return;
            if (refineLogged < 3) {
                refineLogged++;
                log::info(
                    "[native] still picture refined: " + std::to_string(refineFirstBytes / 1024) +
                    " KB + " + std::to_string(refineBytes / 1024) + " KB over " +
                    std::to_string(refinePasses) + " passes, " + std::to_string(refineHeld) +
                    " held for the link (" + how + ")");
            }
            refinePasses = 0;
        };
        auto resetBurst = [&]() {
            refineConv.reset();
            refineDone = false;
            refinePasses = 0;
            refineHeld = 0;
            refineBytes = 0;
            refineFirstBytes = m_LastEmitBytes;
        };
        auto noteReal = [&]() {
            closeBurst("screen moved");
            lastSentUs = steadyNowUs();
            lastRealUs = lastSentUs;
            resetBurst();
        };
        auto emitPicture = [&](const FrameStamps& stamps) -> bool {
            if (!m_Cadence.admit(stamps.convertedUs)) return true;
            if (!emit(frameNumber, stamps, error)) return false;
            noteReal();
            if (effective.noteFrame(steadyNowUs())) {
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
            }
            return true;
        };

        m_LoopStartUs = steadyNowUs();
        while (m_Running.load()) {
            if (const int kbps = m_PendingBitrate.exchange(0); kbps > 0) {
                governor.setSetting(kbps);
                applyGovernor("ceiling moved");
            }
            if (m_ClientRefreshDirty.exchange(false)) {
                FrameCadence chosen{0};
                std::string line;
                const int fps =
                    chooseCadence(m_ClientMilliHz.load(), m_ClientVsync.load(), chosen, line);
                if (chosen.intervalUs() != m_Cadence.intervalUs() || fps != m_CadenceFps) {
                    m_Cadence = chosen;
                    m_CadenceFps = fps;
                    log::info(line + " (client screen changed mid-session)");
                    if (effective.retarget(fps))
                        applyBitrate(boosted ? encode::stillBitrateKbps(baseKbps) : baseKbps);
                }
            }
            if (m_CursorModeDirty.exchange(false)) {
                std::string cursorError;
                // The compositor draws the pointer only when the picture
                // wants it AND the engine is not drawing a magnified one.
                if (!m_Capture->setShowsCursor(m_CompositeCursor.load() && !selfDrawn(),
                                               cursorError))
                    log::warning("[native] " + cursorError);
                // A still screen produces no frame for the change; the next
                // one carries the pointer (or not), and a keyframe makes the
                // switch clean.
                if (m_CompositeCursor.load()) m_ForceKeyframe.store(true);
            }
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
                // The host's own look at its send buffer (RateGovernor::backlog).
                if (governor.backlog(sendBacklogMs(), nowMs))
                    applyGovernor("send buffer backed up for over a second");
                encode::RateGovernor::BacklogEpisode ep;
                if (governor.takeEndedBacklog(ep))
                    log::info("[native] link: " + governor.drainedLine(ep));
            }

            const int64_t sinceRealUs = steadyNowUs() - lastRealUs;
            const bool refineSoon =
                sinceRealUs < (kRefineDelayUs + kRefineWindowUs) && !refineDone && haveFrame;
            const bool refining = refineSoon && sinceRealUs >= kRefineDelayUs;
            if (!refineSoon && !refineDone) closeBurst("window closed");

            const int64_t idleIntervalUs = floorIntervalUs();
            const int idleTimeoutMs = static_cast<int>(idleIntervalUs / 1000) < kAcquireTimeoutMs
                                          ? static_cast<int>(idleIntervalUs / 1000)
                                          : kAcquireTimeoutMs;
            int timeoutMs = refineSoon ? refineTimeoutMs : idleTimeoutMs;
            // Drawing the pointer ourselves: the capture no longer makes a
            // frame for a pointer move on a still screen, so the loop looks
            // at the pointer at the stream's own rate instead. The same when
            // the CLIENT draws it: its position still has to be watched to be
            // reported (reportCursorPosition), and a still screen would
            // otherwise only be looked at once per idle interval.
            if (haveFrame && (selfDrawn() || !m_CompositeCursor.load()))
                timeoutMs = std::min(timeoutMs, std::max(4, 1000 / std::max(1, m_EncodeFps)));

            // A mode change is not a loss to ScreenCaptureKit (displayModeChanged
            // says why), so it is looked for here and handled as one.
            bool modeChanged = false;
            if (steadyNowUs() >= nextModeCheckUs) {
                nextModeCheckUs = steadyNowUs() + kModeCheckUs;
                modeChanged = displayModeChanged();
                if (modeChanged)
                    log::info("[native] display mode changed — restarting the capture on it");
            }

            capture::SckFrame fresh;
            const int64_t acquireStartUs = steadyNowUs();
            const capture::AcquireStatus status =
                modeChanged ? capture::AcquireStatus::Lost : m_Capture->acquire(timeoutMs, fresh);
            if (m_Config.tuning.clickTrace && !modeChanged)
                traceCapture(status, fresh, acquireStartUs, steadyNowUs());

            if (status != capture::AcquireStatus::Timeout && boosted) {
                boosted = false;
                applyBitrate(baseKbps);
            }

            reportCursor();
            reportCursorPosition();

            if (status == capture::AcquireStatus::Timeout) {
                if (!haveFrame) continue;

                if (m_ForceKeyframe.load(std::memory_order_relaxed)) {
                    if (!emit(frameNumber, resendStamps(steadyNowUs()), error)) return;
                    lastSentUs = steadyNowUs();
                    continue;
                }
                if (selfDrawn() && pointerChanged() &&
                    steadyNowUs() - lastSentUs >= 1000000 / std::max(1, m_EncodeFps)) {
                    // A pointer move IS a new picture when the engine draws
                    // it: the held buffer, re-encoded with the pointer where
                    // it is now (emit() repaints it), at the stream's rate.
                    if (!emit(frameNumber, resendStamps(steadyNowUs()), error)) return;
                    lastSentUs = steadyNowUs();
                    m_PointerFrames++;
                    continue;
                }
                if (steadyNowUs() - lastSentUs < (refining ? refineIntervalUs : idleIntervalUs))
                    continue;
                if (refining && !m_Link.drainedAt(steadyNowUs())) {
                    refineHeld++;
                    continue;
                }
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

            if (status == capture::AcquireStatus::Lost) {
                haveFrame = false;
                m_Held = capture::SckFrame{};
                m_Patch = convert::PlanePatch{};
                closeBurst("display lost");
                switch (restartCapture(error)) {
                case Restart::Restarted: break;
                case Restart::Stopped:
                    finish("the session was stopped while the display was away");
                    return;
                case Restart::Failed: finish("capture could not be restarted: " + error); return;
                }
                m_ForceKeyframe.store(true);
                boosted = false;
                applyBitrate(baseKbps);
                lastRealUs = steadyNowUs();
                resetBurst();
                continue;
            }

            if (status != capture::AcquireStatus::Ok) {
                finish("capture failed");
                return;
            }

            m_PresentsSeen++;
            m_Held = fresh;
            // A new compositor buffer: whatever pointer was drawn into the
            // previous one went with it.
            m_HeldSerial++;
            haveFrame = true;
            const int64_t submittedUs = steadyNowUs();
            // No conversion: the compositor's buffer goes to the encoder as
            // it is, so "converted" is "submitted".
            if (!emitPicture(
                    FrameStamps{fresh.presentUs, fresh.capturedUs, submittedUs, submittedUs}))
                return;
        }
    }

    bool emit(uint32_t& frameNumber, const FrameStamps& stamps, std::string& error)
    {
        // The magnified pointer goes into the buffer right before it is
        // encoded — every time, because the same buffer is re-encoded by the
        // still-screen floor and the pointer may have moved since.
        if (selfDrawn()) paintCursor();
        const bool forceKeyframe = m_ForceKeyframe.exchange(false);
        encode::EncoderOutput encoded;
        if (!m_Encoder->encode(m_Held.pixels, forceKeyframe, stamps.presentUs, encoded, error)) {
            finish("encode failed: " + error);
            return false;
        }
        if (!encoded.data || encoded.size == 0) {
            // The encoder dropped it (see VtEncoder::encode). Nothing to send;
            // a forced keyframe is asked for again rather than lost.
            if (forceKeyframe) m_ForceKeyframe.store(true);
            m_Encoder->releaseOutput();
            return true;
        }
        if (encoded.keyframe && !m_LoggedFirstKeyframe) {
            m_LoggedFirstKeyframe = true;
            log::info("[native] first keyframe: " + std::to_string(encoded.size / 1024) + " KB (" +
                      std::to_string(m_Info.width) + "x" + std::to_string(m_Info.height) + ")");
        }
        m_LastEmitBytes = encoded.size;
        m_LastEmitQp = encoded.avgQp;
        m_Link.sent(steadyNowUs(), encoded.size, m_LinkKbps);

        if (m_Callbacks.onVideo) {
            EncodedFrame out;
            out.data = encoded.data;
            out.size = encoded.size;
            out.keyframe = encoded.keyframe;
            out.frameNumber = frameNumber++;
            out.avgQp = encoded.avgQp;
            out.presentUs = stamps.presentUs;
            out.capturedUs = stamps.capturedUs;
            out.submittedUs = stamps.submittedUs;
            out.convertedUs = stamps.convertedUs;
            out.encodedUs = steadyNowUs();
            m_Callbacks.onVideo(out);
        }
        m_Encoder->releaseOutput();
        return true;
    }

    /// The pointer for a client that draws its own: AppKit's account of the
    /// system cursor, rasterized at the display's pixel scale, named when it
    /// is one of the standard shapes, sent only when it changes.
    void reportCursor()
    {
        if (!m_Callbacks.onCursor || m_CompositeCursor.load()) return;
        const int64_t now = steadyNowUs();
        const bool forced = m_ResendCursor.exchange(false);
        if (!forced && now - m_LastCursorPollUs < kCursorPollUs) return;
        m_LastCursorPollUs = now;

        @autoreleasepool {
            if (m_KnownShapes.empty()) learnStandardShapes();

            NSCursor* cursor = [NSCursor currentSystemCursor];
            CursorShape shape;
            // A pointer AppKit can draw is a pointer: see pointerNow for why
            // macOS is not asked whether it is on screen.
            const bool visible = cursor && rasterize(cursor, backingScale(), shape);
            if (!forced && visible == m_ReportedVisible &&
                (!visible || shape.hash == m_ReportedHash))
                return;
            m_ReportedVisible = visible;
            m_ReportedHash = shape.hash;

            CursorUpdate update;
            update.visible = visible;
            update.width = shape.width;
            update.height = shape.height;
            update.hotspotX = shape.hotspotX;
            update.hotspotY = shape.hotspotY;
            update.kind = "";
            for (const auto& known : m_KnownShapes)
                if (known.first == shape.hash) update.kind = known.second;
            // The bitmap is in display pixels; the frame may be smaller — and
            // the picture inside it smaller again when the two do not share an
            // aspect (FrameFit), so the ratio is the fit's, not the widths'.
            update.scale = platform::frameFit(m_Display.pixelWidth, m_Display.pixelHeight,
                                              m_Info.width, m_Info.height)
                               .scale;
            update.pixels = visible ? shape.pixels.data() : nullptr;
            m_Callbacks.onCursor(update);
        }
    }

    /// Where the pointer is, for a client that draws it without a pointer device
    /// of its own to know. Throttled by the gate — see CursorUpdate::positionOnly
    /// and the Windows session, which this mirrors. The position is the same
    /// one the magnified pointer is painted at: frame pixels, through the fit.
    void reportCursorPosition()
    {
        if (!m_Callbacks.onCursor) return;
        if (m_CompositeCursor.load()) {
            m_PositionGate.reset();
            return;
        }
        const PointerNow p = pointerNow();
        if (!m_PositionGate.due(p.visible, static_cast<int>(p.fx), static_cast<int>(p.fy),
                                steadyNowUs()))
            return;
        CursorUpdate update;
        update.positionOnly = true;
        update.visible = p.visible;
        update.x = p.fx;
        update.y = p.fy;
        m_Callbacks.onCursor(update);
    }

    double backingScale() const
    {
        const int points = m_Display.right - m_Display.left;
        if (points <= 0 || m_Display.pixelWidth <= 0) return 1.0;
        return static_cast<double>(m_Display.pixelWidth) / points;
    }

    // ── The magnified pointer, drawn by the engine ──────────────────────────

    struct PointerNow
    {
        bool visible = false;
        /// Position in FRAME pixels — the pointer's hotspot.
        float fx = 0.0f;
        float fy = 0.0f;
    };

    /// Where the pointer is, in the frame, if it is on this display.
    PointerNow pointerNow() const
    {
        PointerNow p;
        CGEventRef event = CGEventCreate(nullptr);
        if (!event) return p;
        const CGPoint at = CGEventGetLocation(event);
        CFRelease(event);
        const int w = m_Display.right - m_Display.left;
        const int h = m_Display.bottom - m_Display.top;
        if (w <= 0 || h <= 0 || m_Info.width <= 0 || m_Info.height <= 0) return p;
        const double x = at.x - m_Display.left;
        const double y = at.y - m_Display.top;
        if (x < 0 || y < 0 || x >= w || y >= h) return p;
        // On this display, so: shown. macOS offers no working way to ask
        // whether the pointer is hidden. CGCursorIsVisible() was that way, and
        // it was measured dead on macOS 15.6 (10/09/2026): it answers false
        // forever, in the app's own signed bundle as in a plain binary, in the
        // GUI session as over SSH, with the pointer moving and the display
        // awake — and it keeps answering false while the asking process hides
        // and unhides the pointer itself, which is what proves the getter and
        // not the pointer. Its deprecation note says as much ("no longer
        // supported"), and there is no replacement a process other than the
        // cursor's owner can call.
        //
        // Trusting it cost the pointer everywhere on a macOS host: the phone's
        // magnified pointer was never painted, and the desktop client was told
        // the pointer was gone and drew nothing over a picture that had none
        // either — a viewer with no pointer at all, on both paths.
        //
        // So the pointer counts as visible whenever AppKit hands us a shape.
        // What that gives up: an application that hides the pointer for real —
        // a fullscreen game, a video player after a few still seconds — still
        // gets one drawn for the viewer. A pointer that should not be there is
        // a smaller loss than no pointer at all, and gaming mode is unaffected
        // (the compositor draws that one, and it disappears with the real one).
        p.visible = true;
        // Through the letterbox, not across the whole frame: see FrameFit. A
        // pointer at the right edge of a 1.54:1 panel streamed at 16:9 belongs
        // at the right edge of the PICTURE, not 129 pixels further out on the
        // black bar — where nothing ever repaints over it.
        const platform::FrameFit fit = platform::frameFit(w, h, m_Info.width, m_Info.height);
        p.fx = fit.offsetX + static_cast<float>(x) * fit.scale;
        p.fy = fit.offsetY + static_cast<float>(y) * fit.scale;
        return p;
    }

    /// Has the pointer moved, appeared, vanished or changed shape since the
    /// last paint? Called on a still screen, at the stream's rate; the shape
    /// is rasterised at most every kCursorPollUs, the position every time.
    bool pointerChanged()
    {
        if (m_PointerDirty.exchange(false)) return true;
        const PointerNow p = pointerNow();
        if (p.visible != m_DrawnVisible) return true;
        if (p.visible && (std::fabs(p.fx - m_DrawnX) >= 0.5f || std::fabs(p.fy - m_DrawnY) >= 0.5f))
            return true;
        if (!p.visible) return false;
        const int64_t now = steadyNowUs();
        if (now - m_LastShapePollUs < kCursorPollUs) return false;
        m_LastShapePollUs = now;
        @autoreleasepool {
            CursorShape shape;
            if (!rasterize([NSCursor currentSystemCursor], backingScale(), shape)) return false;
            return shape.hash != m_DrawnHash;
        }
    }

    /// Draw the system pointer, magnified for the client's screen, into the
    /// held compositor buffer — after undoing the previous drawing when this
    /// buffer is the one it was made in.
    void paintCursor()
    {
        CVPixelBufferRef buffer = m_Held.pixels;
        if (!buffer) return;
        @autoreleasepool {
            CursorShape shape;
            const bool haveShape = rasterize([NSCursor currentSystemCursor], backingScale(), shape);
            if (haveShape && shape.hash != m_PreparedHash) {
                m_Prepared = convert::prepareCursor(shape.pixels.data(), shape.width, shape.height,
                                                    m_Info.hdr ? convert::BlendTarget::P010Bt2020Pq
                                                               : convert::BlendTarget::Nv12Bt709);
                m_PreparedHash = shape.hash;
                m_PreparedHotspotX = shape.hotspotX;
                m_PreparedHotspotY = shape.hotspotY;
            }
            const PointerNow p = pointerNow();

            if (CVPixelBufferLockBaseAddress(buffer, 0) != kCVReturnSuccess) {
                if (!m_LockFailedLogged) {
                    m_LockFailedLogged = true;
                    log::warning("[native] cursor: the compositor's buffer cannot be written — "
                                 "the pointer stays at its own size");
                }
                return;
            }
            convert::PlaneViews planes;
            planes.y = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
            planes.yStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
            planes.uv = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
            planes.uvStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
            planes.width = static_cast<int>(CVPixelBufferGetWidthOfPlane(buffer, 0)) & ~1;
            planes.height = static_cast<int>(CVPixelBufferGetHeightOfPlane(buffer, 0)) & ~1;
            planes.tenBit = m_Info.hdr;
            if (planes.y && planes.uv) {
                // Undo: the buffer is the one the last pointer was drawn in.
                if (m_Patch.valid() && m_PatchSerial == m_HeldSerial)
                    convert::restorePatch(planes, m_Patch);
                m_Patch = convert::PlanePatch{};

                if (p.visible && haveShape && !m_Prepared.empty()) {
                    // The shape is in display pixels; the frame may be
                    // smaller. Its natural size in the frame is the floor,
                    // the client's request the target, sized on the longer
                    // side of the ink (CursorState::inkWidth says why).
                    const float frameScale =
                        platform::frameFit(m_Display.pixelWidth, m_Display.pixelHeight,
                                           planes.width, planes.height)
                            .scale;
                    const int ink = std::max(m_Prepared.inkWidth, m_Prepared.inkHeight);
                    const int wanted = m_CursorFramePx.load();
                    float scale = frameScale;
                    if (wanted > 0 && ink > 0)
                        scale = std::max(frameScale, static_cast<float>(wanted) / ink);
                    scale = std::min(scale, frameScale * kMaxCursorMagnify);

                    convert::CursorPlacement place;
                    place.scale = scale;
                    place.x = p.fx - m_PreparedHotspotX * scale;
                    place.y = p.fy - m_PreparedHotspotY * scale;
                    const convert::BlendRect rect =
                        convert::blendFootprint(m_Prepared, place, planes);
                    convert::savePatch(planes, rect, m_Patch);
                    convert::blendCursor(m_Prepared, place, planes);
                    m_PatchSerial = m_HeldSerial;
                    if (!m_PaintLogged) {
                        m_PaintLogged = true;
                        // Read-back, once, in debug: the raster pixel near the
                        // hotspot, what it was prepared into, and what the
                        // planes hold where it was drawn — the three numbers
                        // that tell a colour bug from a layout bug (06/09/2026:
                        // a "green pointer" report was the bench Mac's own
                        // Accessibility pointer colour, BGRA 0 255 0 255 at
                        // the source, blended faithfully).
                        {
                            const int sx = std::min(shape.width - 1, m_PreparedHotspotX + 2);
                            const int sy = std::min(shape.height - 1, m_PreparedHotspotY + 4);
                            const size_t si = static_cast<size_t>(sy) * shape.width + sx;
                            const uint8_t* sp = shape.pixels.data() + si * 4;
                            const int dx = static_cast<int>(place.x + sx * scale + scale / 2);
                            const int dy = static_cast<int>(place.y + sy * scale + scale / 2);
                            int y = -1, cb = -1, cr = -1;
                            if (dx >= 0 && dy >= 0 && dx < planes.width && dy < planes.height) {
                                const uint8_t* yl =
                                    planes.y + static_cast<size_t>(dy) * planes.yStride;
                                const uint8_t* ul =
                                    planes.uv + static_cast<size_t>(dy / 2) * planes.uvStride;
                                if (planes.tenBit) {
                                    uint16_t w;
                                    std::memcpy(&w, yl + dx * 2, 2);
                                    y = w >> 6;
                                    std::memcpy(&w, ul + (dx & ~1) * 2, 2);
                                    cb = w >> 6;
                                    std::memcpy(&w, ul + ((dx & ~1) + 1) * 2, 2);
                                    cr = w >> 6;
                                } else {
                                    y = yl[dx];
                                    cb = ul[dx & ~1];
                                    cr = ul[(dx & ~1) + 1];
                                }
                            }
                            char probe[240];
                            std::snprintf(
                                probe, sizeof(probe),
                                "[native] cursor probe: raster(%d,%d) BGRA %d %d %d %d -> "
                                "prepared luma %.1f cb %.1f cr %.1f a %.2f -> frame(%d,%d) "
                                "Y %d Cb %d Cr %d (strides %zu/%zu, %s)",
                                sx, sy, sp[0], sp[1], sp[2], sp[3], m_Prepared.luma[si],
                                m_Prepared.cb[si], m_Prepared.cr[si], m_Prepared.alpha[si], dx, dy,
                                y, cb, cr, planes.yStride, planes.uvStride,
                                planes.tenBit ? "10-bit" : "8-bit");
                            log::debug(probe);
                        }
                        char line[160];
                        std::snprintf(line, sizeof(line),
                                      "[native] cursor: drawing the pointer at x%.2f of the frame "
                                      "(%d px asked, shape %dx%d, ink %dx%d)",
                                      scale / (frameScale > 0.0f ? frameScale : 1.0f), wanted,
                                      m_Prepared.width, m_Prepared.height, m_Prepared.inkWidth,
                                      m_Prepared.inkHeight);
                        log::info(line);
                    }
                }
            }
            CVPixelBufferUnlockBaseAddress(buffer, 0);
            m_DrawnVisible = p.visible;
            m_DrawnX = p.fx;
            m_DrawnY = p.fy;
            m_DrawnHash = haveShape ? shape.hash : 0;
        }
    }

    /// The standard pointers, hashed once, so a shape can be named for the
    /// client (CursorUpdate::kind) instead of only pictured.
    void learnStandardShapes()
    {
        struct Named
        {
            NSCursor* cursor;
            const char* kind;
        };
        const Named standard[] = {
            {[NSCursor arrowCursor], "default"},
            {[NSCursor IBeamCursor], "text"},
            {[NSCursor pointingHandCursor], "pointer"},
            {[NSCursor crosshairCursor], "crosshair"},
            {[NSCursor resizeLeftRightCursor], "ew-resize"},
            {[NSCursor resizeUpDownCursor], "ns-resize"},
            {[NSCursor openHandCursor], "grab"},
            {[NSCursor closedHandCursor], "grabbing"},
            {[NSCursor operationNotAllowedCursor], "not-allowed"},
            {[NSCursor dragCopyCursor], "copy"},
            {[NSCursor dragLinkCursor], "alias"},
            {[NSCursor contextualMenuCursor], "context-menu"},
            {[NSCursor IBeamCursorForVerticalLayout], "vertical-text"},
        };
        const double scale = backingScale();
        for (const Named& n : standard) {
            CursorShape shape;
            if (rasterize(n.cursor, scale, shape)) m_KnownShapes.emplace_back(shape.hash, n.kind);
        }
    }

    int chooseCadence(int clientMilliHz, bool clientVsync, FrameCadence& cadence,
                      std::string& line) const
    {
        const int displayHz = (m_DisplayMilliHz + 500) / 1000;
        int fps = m_Config.fps > 0 ? m_Config.fps : displayHz;
        if (fps <= 0) fps = 60;
        // A client whose decoder cannot keep up asks for fewer frames than the
        // viewer set (setClientFpsCap), and a rate chosen FOR the viewer comes
        // with a ceiling of its own (SessionConfig::maxFps — the rate the
        // browser's pixel budget was sized at). Both only ever lower the rate;
        // the smaller of the two is the one the cadence answers to.
        const int asked = m_ClientFpsCap.load();
        const int cap = m_Config.maxFps > 0 && (asked <= 0 || m_Config.maxFps < asked)
                            ? m_Config.maxFps
                            : asked;
        const bool capped = cap > 0 && cap < fps;
        if (capped) fps = cap;
        const int wanted = capped ? cap : m_Config.fps;

        AlignedCadence aligned;
        if (wanted > 0 && clientVsync)
            aligned = alignCadence(wanted, clientMilliHz, displayHz, cap);

        if (aligned.aligned) {
            fps = aligned.fps;
            cadence = fps < displayHz ? FrameCadence::fromIntervalNs(aligned.intervalNs, displayHz)
                                      : FrameCadence(0, displayHz);
            line = "[native] cadence: " + std::to_string(fps) + " fps stream for a " +
                   hzString(clientMilliHz) + " Hz client presenting on vsync (" +
                   std::to_string(m_Config.fps) + " set) on a " + hzString(m_DisplayMilliHz) +
                   " Hz display" +
                   (cadence.enabled() ? " — the first present of each interval is encoded, at once"
                                      : " — every present is encoded");
            if (capped)
                line += " (no more than " + std::to_string(cap) + " fps: " +
                        (cap == m_Config.maxFps ? "the rate chosen for this client"
                                                : "what its decoder keeps up with") +
                        ")";
            return fps;
        }
        cadence = FrameCadence(fps < displayHz ? fps : 0, displayHz);
        line = "[native] cadence: " + std::to_string(fps) + " fps stream on a " +
               hzString(m_DisplayMilliHz) + " Hz display" +
               (cadence.enabled() ? " — the first present of each interval is encoded, at once"
                                  : " — every present is encoded");
        if (capped)
            line += " (no more than " + std::to_string(cap) + " fps: " +
                    (cap == m_Config.maxFps ? "the rate chosen for this client"
                                            : "what its decoder keeps up with") +
                    ")";
        return fps;
    }

    void logCadence()
    {
        if (m_PresentsSeen == 0) return;
        const double seconds = (steadyNowUs() - m_LoopStartUs) / 1e6;
        char span[32];
        std::snprintf(span, sizeof(span), "%.1f", seconds);
        std::string line = "[native] cadence: " + hzString(m_DisplayMilliHz) + " Hz display, " +
                           std::to_string(m_CadenceFps) + " fps stream — " +
                           std::to_string(m_PresentsSeen) + " presents in " + span + " s";
        if (m_Cadence.enabled())
            line += ", " + std::to_string(m_Cadence.skipped()) + " not carried";
        else
            line += ", every one carried";
        if (m_PointerFrames > 0)
            line += ", " + std::to_string(m_PointerFrames) + " pointer-only re-encodes";
        log::info(line);
    }

    void finish(const std::string& reason) noexcept
    {
        m_Running.store(false);
        try {
            log::warning("[native] session ended: " + reason);
            if (m_Callbacks.onEnded) m_Callbacks.onEnded(reason);
        } catch (...) {}
    }

    SessionConfig m_Config;
    ResolvedTarget m_Target;
    SessionCallbacks m_Callbacks;
    SessionInfo m_Info;

    platform::MacDisplay m_Display;
    IOPMAssertionID m_KeepAwake = kIOPMNullAssertionID;
    /// The mode "Match my screen" replaced, to put back at stop().
    CGDisplayModeRef m_SavedMode = nullptr;
    CGDirectDisplayID m_SavedModeDisplay = 0;
    IOPMAssertionID m_Wake = kIOPMNullAssertionID;
    /// The App Nap opt-out, held from start() to stop().
    id<NSObject> m_Activity = nil;

    int m_DisplayMilliHz = 0;
    int m_EncodeFps = 0;
    FrameCadence m_Cadence{0};
    int m_CadenceFps = 0;
    std::atomic<int> m_ClientMilliHz{0};
    std::atomic<bool> m_ClientVsync{false};
    std::atomic<bool> m_ClientRefreshDirty{false};
    /// Frames per second the client asked not to exceed, 0 for none — see
    /// setClientFpsCap.
    std::atomic<int> m_ClientFpsCap{0};

    std::unique_ptr<capture::SckCapture> m_Capture;
    std::unique_ptr<audio::PacedOpusSink> m_Audio;
    /// The speakers, silenced for the length of the session when the client
    /// asked for it. Its destructor releases too, so a session torn down by an
    /// unusual path cannot leave the room mute.
    audio::HostMute m_HostMute;
    std::unique_ptr<encode::VtEncoder> m_Encoder;
    /// The frame the capture last handed out — the picture the still-screen
    /// floor and the refinement burst re-encode.
    capture::SckFrame m_Held;

    std::mutex m_InputMutex;
    std::unique_ptr<input::CgInput> m_Input;

    /// The bench's click trace (EncoderTuning::clickTrace), and the captured
    /// display's refresh it reads. Capture thread, and the input's.
    ClickTrace m_ClickTrace;
    DisplayClock m_DisplayClock;

    /// The mode the capture was last opened on, in pixels — see
    /// displayModeChanged. Capture thread only, after start().
    int m_ModePixelWidth = 0;
    int m_ModePixelHeight = 0;

    /// See setDisplayFormatCallback, and the last format said. Guarded by
    /// m_FormatMutex: set on the consumer's thread, read on the capture thread.
    std::mutex m_FormatMutex;
    DisplayFormatCallback m_OnDisplayFormat;
    DisplayFormat m_LastFormat;

    std::thread m_Thread;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_ForceKeyframe{true};
    std::atomic<bool> m_CompositeCursor{true};
    std::atomic<bool> m_CursorModeDirty{false};
    /// How wide the client wants the pointer, in frame pixels; 0 = its own
    /// size, which the compositor then draws.
    std::atomic<int> m_CursorFramePx{0};
    std::atomic<bool> m_PointerDirty{false};
    std::atomic<int> m_FloorFps{0};
    std::atomic<bool> m_ResendCursor{false};
    std::atomic<int> m_PendingBitrate{0};

    std::mutex m_LinkMutex;
    LinkFeedback m_LinkFeedback;
    bool m_LinkPending = false;
    /// The relay's send buffer for the governor — see setSendBacklogProbe.
    std::mutex m_BacklogProbeMutex;
    SendBacklogProbe m_SendBacklogProbe;
    encode::LinkOccupancy m_Link;
    int m_LinkKbps = 0;

    bool m_LoggedFirstKeyframe = false;
    size_t m_LastEmitBytes = 0;
    int m_LastEmitQp = -1;
    int64_t m_PresentsSeen = 0;
    int64_t m_LoopStartUs = 0;

    int64_t m_LastCursorPollUs = 0;
    uint64_t m_ReportedHash = 0;
    bool m_ReportedVisible = false;
    /// When the pointer's position last went out to a self-drawing client.
    CursorPositionGate m_PositionGate;
    std::vector<std::pair<uint64_t, const char*>> m_KnownShapes;

    // The engine-drawn pointer (capture thread only).
    convert::PreparedCursor m_Prepared;
    uint64_t m_PreparedHash = 0;
    int m_PreparedHotspotX = 0;
    int m_PreparedHotspotY = 0;
    /// What the last blend overwrote, and which held buffer it was.
    convert::PlanePatch m_Patch;
    uint64_t m_PatchSerial = 0;
    uint64_t m_HeldSerial = 0;
    /// The pointer as last painted, for pointerChanged().
    bool m_DrawnVisible = false;
    float m_DrawnX = -1.0f;
    float m_DrawnY = -1.0f;
    uint64_t m_DrawnHash = 0;
    int64_t m_LastShapePollUs = 0;
    int64_t m_PointerFrames = 0;
    bool m_PaintLogged = false;
    bool m_LockFailedLogged = false;
};

} // namespace

namespace detail {

std::unique_ptr<Session> createPlatformSession(const SessionConfig& config,
                                               const ResolvedTarget& target,
                                               const SessionCallbacks& callbacks,
                                               std::string& error)
{
    if (target.encoder != EncoderApi::VideoToolbox) {
        error = std::string("no macOS encoder for ") + toString(target.encoder);
        return nullptr;
    }
    return std::make_unique<MacSession>(config, target, callbacks);
}

} // namespace detail
} // namespace mw::native
