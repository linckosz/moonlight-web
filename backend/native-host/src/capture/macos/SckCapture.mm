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

#include "../../core/Log.h"

#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <mach/mach_time.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

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
    if (type != SCStreamOutputTypeScreen || !self.owner) return;
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
        // No audio through this stream (off by default; the property itself
        // only exists from macOS 13).

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
              (m_ShowsCursor ? "in the picture" : "left out"));
    return true;
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
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->clearPending();
        if (d->held) CVPixelBufferRelease(d->held);
        d->held = nullptr;
    }
    if (d->started) {
        log::info("[native] ScreenCaptureKit: " + std::to_string(d->delivered) + " frame(s), " +
                  std::to_string(d->idle) + " idle, " + std::to_string(d->replaced) +
                  " superseded before being taken");
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
