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

#include "MacDisplays.h"

#include "../../audio/PacedOpusSink.h"
#include "../../capture/macos/SckCapture.h"
#include "../../core/CadenceAlign.h"
#include "../../core/FrameCadence.h"
#include "../../core/Log.h"
#include "../../core/RestartBackoff.h"
#include "../../core/Session.h"
#include "../../encode/RateControl.h"
#include "../../encode/RateGovernor.h"
#include "../../encode/macos/VtEncoder.h"
#include "../../input/macos/CgInput.h"

#import <AppKit/AppKit.h>

#include <IOKit/pwr_mgt/IOPMLib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
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
//    directly (SckCapture.h), so a frame goes capture → encoder in place;
//  - the pointer in the picture is the compositor's (showsCursor); the shape
//    for a client that draws its own comes from AppKit's account of the
//    system cursor, polled, not from the capture;
//  - no pointer-only path: with the pointer in the picture a move IS a frame;
//  - the sound arrives as a second output of the SAME ScreenCaptureKit stream
//    (macOS has no loopback device to open), so it is set up on the capture
//    and paced into Opus by the platform-neutral sink;
//  - no reference invalidation, no intra-refresh (VideoToolbox has neither),
//    no virtual gamepad, no HDR.
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

std::string hzString(int milliHz)
{
    return std::to_string((milliHz + 500) / 1000);
}

constexpr int kMaxFloorFps = 480;
/// How often the system pointer's shape is looked at, at most.
constexpr int64_t kCursorPollUs = 50 * 1000;

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
            auto sink = std::make_unique<audio::PacedOpusSink>(m_Callbacks.onAudio);
            std::string audioError;
            if (sink->start("ScreenCaptureKit, 48 kHz stereo", audioError))
                m_Audio = std::move(sink);
            else
                log::warning("[native] audio unavailable, streaming silent: " + audioError);
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
        m_Info.hdr = false;
        m_Info.yuv444 = false;
        m_Info.intraRefresh = false;
        m_Info.intraRefreshFrames = 0;
        m_Info.referenceInvalidation = false;
        // The compositor writes NV12 the encoder reads in place: one copy
        // remains, the bitstream leaving VRAM.
        m_Info.copiesPerFrame = 1;
        m_Info.crossGpuCopy = false;
        m_Info.audio = static_cast<bool>(m_Audio);

        log::info(std::string("[native] session: ") + m_Display.name + " " +
                  std::to_string(m_Info.width) + "x" + std::to_string(m_Info.height) + "@" +
                  std::to_string(m_EncodeFps) + " " + toString(m_Info.codec) +
                  " via VideoToolbox on " + m_Info.gpuName +
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
        if (m_KeepAwake != kIOPMNullAssertionID) {
            IOPMAssertionRelease(m_KeepAwake);
            m_KeepAwake = kIOPMNullAssertionID;
        }
        if (m_Wake != kIOPMNullAssertionID) {
            IOPMAssertionRelease(m_Wake);
            m_Wake = kIOPMNullAssertionID;
        }
        if (!wasRunning && !m_Encoder && !m_Capture && !m_Audio) return;
        m_Encoder.reset();
        // The capture goes first: stopping it is what guarantees no audio
        // callback is still in flight when the sink it points at is freed.
        m_Capture.reset();
        m_Audio.reset();
    }

    const SessionInfo& info() const override { return m_Info; }

    void sendInput(const InputEvent& event) override
    {
        std::lock_guard<std::mutex> lock(m_InputMutex);
        if (m_Input) m_Input->inject(event);
    }

    void setCompositeCursor(bool composite, int cursorFramePx) override
    {
        // The compositor draws the pointer at its own size; a magnified
        // pointer for a phone (cursorFramePx) has no route here and is noted
        // once rather than silently dropped.
        if (cursorFramePx > 0 && !m_MagnifyNoted.exchange(true))
            log::info("[native] cursor: a magnified pointer was asked for; ScreenCaptureKit "
                      "draws it at its own size");
        if (m_CompositeCursor.exchange(composite) == composite) return;
        log::info(composite ? "[native] cursor: drawn into the picture (gaming)"
                            : "[native] cursor: handed to the client to draw (desktop)");
        m_CursorModeDirty.store(true);
        m_ResendCursor.store(true);
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

    void setClientRefresh(int milliHz, bool vsync) override
    {
        if (milliHz < 1000) milliHz = 0;
        if (milliHz > 1000000) milliHz = 1000000;
        const bool same =
            m_ClientMilliHz.exchange(milliHz) == milliHz && m_ClientVsync.exchange(vsync) == vsync;
        if (same) return;
        m_ClientRefreshDirty.store(true);
    }

private:
    bool takeLinkFeedback(LinkFeedback& out)
    {
        std::lock_guard<std::mutex> lock(m_LinkMutex);
        if (!m_LinkPending) return false;
        out = m_LinkFeedback;
        m_LinkPending = false;
        return true;
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

    bool openCapture(int outputWidth, int outputHeight, std::string& error)
    {
        m_Capture =
            std::make_unique<capture::SckCapture>(m_Display.displayId, outputWidth, outputHeight,
                                                  m_DisplayMilliHz, m_CompositeCursor.load());
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
                               m_Config.bitrateKbps, m_Config.tuning, error);
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
            if (openCapture(m_Info.width, m_Info.height, error)) break;
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
        {
            const capture::DesktopRect rect = m_Capture->desktopRect();
            std::lock_guard<std::mutex> lock(m_InputMutex);
            if (m_Input) m_Input->setDisplayRect(rect.left, rect.top, rect.right, rect.bottom);
        }
        m_ResendCursor.store(true);
        return Restart::Restarted;
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

        auto floorIntervalUs = [this]() -> int64_t {
            int fps = m_FloorFps.load(std::memory_order_relaxed);
            if (fps <= 0) return kIdleFloorUs;
            if (m_Config.fps > 0 && fps > m_Config.fps) fps = m_Config.fps;
            const int64_t interval = 1000000 / fps;
            return interval < kIdleFloorUs ? interval : kIdleFloorUs;
        };

        encode::RateGovernor governor;
        governor.start(m_Config.bitrateKbps, steadyNowUs() / 1000);
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
                if (!m_Capture->setShowsCursor(m_CompositeCursor.load(), cursorError))
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
                        applyGovernor(fb.gaps > 0 || fb.evictions > 0 ? "frames lost"
                                      : fb.owdRiseMs >= encode::RateGovernor::kOveruseMs
                                          ? "delay rising"
                                          : "quiet, raising");
                } else if (governor.tick(nowMs)) {
                    applyGovernor("no report from the receiver");
                }
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
            const int timeoutMs = refineSoon ? refineTimeoutMs : idleTimeoutMs;

            capture::SckFrame fresh;
            const capture::AcquireStatus status = m_Capture->acquire(timeoutMs, fresh);

            if (status != capture::AcquireStatus::Timeout && boosted) {
                boosted = false;
                applyBitrate(baseKbps);
            }

            reportCursor();

            if (status == capture::AcquireStatus::Timeout) {
                if (!haveFrame) continue;

                if (m_ForceKeyframe.load(std::memory_order_relaxed)) {
                    if (!emit(frameNumber, resendStamps(steadyNowUs()), error)) return;
                    lastSentUs = steadyNowUs();
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
            // CGCursorIsVisible is deprecated without a replacement that a
            // process other than the cursor's owner can call; it still answers.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            const bool visible =
                cursor && rasterize(cursor, backingScale(), shape) && CGCursorIsVisible();
#pragma clang diagnostic pop
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
            // The bitmap is in display pixels; the frame may be smaller.
            update.scale =
                (m_Display.pixelWidth > 0 && m_Info.width > 0)
                    ? static_cast<float>(m_Info.width) / static_cast<float>(m_Display.pixelWidth)
                    : 1.0f;
            update.pixels = visible ? shape.pixels.data() : nullptr;
            m_Callbacks.onCursor(update);
        }
    }

    double backingScale() const
    {
        const int points = m_Display.right - m_Display.left;
        if (points <= 0 || m_Display.pixelWidth <= 0) return 1.0;
        return static_cast<double>(m_Display.pixelWidth) / points;
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

        AlignedCadence aligned;
        if (m_Config.fps > 0 && clientVsync)
            aligned = alignCadence(m_Config.fps, clientMilliHz, displayHz);

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
            return fps;
        }
        cadence = FrameCadence(fps < displayHz ? fps : 0, displayHz);
        line = "[native] cadence: " + std::to_string(fps) + " fps stream on a " +
               hzString(m_DisplayMilliHz) + " Hz display" +
               (cadence.enabled() ? " — the first present of each interval is encoded, at once"
                                  : " — every present is encoded");
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
    IOPMAssertionID m_Wake = kIOPMNullAssertionID;

    int m_DisplayMilliHz = 0;
    int m_EncodeFps = 0;
    FrameCadence m_Cadence{0};
    int m_CadenceFps = 0;
    std::atomic<int> m_ClientMilliHz{0};
    std::atomic<bool> m_ClientVsync{false};
    std::atomic<bool> m_ClientRefreshDirty{false};

    std::unique_ptr<capture::SckCapture> m_Capture;
    std::unique_ptr<audio::PacedOpusSink> m_Audio;
    std::unique_ptr<encode::VtEncoder> m_Encoder;
    /// The frame the capture last handed out — the picture the still-screen
    /// floor and the refinement burst re-encode.
    capture::SckFrame m_Held;

    std::mutex m_InputMutex;
    std::unique_ptr<input::CgInput> m_Input;

    std::thread m_Thread;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_ForceKeyframe{true};
    std::atomic<bool> m_CompositeCursor{true};
    std::atomic<bool> m_CursorModeDirty{false};
    std::atomic<bool> m_MagnifyNoted{false};
    std::atomic<int> m_FloorFps{0};
    std::atomic<bool> m_ResendCursor{false};
    std::atomic<int> m_PendingBitrate{0};

    std::mutex m_LinkMutex;
    LinkFeedback m_LinkFeedback;
    bool m_LinkPending = false;
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
    std::vector<std::pair<uint64_t, const char*>> m_KnownShapes;
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
