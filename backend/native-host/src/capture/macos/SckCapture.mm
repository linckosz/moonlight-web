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

#include "SckCapture.h"

#include "../../audio/AudioInterleave.h"
#include "../../core/Log.h"

#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <mach/mach_time.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace mw::native::capture {
namespace {

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// mach_absolute_time ticks → microseconds. The timebase is 1:1 on Apple
/// Silicon and 125:3 on Intel; asked once, either way.
int64_t machToUs(uint64_t ticks)
{
    static mach_timebase_info_data_t info = [] {
        mach_timebase_info_data_t i;
        mach_timebase_info(&i);
        return i;
    }();
    return static_cast<int64_t>(ticks * info.numer / info.denom / 1000);
}

std::string describe(NSError* error)
{
    if (!error) return "unknown error";
    return std::string([[error localizedDescription] UTF8String] ?: "error") + " (" +
           std::to_string(error.code) + ")";
}

} // namespace

/// Everything the Objective-C side needs to reach, kept out of the header.
struct SckCapture::Impl
{
    SCStream* stream = nil;
    SCStreamConfiguration* config = nil;
    id sink = nil;
    dispatch_queue_t queue = nullptr;
    /// Audio has a queue of its own so a frame being parked never delays a
    /// packet — the video queue can sit under the capture's mutex.
    dispatch_queue_t audioQueue = nullptr;

    /// Set before start() when the session wants sound. Read only on the audio
    /// queue, and cleared in stop() after the stream has been stopped.
    SckCapture::AudioSampleCallback onAudio;
    /// The interleaving buffer, owned by the audio queue alone (serial).
    std::vector<float> audioScratch;
    /// The AudioBufferList CoreMedia fills, sized by CoreMedia itself and kept
    /// between buffers. Audio queue only.
    std::vector<uint8_t> audioListStorage;
    uint64_t audioFrames = 0;
    uint64_t audioCallbacks = 0;

    std::mutex mutex;
    std::condition_variable cv;
    /// The newest frame not yet taken. A newer one REPLACES it — no queue.
    CVPixelBufferRef pending = nullptr;
    int64_t pendingPresentUs = 0;
    int64_t pendingCapturedUs = 0;
    /// The frame acquire() last handed out, kept alive for the caller.
    CVPixelBufferRef held = nullptr;
    bool lost = false;
    std::string lostReason;

    uint64_t delivered = 0;
    uint64_t idle = 0;
    uint64_t replaced = 0;
    bool started = false;

    void clearPending()
    {
        if (pending) CVPixelBufferRelease(pending);
        pending = nullptr;
    }
};

} // namespace mw::native::capture

// The stream's output and delegate, in one object. Holds a raw pointer to the
// Impl: the capture stops the stream (and waits) before the Impl goes away.
@interface MWSckSink : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) mw::native::capture::SckCapture::Impl* owner;
@end

@implementation MWSckSink

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (!self.owner) return;
    if (@available(macOS 13.0, *)) {
        if (type == SCStreamOutputTypeAudio) {
            [self handleAudio:sampleBuffer];
            return;
        }
    }
    if (type != SCStreamOutputTypeScreen) return;
    auto* d = self.owner;

    // The frame's status rides in the sample attachments. Idle means "nothing
    // changed since the last one" and comes with no picture worth encoding;
    // Complete (and Started, the very first) carry the compositor's output.
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    int64_t displayUs = 0;
    if (attachments && CFArrayGetCount(attachments) > 0) {
        NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
        NSNumber* status = info[SCStreamFrameInfoStatus];
        if (status) {
            const auto value = static_cast<SCFrameStatus>(status.integerValue);
            if (value != SCFrameStatusComplete && value != SCFrameStatusStarted) {
                std::lock_guard<std::mutex> lock(d->mutex);
                d->idle++;
                return;
            }
        }
        NSNumber* displayTime = info[SCStreamFrameInfoDisplayTime];
        if (displayTime) {
            // Mach time of the present, brought onto the steady clock through
            // "how long ago was that" — the two clocks tick together.
            const int64_t agoUs = mw::native::capture::machToUs(mach_absolute_time()) -
                                  mw::native::capture::machToUs(displayTime.unsignedLongLongValue);
            displayUs = mw::native::capture::steadyNowUs() - (agoUs > 0 ? agoUs : 0);
        }
    }

    CVImageBufferRef image = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!image) return;
    const int64_t nowUs = mw::native::capture::steadyNowUs();

    std::lock_guard<std::mutex> lock(d->mutex);
    if (d->pending) d->replaced++;
    d->clearPending();
    d->pending = CVPixelBufferRetain(image);
    d->pendingPresentUs = displayUs > 0 ? displayUs : nowUs;
    d->pendingCapturedUs = nowUs;
    d->delivered++;
    d->cv.notify_one();
}

/// One buffer of host audio, as Core Audio hands it over: float32, 48 kHz,
/// and PLANAR — one buffer per channel — which is why the interleaver exists.
/// Called on the audio queue, which is serial, so the scratch buffer needs no
/// lock of its own.
- (void)handleAudio:(CMSampleBufferRef)sampleBuffer
{
    auto* d = self.owner;
    if (!d) return;
    if (!d->onAudio) return;
    d->audioCallbacks++;
    const CMItemCount frames = CMSampleBufferGetNumSamples(sampleBuffer);
    if (frames <= 0) return;

    // The buffer list is asked for its own size first, then filled. A fixed
    // struct sized for eight channels looks like it should be enough and is
    // NOT: CoreMedia answers kCMSampleBufferError_ArrayTooSmall (-12737) for
    // it, because the size it wants covers more than the buffers themselves.
    // Measured on macOS 15.6 — the two-call form is the one that works, and
    // the storage is kept between buffers so the audio queue allocates nothing
    // per packet.
    size_t needed = 0;
    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer, &needed, nullptr, 0, kCFAllocatorDefault, kCFAllocatorDefault, 0,
            nullptr) != noErr ||
        needed == 0)
        return;
    if (d->audioListStorage.size() < needed) d->audioListStorage.resize(needed);
    auto* list = reinterpret_cast<AudioBufferList*>(d->audioListStorage.data());

    CMBlockBufferRef block = nullptr;
    const OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sampleBuffer, nullptr, list, d->audioListStorage.size(), kCFAllocatorDefault,
        kCFAllocatorDefault, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block);
    if (status != noErr || !block) {
        if (d->audioCallbacks == 1)
            mw::native::log::warning("[native] audio tap: buffer list refused, status=" +
                                     std::to_string(status));
        if (block) CFRelease(block);
        return;
    }

    const size_t count = static_cast<size_t>(frames);
    const UInt32 buffers = list->mNumberBuffers;
    if (d->audioFrames == 0)
        mw::native::log::info("[native] audio tap: " + std::to_string(buffers) + " buffer(s), " +
                              std::to_string(list->mBuffers[0].mNumberChannels) +
                              " channel(s) each, " + std::to_string(count) + " samples");
    if (buffers == 1 && list->mBuffers[0].mNumberChannels == 2) {
        // Already interleaved stereo: hand it over untouched.
        d->onAudio(static_cast<const float*>(list->mBuffers[0].mData), count);
    } else if (buffers >= 1) {
        const float* planes[2] = {nullptr, nullptr};
        const int planeCount = buffers >= 2 ? 2 : 1;
        for (int i = 0; i < planeCount; ++i)
            planes[i] = static_cast<const float*>(list->mBuffers[i].mData);
        d->audioScratch.resize(count * 2);
        mw::native::audio::interleaveToStereo(planes, planeCount, count, d->audioScratch.data());
        d->onAudio(d->audioScratch.data(), count);
    }
    d->audioFrames += count;
    CFRelease(block);
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
    (void)stream;
    if (!self.owner) return;
    auto* d = self.owner;
    std::lock_guard<std::mutex> lock(d->mutex);
    d->lost = true;
    d->lostReason = mw::native::capture::describe(error);
    d->cv.notify_all();
}

@end

namespace mw::native::capture {

SckCapture::SckCapture(uint32_t displayId, int outputWidth, int outputHeight, int refreshMilliHz,
                       bool showsCursor)
    : d(std::make_unique<Impl>())
    , m_DisplayId(displayId)
    , m_Width(outputWidth)
    , m_Height(outputHeight)
    , m_RefreshMilliHz(refreshMilliHz)
    , m_ShowsCursor(showsCursor)
{}

SckCapture::~SckCapture()
{
    stop();
}

bool SckCapture::start(std::string& error)
{
    if (d->started) return true;

    // The display's rectangle in points, for the input layer.
    const CGRect bounds = CGDisplayBounds(m_DisplayId);
    m_Rect = DesktopRect{static_cast<int>(bounds.origin.x), static_cast<int>(bounds.origin.y),
                         static_cast<int>(bounds.origin.x + bounds.size.width),
                         static_cast<int>(bounds.origin.y + bounds.size.height)};

    // Output size: what was asked, or the display's own pixels. Even, as NV12
    // wants; SCK scales the composited desktop into it on the GPU.
    if (m_Width <= 0 || m_Height <= 0) {
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(m_DisplayId);
        if (!mode) {
            error = "the display has no current mode (unplugged?)";
            return false;
        }
        m_Width = static_cast<int>(CGDisplayModeGetPixelWidth(mode));
        m_Height = static_cast<int>(CGDisplayModeGetPixelHeight(mode));
        CGDisplayModeRelease(mode);
    }
    m_Width &= ~1;
    m_Height &= ~1;
    if (m_Width < 2 || m_Height < 2) {
        error = "the output size is empty";
        return false;
    }
    if (m_RefreshMilliHz <= 0) m_RefreshMilliHz = 60000;

    @autoreleasepool {
        // What can be shared, asked once. Asynchronous by API; waited on
        // here, because start() is a synchronous contract and the answer
        // takes tens of milliseconds.
        __block SCShareableContent* content = nil;
        __block NSError* contentError = nil;
        dispatch_semaphore_t gate = dispatch_semaphore_create(0);
        [SCShareableContent
            getShareableContentExcludingDesktopWindows:NO
                                   onScreenWindowsOnly:YES
                                     completionHandler:^(SCShareableContent* c, NSError* e) {
                                         content = c;
                                         contentError = e;
                                         dispatch_semaphore_signal(gate);
                                     }];
        if (dispatch_semaphore_wait(gate, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) !=
            0) {
            error = "ScreenCaptureKit did not answer within 5 s";
            return false;
        }
        if (!content) {
            error = "ScreenCaptureKit refused to list the displays: " + describe(contentError) +
                    " — Screen Recording permission is the usual cause";
            return false;
        }

        SCDisplay* display = nil;
        for (SCDisplay* candidate in content.displays) {
            if (candidate.displayID == m_DisplayId) {
                display = candidate;
                break;
            }
        }
        if (!display) {
            error = "display " + std::to_string(m_DisplayId) + " is not among the " +
                    std::to_string(content.displays.count) + " ScreenCaptureKit can share";
            return false;
        }

        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display
                                                          excludingWindows:@[]];

        SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
        config.width = static_cast<size_t>(m_Width);
        config.height = static_cast<size_t>(m_Height);
        // NV12, video range, BT.709: exactly what the encoder consumes, so
        // there is no conversion stage on this platform (see the header).
        config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        config.colorMatrix = kCGDisplayStreamYCbCrMatrix_ITU_R_709_2;
        config.colorSpaceName = kCGColorSpaceSRGB;
        // Every present of the panel: the session's own cadence gate decides
        // which ones the stream carries, the same way it does on Windows.
        config.minimumFrameInterval = CMTimeMake(1000, m_RefreshMilliHz);
        config.queueDepth = 3;
        config.showsCursor = m_ShowsCursor;

        // The host's sound, when the session asked for it. 48 kHz stereo is
        // what the rest of the path speaks, so no resampling happens anywhere.
        // The property only exists from macOS 13 — on 12.x the picture still
        // streams, silently, and audioActive() says so.
        m_AudioActive = false;
        if (d->onAudio) {
            if (@available(macOS 13.0, *)) {
                config.capturesAudio = YES;
                config.sampleRate = 48000;
                config.channelCount = 2;
                // Our own process makes no sound; excluding it would only
                // hide a browser tab opened ON the Mac being streamed, which
                // is a feedback loop the user can hear and close themselves.
                config.excludesCurrentProcessAudio = NO;
                m_AudioActive = true;
            } else {
                log::warning("[native] audio: ScreenCaptureKit captures sound from macOS 13 on — "
                             "this Mac streams the picture only");
            }
        }

        d->queue = dispatch_queue_create("moonlightweb.native.sck", DISPATCH_QUEUE_SERIAL);
        MWSckSink* sink = [[MWSckSink alloc] init];
        sink.owner = d.get();
        d->sink = sink;
        d->config = config;
        d->lost = false;
        d->lostReason.clear();

        SCStream* stream = [[SCStream alloc] initWithFilter:filter
                                              configuration:config
                                                   delegate:sink];
        NSError* addError = nil;
        if (![stream addStreamOutput:sink
                                type:SCStreamOutputTypeScreen
                  sampleHandlerQueue:d->queue
                               error:&addError]) {
            error = "ScreenCaptureKit refused the frame output: " + describe(addError);
            return false;
        }

        if (m_AudioActive) {
            if (@available(macOS 13.0, *)) {
                d->audioQueue =
                    dispatch_queue_create("moonlightweb.native.sck.audio", DISPATCH_QUEUE_SERIAL);
                NSError* audioError = nil;
                if (![stream addStreamOutput:sink
                                        type:SCStreamOutputTypeAudio
                          sampleHandlerQueue:d->audioQueue
                                       error:&audioError]) {
                    // The picture is the session; sound is not worth failing
                    // it. Say what happened and carry on silent.
                    log::warning("[native] audio: ScreenCaptureKit refused the audio output: " +
                                 describe(audioError) + " — streaming silent");
                    m_AudioActive = false;
                }
            }
        }

        __block NSError* startError = nil;
        dispatch_semaphore_t startGate = dispatch_semaphore_create(0);
        [stream startCaptureWithCompletionHandler:^(NSError* e) {
            startError = e;
            dispatch_semaphore_signal(startGate);
        }];
        if (dispatch_semaphore_wait(startGate,
                                    dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
            error = "ScreenCaptureKit did not start within 5 s";
            return false;
        }
        if (startError) {
            error = "ScreenCaptureKit refused to start: " + describe(startError);
            return false;
        }
        d->stream = stream;
    }

    d->started = true;
    log::info("[native] ScreenCaptureKit: display " + std::to_string(m_DisplayId) + " → " +
              std::to_string(m_Width) + "x" + std::to_string(m_Height) + " NV12 at " +
              std::to_string((m_RefreshMilliHz + 500) / 1000) + " Hz, pointer " +
              (m_ShowsCursor ? "in the picture" : "left out") +
              (m_AudioActive ? ", with the host's audio (48 kHz stereo)" : ""));
    return true;
}

void SckCapture::setAudioSink(AudioSampleCallback onSamples)
{
    d->onAudio = std::move(onSamples);
}

void SckCapture::stop()
{
    if (!d) return;
    if (d->stream) {
        @autoreleasepool {
            dispatch_semaphore_t gate = dispatch_semaphore_create(0);
            [d->stream stopCaptureWithCompletionHandler:^(NSError* e) {
                (void)e;
                dispatch_semaphore_signal(gate);
            }];
            dispatch_semaphore_wait(gate, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
            d->stream = nil;
        }
    }
    if (d->sink) {
        [(MWSckSink*)d->sink setOwner:nullptr];
        d->sink = nil;
    }
    d->config = nil;
    d->queue = nullptr;
    // The stream is stopped and the sink disowned, so no audio callback can be
    // in flight: dropping it here is what lets the session free the sink it
    // points at.
    d->audioQueue = nullptr;
    d->onAudio = nullptr;
    m_AudioActive = false;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->clearPending();
        if (d->held) CVPixelBufferRelease(d->held);
        d->held = nullptr;
    }
    if (d->started) {
        log::info(
            "[native] ScreenCaptureKit: " + std::to_string(d->delivered) + " frame(s), " +
            std::to_string(d->idle) + " idle, " + std::to_string(d->replaced) +
            " superseded before being taken" +
            (d->audioFrames > 0 ? ", " + std::to_string(d->audioFrames) + " audio samples" : ""));
        d->started = false;
    }
}

AcquireStatus SckCapture::acquire(int timeoutMs, SckFrame& frame)
{
    if (!d->started) return AcquireStatus::Failed;
    std::unique_lock<std::mutex> lock(d->mutex);
    if (!d->pending && !d->lost) {
        d->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 1),
                       [this] { return d->pending != nullptr || d->lost; });
    }
    if (d->lost) {
        log::warning("[native] ScreenCaptureKit stopped the stream: " + d->lostReason);
        return AcquireStatus::Lost;
    }
    if (!d->pending) return AcquireStatus::Timeout;

    if (d->held) CVPixelBufferRelease(d->held);
    d->held = d->pending;
    d->pending = nullptr;

    frame.pixels = d->held;
    frame.width = m_Width;
    frame.height = m_Height;
    frame.presentUs = d->pendingPresentUs;
    frame.capturedUs = d->pendingCapturedUs;
    return AcquireStatus::Ok;
}

void SckCapture::release()
{
    std::lock_guard<std::mutex> lock(d->mutex);
    if (d->held) CVPixelBufferRelease(d->held);
    d->held = nullptr;
}

bool SckCapture::setShowsCursor(bool shows, std::string& error)
{
    if (shows == m_ShowsCursor) return true;
    m_ShowsCursor = shows;
    if (!d->stream || !d->config) return true;
    @autoreleasepool {
        d->config.showsCursor = shows;
        __block NSError* updateError = nil;
        dispatch_semaphore_t gate = dispatch_semaphore_create(0);
        [d->stream updateConfiguration:d->config
                     completionHandler:^(NSError* e) {
                         updateError = e;
                         dispatch_semaphore_signal(gate);
                     }];
        dispatch_semaphore_wait(gate, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
        if (updateError) {
            error = "ScreenCaptureKit refused the pointer change: " + describe(updateError);
            return false;
        }
    }
    return true;
}

} // namespace mw::native::capture
