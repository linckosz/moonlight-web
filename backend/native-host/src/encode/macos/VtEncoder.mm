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

#include "VtEncoder.h"

#include "../../core/Log.h"
#include "../RateControl.h"

#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace mw::native::encode {
namespace {

std::string osStatusString(OSStatus status)
{
    return std::to_string(static_cast<int>(status));
}

/// One CFNumber-valued property, with the failure named.
bool setInt(VTCompressionSessionRef session, CFStringRef key, int32_t value, const char* name,
            std::string& error)
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    const OSStatus status = VTSessionSetProperty(session, key, number);
    CFRelease(number);
    if (status != noErr) {
        error = std::string("VideoToolbox refused ") + name + " (" + osStatusString(status) + ")";
        return false;
    }
    return true;
}

bool setBool(VTCompressionSessionRef session, CFStringRef key, bool value, const char* name,
             std::string& error)
{
    const OSStatus status =
        VTSessionSetProperty(session, key, value ? kCFBooleanTrue : kCFBooleanFalse);
    if (status != noErr) {
        error = std::string("VideoToolbox refused ") + name + " (" + osStatusString(status) + ")";
        return false;
    }
    return true;
}

/// Big-endian NAL length, 1 to 4 bytes.
uint32_t readLength(const uint8_t* p, size_t size)
{
    uint32_t value = 0;
    for (size_t i = 0; i < size; ++i)
        value = (value << 8) | p[i];
    return value;
}

} // namespace

struct VtEncoder::Impl
{
    VTCompressionSessionRef session = nullptr;

    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    OSStatus status = noErr;
    bool dropped = false;
    CMSampleBufferRef sample = nullptr;
    /// The contiguous view of the sample's data, when the block buffer was
    /// not already one piece.
    CMBlockBufferRef contiguous = nullptr;

    void clearSample()
    {
        if (contiguous) CFRelease(contiguous);
        contiguous = nullptr;
        if (sample) CFRelease(sample);
        sample = nullptr;
    }
};

namespace {

/// VideoToolbox's output callback: on the encoder's own thread, once per frame
/// submitted. Parks the sample and wakes encode(), which is waiting for it.
void outputCallback(void* refcon, void* frameRefcon, OSStatus status, VTEncodeInfoFlags flags,
                    CMSampleBufferRef sample)
{
    (void)frameRefcon;
    auto* d = static_cast<VtEncoder::Impl*>(refcon);
    std::lock_guard<std::mutex> lock(d->mutex);
    d->clearSample();
    d->status = status;
    d->dropped = (flags & kVTEncodeInfo_FrameDropped) != 0;
    if (sample && status == noErr) {
        CFRetain(sample);
        d->sample = sample;
    }
    d->done = true;
    d->cv.notify_one();
}

} // namespace

VtEncoder::VtEncoder()
    : d(std::make_unique<Impl>())
{}

VtEncoder::~VtEncoder()
{
    stop();
}

bool VtEncoder::init(Codec codec, int width, int height, int fps, int bitrateKbps, bool hdr,
                     const EncoderTuning& tuning, std::string& error)
{
    stop();
    m_Codec = codec;
    m_Width = width & ~1;
    m_Height = height & ~1;
    m_Fps = fps > 0 ? fps : 60;
    m_BitrateKbps = bitrateKbps > 0 ? bitrateKbps : 20000;
    m_Hdr = hdr;
    m_Tuning = tuning;

    CMVideoCodecType codecType = 0;
    switch (codec) {
    case Codec::H264: codecType = kCMVideoCodecType_H264; break;
    case Codec::Hevc: codecType = kCMVideoCodecType_HEVC; break;
    case Codec::Av1: error = "no Apple hardware encodes AV1"; return false;
    }
    if (hdr && codec != Codec::Hevc) {
        // The Selector routes HDR to HEVC; this is the guard that keeps a
        // future caller from asking for a stream no browser decodes.
        error = "HDR needs HEVC on macOS — H.264 has no HDR path a browser decodes";
        return false;
    }

    // Hardware or nothing: the software encoders VideoToolbox also lists are
    // not the path this engine promises, and a session that silently fell on
    // one would read as "the Mac is slow".
    CFMutableDictionaryRef spec = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(spec, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
                         kCFBooleanTrue);

    // The frames will be the capture's own IOSurface-backed NV12 (or, in HDR,
    // 10-bit 'x420') buffers; telling the session so up front lets it skip
    // its own pixel transfer.
    CFMutableDictionaryRef source = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    {
        int32_t format = hdr ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                             : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &format);
        CFDictionarySetValue(source, kCVPixelBufferPixelFormatTypeKey, n);
        CFRelease(n);
        n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &m_Width);
        CFDictionarySetValue(source, kCVPixelBufferWidthKey, n);
        CFRelease(n);
        n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &m_Height);
        CFDictionarySetValue(source, kCVPixelBufferHeightKey, n);
        CFRelease(n);
        CFDictionaryRef surface =
            CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
                               &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(source, kCVPixelBufferIOSurfacePropertiesKey, surface);
        CFRelease(surface);
    }

    VTCompressionSessionRef session = nullptr;
    const OSStatus created =
        VTCompressionSessionCreate(kCFAllocatorDefault, m_Width, m_Height, codecType, spec, source,
                                   kCFAllocatorDefault, outputCallback, d.get(), &session);
    CFRelease(spec);
    CFRelease(source);
    if (created != noErr || !session) {
        error = std::string("VideoToolbox has no hardware ") + toString(codec) +
                " encoder for this size (" + osStatusString(created) + ")";
        return false;
    }
    d->session = session;

    // ── The latency configuration ───────────────────────────────────────────
    // Real time, no reordering (no B-frames, one frame in flight), keyframes
    // only when asked, speed over quality. The same choices every other
    // encoder in this tree made after measurement.
    if (!setBool(session, kVTCompressionPropertyKey_RealTime, true, "real-time mode", error) ||
        !setBool(session, kVTCompressionPropertyKey_AllowFrameReordering, false,
                 "no frame reordering", error) ||
        !setInt(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, 0x7fffffff,
                "the keyframe interval", error) ||
        !setInt(session, kVTCompressionPropertyKey_ExpectedFrameRate, m_Fps,
                "the expected frame rate", error))
        return false;
    {
        // Seconds between forced keyframes: a day, i.e. never on its own.
        double never = 86400.0;
        CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &never);
        VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, n);
        CFRelease(n);
    }
    // Zero frames of delay: the encoder may not hold a frame back to look at
    // the next one. Not every encoder accepts it, so its refusal is logged,
    // never fatal.
    {
        std::string soft;
        if (!setInt(session, kVTCompressionPropertyKey_MaxFrameDelayCount, 0, "zero frame delay",
                    soft))
            log::debug("[native] " + soft);
        if (!setBool(session, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, true,
                     "speed over quality", soft))
            log::debug("[native] " + soft);
    }

    const char* profileName = "";
    if (codec == Codec::H264) {
        if (!setBool(session, kVTCompressionPropertyKey_AllowTemporalCompression, true,
                     "temporal compression", error))
            return false;
        VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel,
                             kVTProfileLevel_H264_High_AutoLevel);
        VTSessionSetProperty(session, kVTCompressionPropertyKey_H264EntropyMode,
                             kVTH264EntropyMode_CABAC);
        profileName = "High";
    } else if (hdr) {
        // Main10, named: the profile is what makes the 10 bits real (a Main
        // session accepts the 10-bit surface and encodes 8 bits into it). And
        // the colour description in the stream — primaries 9, transfer 16,
        // matrix 9 — is what tells the browser to run the PQ curve backwards;
        // without it the picture decodes fine and looks flat and grey
        // (design §16.2). Set on the session so it lands in the VUI.
        VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel,
                             kVTProfileLevel_HEVC_Main10_AutoLevel);
        VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowOpenGOP, kCFBooleanFalse);
        VTSessionSetProperty(session, kVTCompressionPropertyKey_ColorPrimaries,
                             kCMFormatDescriptionColorPrimaries_ITU_R_2020);
        VTSessionSetProperty(session, kVTCompressionPropertyKey_TransferFunction,
                             kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ);
        VTSessionSetProperty(session, kVTCompressionPropertyKey_YCbCrMatrix,
                             kCMFormatDescriptionYCbCrMatrix_ITU_R_2020);
        profileName = "Main10 (BT.2020 PQ)";
    } else {
        VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel,
                             kVTProfileLevel_HEVC_Main_AutoLevel);
        VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowOpenGOP, kCFBooleanFalse);
        profileName = "Main";
    }

    if (!applyRate(m_BitrateKbps, error)) return false;

    const OSStatus prepared = VTCompressionSessionPrepareToEncodeFrames(session);
    if (prepared != noErr) {
        error = "VideoToolbox could not prepare the encoder (" + osStatusString(prepared) + ")";
        return false;
    }

    m_Encoded = 0;
    const uint32_t vbv =
        vbvBits(static_cast<uint32_t>(m_BitrateKbps) * 1000u, m_Fps, m_Tuning.vbvFrames);
    log::info(std::string("[native] VideoToolbox ready: ") + toString(codec) + " " + profileName +
              ", " + std::to_string(m_Width) + "x" + std::to_string(m_Height) + "@" +
              std::to_string(m_Fps) + ", " + std::to_string(m_BitrateKbps) + " kbps, VBV " +
              std::to_string(vbv / 1000) +
              " kbit, real-time, no reordering, keyframes on "
              "demand only — hardware (Apple Video Encoder)" +
              (m_Tuning.describe().empty() ? "" : " [bench: " + m_Tuning.describe() + "]"));
    return true;
}

bool VtEncoder::applyRate(int bitrateKbps, std::string& error)
{
    if (!d->session) return false;
    const uint32_t bitsPerSecond = static_cast<uint32_t>(bitrateKbps) * 1000u;
    if (!setInt(d->session, kVTCompressionPropertyKey_AverageBitRate,
                static_cast<int32_t>(bitsPerSecond), "the average bitrate", error))
        return false;

    // The VBV, as VideoToolbox spells it: at most N bytes over a window of S
    // seconds. One frame's worth (RateControl.h's floor and all) over one
    // frame's time is the same bound the other encoders configure.
    const uint32_t vbv = vbvBits(bitsPerSecond, m_Fps, m_Tuning.vbvFrames);
    const int32_t bytes = static_cast<int32_t>(vbv / 8);
    const double seconds = static_cast<double>(vbv) / static_cast<double>(bitsPerSecond);
    CFNumberRef limitBytes = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bytes);
    CFNumberRef limitSeconds = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &seconds);
    const void* values[2] = {limitBytes, limitSeconds};
    CFArrayRef limits = CFArrayCreate(kCFAllocatorDefault, values, 2, &kCFTypeArrayCallBacks);
    const OSStatus status =
        VTSessionSetProperty(d->session, kVTCompressionPropertyKey_DataRateLimits, limits);
    CFRelease(limits);
    CFRelease(limitBytes);
    CFRelease(limitSeconds);
    if (status != noErr) {
        error = "VideoToolbox refused the data-rate limit (" + osStatusString(status) + ")";
        return false;
    }
    return true;
}

bool VtEncoder::setBitrate(int bitrateKbps, std::string& error)
{
    if (bitrateKbps <= 0) return true;
    if (bitrateKbps == m_BitrateKbps) return true;
    m_BitrateKbps = bitrateKbps;
    return applyRate(bitrateKbps, error);
}

bool VtEncoder::encode(CVPixelBufferRef pixels, bool forceKeyframe, int64_t presentUs,
                       EncoderOutput& out, std::string& error)
{
    if (!d->session) {
        error = "the encoder is not initialized";
        return false;
    }
    if (m_OutputHeld) releaseOutput();
    if (!pixels) {
        error = "no picture to encode";
        return false;
    }

    CFDictionaryRef frameProps = nullptr;
    if (forceKeyframe) {
        const void* keys[1] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
        const void* values[1] = {kCFBooleanTrue};
        frameProps =
            CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
    }

    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->done = false;
        d->clearSample();
    }
    // The presentation time drives the rate control's notion of elapsed
    // time; the frame's real present is the honest value for it.
    const CMTime pts = CMTimeMake(presentUs, 1000000);
    VTEncodeInfoFlags flags = 0;
    const OSStatus submitted = VTCompressionSessionEncodeFrame(
        d->session, pixels, pts, kCMTimeInvalid, frameProps, nullptr, &flags);
    if (frameProps) CFRelease(frameProps);
    if (submitted != noErr) {
        error = "VideoToolbox refused the frame (" + osStatusString(submitted) + ")";
        return false;
    }
    // Wait for THIS frame's bitstream. VideoToolbox is asynchronous; the
    // completion below flushes it, and the callback arrives on its thread.
    VTCompressionSessionCompleteFrames(d->session, kCMTimeInvalid);
    {
        std::unique_lock<std::mutex> lock(d->mutex);
        if (!d->done) d->cv.wait_for(lock, std::chrono::seconds(2), [this] { return d->done; });
        if (!d->done) {
            error = "VideoToolbox did not return the frame within 2 s";
            return false;
        }
        if (d->status != noErr) {
            error = "VideoToolbox failed the frame (" + osStatusString(d->status) + ")";
            return false;
        }
        if (d->dropped || !d->sample) {
            // Dropped by the encoder's own rate control — it should never
            // happen without reordering and with real-time on, and the loop
            // has no frame to emit for it. Reported as an empty output so the
            // caller skips it rather than ending the session.
            out = EncoderOutput{};
            m_OutputHeld = true;
            return true;
        }
    }

    // Keyframe or not, from the sample's own attachments.
    bool keyframe = true;
    if (CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(d->sample, false);
        attachments && CFArrayGetCount(attachments) > 0) {
        auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
        const void* notSync = CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync);
        keyframe = !(notSync && CFBooleanGetValue(static_cast<CFBooleanRef>(notSync)));
    }

    if (!toAnnexB(keyframe, out, error)) return false;
    out.keyframe = keyframe;
    out.avgQp = -1; // VideoToolbox does not say
    m_OutputHeld = true;
    m_Encoded++;
    return true;
}

bool VtEncoder::toAnnexB(bool keyframe, EncoderOutput& out, std::string& error)
{
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(d->sample);
    if (!block) {
        error = "the encoded frame has no data";
        return false;
    }
    if (!CMBlockBufferIsRangeContiguous(block, 0, 0)) {
        if (CMBlockBufferCreateContiguous(kCFAllocatorDefault, block, nullptr, nullptr, 0, 0, 0,
                                          &d->contiguous) != noErr) {
            error = "the encoded frame could not be made contiguous";
            return false;
        }
        block = d->contiguous;
    }
    size_t length = 0;
    size_t total = 0;
    char* data = nullptr;
    if (CMBlockBufferGetDataPointer(block, 0, &length, &total, &data) != noErr || !data) {
        error = "the encoded frame's data is unreadable";
        return false;
    }

    // The parameter sets and the NAL length-field size, from the format.
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(d->sample);
    size_t parameterSets = 0;
    int lengthSize = 4;
    if (m_Codec == Codec::H264) {
        if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                format, 0, nullptr, nullptr, &parameterSets, &lengthSize) != noErr) {
            error = "the H.264 format description has no parameter sets";
            return false;
        }
    } else {
        if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                format, 0, nullptr, nullptr, &parameterSets, &lengthSize) != noErr) {
            error = "the HEVC format description has no parameter sets";
            return false;
        }
    }
    if (lengthSize < 1 || lengthSize > 4) {
        error = "unexpected NAL length field of " + std::to_string(lengthSize) + " bytes";
        return false;
    }

    auto* bytes = reinterpret_cast<uint8_t*>(data);
    static const uint8_t kStartCode[4] = {0, 0, 0, 1};

    if (!keyframe && lengthSize == 4) {
        // In place: every 4-byte length becomes a 4-byte start code, and the
        // frame is handed out from the encoder's own buffer. No copy.
        size_t pos = 0;
        while (pos + 4 <= total) {
            const uint32_t nal = readLength(bytes + pos, 4);
            std::memcpy(bytes + pos, kStartCode, 4);
            pos += 4 + nal;
        }
        if (pos != total) {
            error = "the encoded frame's NAL lengths do not add up";
            return false;
        }
        out.data = bytes;
        out.size = total;
        return true;
    }

    // Keyframe (or an odd length size): assembled, parameter sets first.
    m_Scratch.clear();
    m_Scratch.reserve(total + 256);
    for (size_t i = 0; i < parameterSets; ++i) {
        const uint8_t* set = nullptr;
        size_t setSize = 0;
        OSStatus got = noErr;
        if (m_Codec == Codec::H264)
            got = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &set, &setSize,
                                                                     nullptr, nullptr);
        else
            got = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, i, &set, &setSize,
                                                                     nullptr, nullptr);
        if (got != noErr || !set) continue;
        m_Scratch.insert(m_Scratch.end(), kStartCode, kStartCode + 4);
        m_Scratch.insert(m_Scratch.end(), set, set + setSize);
    }
    size_t pos = 0;
    const size_t ls = static_cast<size_t>(lengthSize);
    while (pos + ls <= total) {
        const uint32_t nal = readLength(bytes + pos, ls);
        pos += ls;
        if (pos + nal > total) break;
        m_Scratch.insert(m_Scratch.end(), kStartCode, kStartCode + 4);
        m_Scratch.insert(m_Scratch.end(), bytes + pos, bytes + pos + nal);
        pos += nal;
    }
    out.data = m_Scratch.data();
    out.size = m_Scratch.size();
    return true;
}

void VtEncoder::releaseOutput()
{
    if (!m_OutputHeld) return;
    std::lock_guard<std::mutex> lock(d->mutex);
    d->clearSample();
    m_OutputHeld = false;
}

void VtEncoder::stop()
{
    if (!d) return;
    if (d->session) {
        VTCompressionSessionInvalidate(d->session);
        CFRelease(d->session);
        d->session = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->clearSample();
    }
    m_OutputHeld = false;
}

} // namespace mw::native::encode
