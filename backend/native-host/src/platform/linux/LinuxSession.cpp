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

#include "../../capture/linux/KmsCapture.h"
#include "../../capture/linux/X11Damage.h"
#if defined(MW_NATIVE_LINUX_PORTAL)
#include "../../capture/linux/GamescopeSession.h"
#include "../../capture/linux/MutterDisplayConfig.h"
#include "../../capture/linux/MutterScreenCast.h"
#include "../../capture/linux/PortalCapture.h"
#include "../../capture/linux/SharedMonitor.h"
#include "../../input/linux/EiInput.h"
#endif
#include "../../convert/linux/GlConvert.h"
#if defined(MW_NATIVE_LINUX_VULKAN)
#include "../../convert/linux/VulkanConvert.h"
#include "../../encode/linux/VulkanAv1Encoder.h"
#include "../../encode/linux/VulkanHevcEncoder.h"
#include "../../encode/linux/VulkanHevcProof.h"
#include "vulkan/VulkanDevice.h"
#endif
#include "../../core/CadenceAlign.h"
#include "../../core/ClickTrace.h"
#include "../../core/CursorPositionGate.h"
#include "../../core/FrameCadence.h"
#include "../../core/LinuxRouteChoice.h"
#include "../../core/Log.h"
#include "../../core/RestartBackoff.h"
#include "../../core/Selector.h"
#include "../../core/Session.h"
#include "../../encode/EncodeLoadCap.h"
#include "../../encode/RateControl.h"
#include "../../encode/RateGovernor.h"
#include "SleepInhibit.h"
#include "../../convert/linux/CpuConvert.h"
#include "../../convert/linux/GlReadback.h"
#include "../../encode/OpenH264Encoder.h"
#include "../../encode/linux/VaapiEncoder.h"
#include "../../input/linux/UinputGamepad.h"
#include "../../input/linux/UinputInput.h"
#include "../../input/linux/WaylandLayout.h"
#include "../../input/linux/X11Layout.h"
#if defined(MW_NATIVE_LINUX_AUDIO)
#include "../../audio/PacedOpusSink.h"
#include "../../audio/linux/HostMute.h"
#include "../../audio/linux/PipeWireCapture.h"
#endif

#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

// The Linux capture → encode → deliver pipeline.
//
// A port of WindowsSession, stage for stage: the same single thread with no
// queue, the same cadence gate, the same still-screen floor and refinement
// burst, the same three-layer bitrate (ceiling → link governor → per-frame
// budget), the same restart on a lost display. Where the two differ it is
// because the platform does — and each difference is marked where it lives:
//
//  - the picture the pointer-only path re-converts is the LAST KMS BUFFER,
//    held by its fd, not a copy (KmsCapture::acquire);
//  - the encoder owns the NV12 surface and the converter renders into it
//    (VaapiEncoder::inputTarget), the reverse of D3D11;
//  - no intra-refresh on radeonsi 23.2 (SessionInfo says so), but reference
//    invalidation IS available: VA-API hands the reference list to us picture
//    by picture, so a lost frame heals with a delta on the GPU pair. Not on the
//    CPU pair — OpenH264 writes its own list;
//  - the sound comes from PipeWire (the default output's monitor), a push
//    source like ScreenCaptureKit's tap, so it goes through PacedOpusSink
//    rather than owning its thread the way WASAPI does — and it is built only
//    where libpipewire is (MW_NATIVE_LINUX_AUDIO);
//  - no HDR.
//
// See §19 of docs/design/native-capture-encoder.md.

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
    /// The held picture encoded again (idle floor, refinement, a keyframe on
    /// request), not a new one: what it costs says nothing about keeping up.
    bool resend = false;
};

FrameStamps resendStamps(int64_t nowUs)
{
    return FrameStamps{nowUs, nowUs, nowUs, nowUs, true};
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

std::string hzString(int milliHz)
{
    return std::to_string((milliHz + 500) / 1000);
}

/// The PCI vendor of the card at @p cardPath (0x1002 AMD, 0x8086 Intel, 0x10DE
/// NVIDIA), 0 when it cannot be told: the vendor table's key (LinuxRoute).
uint32_t pciVendorOf(const std::string& cardPath)
{
    const int fd = ::open(cardPath.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return 0;
    uint32_t vendor = 0;
    drmDevicePtr device = nullptr;
    if (drmGetDevice2(fd, 0, &device) == 0 && device) {
        if (device->bustype == DRM_BUS_PCI && device->deviceinfo.pci)
            vendor = device->deviceinfo.pci->vendor_id;
        drmFreeDevice(&device);
    }
    ::close(fd);
    return vendor;
}

constexpr int kMaxFloorFps = 480;

/// Colour conversion and encoding as ONE object, so the loop is written once.
///
/// Two pairs wear this shape. The GPU pair — GlConvert rendering into the
/// surface VaapiEncoder owns — is the Linux path as it was; the CPU pair —
/// CpuConvert writing planes OpenH264Encoder reads — is the fallback tier for
/// a machine with no render node. Who owns the picture between the two halves
/// is reversed between the pairs (the encoder's surface, the converter's
/// planes), which is exactly why the loop must not know: it asks for a
/// conversion, then for an encode, and the pair sorts out the hand-off.
class VideoPipeline
{
public:
    virtual ~VideoPipeline() = default;

    virtual bool init(const capture::IScreenCapture& capture, Codec codec, int outputWidth,
                      int outputHeight, int fps, int bitrateKbps, bool intraRefresh,
                      const EncoderTuning& tuning, std::string& error) = 0;
    virtual bool convert(const capture::KmsFrame& frame, const capture::CursorState& cursor,
                         const convert::CursorDraw& draw, std::string& error) = 0;
    virtual bool encode(bool forceKeyframe, uint32_t frameNumber, encode::EncoderOutput& out,
                        std::string& error) = 0;
    virtual void releaseOutput() = 0;
    virtual bool setBitrate(int kbps, std::string& error) = 0;
    virtual bool intraRefreshEnabled() const = 0;
    virtual int intraRefreshFrames() const = 0;
    /// Whether a frame the receiver lost can be healed by a delta rather than a
    /// keyframe. False on the CPU pair: OpenH264 writes its own reference list.
    virtual bool supportsReferenceInvalidation() const { return false; }
    virtual bool invalidateReference(uint32_t frameNumber, std::string& error)
    {
        (void)frameNumber;
        error = "reference invalidation is not available on this encoder";
        return false;
    }
    virtual int outputWidth() const = 0;
    virtual int outputHeight() const = 0;
    virtual int copiesPerFrame() const = 0;
    /// For the session's opening log line: the route and its cost. @p source
    /// names where the pixels came from, because the pair cannot know — the
    /// same CPU pair reads a scanout buffer on one machine and a portal's
    /// shared memory on another, and a line that says the wrong one is worse
    /// than no line.
    virtual std::string describe(const char* source) const = 0;
    /// The GPU pair's EGL context follows the capture thread; the thread gives
    /// it back before it ends. The CPU pair has nothing to give back.
    virtual void detachThread() {}
    /// The split route's Vulkan conversion gave up — at init() or while
    /// streaming — where GL would not have: the session rebuilds the pair with
    /// GL converting (LinuxRouteChoice.h). False for every other failure.
    virtual bool conversionGivenUp() const { return false; }
    /// The Vulkan Video chain gave up — at init(), converting or encoding:
    /// the session rebuilds with VA-API. False on every other pair.
    virtual bool vulkanChainGivenUp() const { return false; }
};

/// The resample filter for a stream smaller than the screen: the bench's pick
/// (Lanczos-2 dilated, linear light — docs/bench-native-host §8j), the GPU
/// tier being the one with a GPU to spend on it — kept only where it is
/// cheap (ResampleCost). MW_SCALER=bilinear|lanczos2 is the A/B on a real
/// stream, and pins the filter.
convert::ScaleFilter scaleFilterFromEnvironment(bool& pinned)
{
    convert::ScaleFilter filter = convert::ScaleFilter::Lanczos2;
    pinned = false;
    if (const char* value = std::getenv("MW_SCALER"); value && *value) {
        if (convert::parseScaleFilter(value, filter)) {
            pinned = true;
            log::info(std::string("[native] MW_SCALER in effect: ") + toString(filter));
        } else
            log::info(std::string("[native] MW_SCALER=") + value +
                      " is not a filter (bilinear, lanczos2) — ignored");
    }
    return filter;
}

/// KMS → a GPU conversion → VA-API: the encoder owns the NV12 surface, the
/// converter writes into it. One copy per frame, the bitstream leaving VRAM.
///
/// Two converters wear this shape, with one interface: GlConvert, rendering on
/// the graphics ring through EGL — the Linux path as it was — and VulkanConvert,
/// on a compute queue that runs beside a game rather than behind it — the split
/// route (plan §9-17), AMD's own since §9-20. The encoder is today's either way.
template <class Converter> class VaapiPipeline final : public VideoPipeline
{
public:
    bool init(const capture::IScreenCapture& capture, Codec codec, int outputWidth,
              int outputHeight, int fps, int bitrateKbps, bool intraRefresh,
              const EncoderTuning& tuning, std::string& error) override
    {
        m_ConverterFailed = false;
        // The encoder first: it has the last word on the size. VA-API HEVC
        // rounds down to whole 8-pixel blocks, so a converter built at the size
        // asked for rendered into a surface of another size and the pipeline
        // was refused — measured 15/09/2026, a 1280x1024 display followed at
        // 1350x1080, encoded 1344x1080, stream left at its old shape.
        m_Encoder = std::make_unique<encode::VaapiEncoder>();
        if (!m_Encoder->init(capture.renderNodePath(), codec,
                             outputWidth > 0 ? outputWidth : capture.width(),
                             outputHeight > 0 ? outputHeight : capture.height(), fps, bitrateKbps,
                             intraRefresh, tuning, error))
            return false;
        // Kept only where it is cheap, see noteResampleCost().
        const convert::ScaleFilter filter = scaleFilterFromEnvironment(m_ScalerPinned);
        m_Converter = std::make_unique<Converter>();
#if defined(MW_NATIVE_LINUX_VULKAN)
        // The compute queue at HIGH where the process may have it — the
        // engine's own; priovk=normal measures the route without it.
        if constexpr (std::is_same_v<Converter, convert::VulkanConvert>)
            m_Converter->setWantHighPriority(tuning.prioVk != EncoderTuning::PriorityVk::Normal);
#endif
        if (!m_Converter->init(capture.renderNodePath(), capture.fourcc(), capture.width(),
                               capture.height(), m_Encoder->inputTarget().width,
                               m_Encoder->inputTarget().height, filter, error) ||
            !m_Converter->bindTarget(m_Encoder->inputTarget(), error)) {
            m_ConverterFailed = true;
            return false;
        }
        return true;
    }
    bool convert(const capture::KmsFrame& frame, const capture::CursorState& cursor,
                 const convert::CursorDraw& draw, std::string& error) override
    {
        if (!m_Converter->convert(frame, cursor, draw, error)) {
            m_ConverterFailed = true;
            return false;
        }
        noteResampleCost();
        return true;
    }
    bool conversionGivenUp() const override
    {
        // GL's failures are the session's to end on, as they always were; only
        // the Vulkan conversion has another to fall back on.
#if defined(MW_NATIVE_LINUX_VULKAN)
        if constexpr (std::is_same_v<Converter, convert::VulkanConvert>) return m_ConverterFailed;
#endif
        return false;
    }
    bool encode(bool forceKeyframe, uint32_t frameNumber, encode::EncoderOutput& out,
                std::string& error) override
    {
        return m_Encoder->encode(forceKeyframe, frameNumber, out, error);
    }
    void releaseOutput() override { m_Encoder->releaseOutput(); }
    bool setBitrate(int kbps, std::string& error) override
    {
        return m_Encoder->setBitrate(kbps, error);
    }
    bool intraRefreshEnabled() const override { return m_Encoder->intraRefreshEnabled(); }
    int intraRefreshFrames() const override { return m_Encoder->intraRefreshFrames(); }
    bool supportsReferenceInvalidation() const override
    {
        return m_Encoder->supportsReferenceInvalidation();
    }
    bool invalidateReference(uint32_t frameNumber, std::string& error) override
    {
        return m_Encoder->invalidateReference(frameNumber, error);
    }
    int outputWidth() const override { return m_Converter->outputWidth(); }
    int outputHeight() const override { return m_Converter->outputHeight(); }
    int copiesPerFrame() const override { return 1; }
    std::string describe(const char* source) const override
    {
        return std::string("via VA-API — ") + source + " → " + Converter::apiName() +
               (m_Converter->highPriority() ? " (high priority)" : "") +
               " → VA-API, 1 copy (the bitstream)";
    }
    void detachThread() override
    {
        if (m_Converter) m_Converter->detachThread();
    }

private:
    /// The measured rule of ResampleCost.h, as on Windows: GlConvert times the
    /// resample on the GPU during the stream's first frames, and a pass that
    /// adds more than ResampleCost::kBudgetUs to every frame's latency goes —
    /// convert() waits for the GPU, so its whole cost is latency. On the
    /// Radeon 780M, 1080p to 720p, it was 0.95 ms (22/09/2026): kept.
    void noteResampleCost()
    {
        int64_t costUs = 0;
        if (!m_Converter->takeResampleCost(costUs)) return;
        char ms[16], budget[16];
        std::snprintf(ms, sizeof(ms), "%.1f", costUs / 1000.0);
        std::snprintf(budget, sizeof(budget), "%.1f", convert::ResampleCost::kBudgetUs / 1000.0);
        const bool affordable = convert::ResampleCost::affordable(costUs);
        if (!affordable && !m_ScalerPinned && m_Converter->dropResample()) {
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

    std::unique_ptr<Converter> m_Converter;
    std::unique_ptr<encode::VaapiEncoder> m_Encoder;
    bool m_ScalerPinned = false;
    /// The converter's half failed (init, bindTarget or convert) — not the
    /// encoder's.
    bool m_ConverterFailed = false;
};

/// The Linux path as it was: GL on the graphics ring into VA-API.
using GpuPipeline = VaapiPipeline<convert::GlConvert>;

#if defined(MW_NATIVE_LINUX_VULKAN)
/// KMS → Vulkan compute → Vulkan Video (plan pipeline-video-d3d12-v2, C13.5):
/// the whole chain in one API on one device. The encoder owns its NV12 input
/// image, the conversion writes it in place through its plane views, the
/// encode waits for that on the GPU. One copy per frame, the bitstream.
///
/// Built only once the route choice let it (LinuxRouteChoice.h) — the pixel
/// proof passed for this GPU (VulkanHevcProof) — and given up on at the first
/// failure, opening or streaming: the session goes on through VA-API, or —
/// AV1, which nothing else here encodes (C13.12) — ends.
///
/// @p Encoder: VulkanHevcEncoder or VulkanAv1Encoder, the same interface.
template <typename Encoder> class VulkanPipeline final : public VideoPipeline
{
public:
    /// @p clientCrops: SessionConfig::clientCropsToFrame, which AV1 alone
    /// reads (VulkanAv1Encoder::setClientCrops).
    explicit VulkanPipeline(bool clientCrops = false)
        : m_ClientCrops(clientCrops)
    {}

    bool init(const capture::IScreenCapture& capture, Codec codec, int outputWidth,
              int outputHeight, int fps, int bitrateKbps, bool intraRefresh,
              const EncoderTuning& tuning, std::string& error) override
    {
        m_Failed = true;
        // The conversion's queue at HIGH where the process may have it; the
        // encode queue keeps the default (VulkanHevcEncoder).
        vulkan::DeviceOptions options;
        options.wantHigh = tuning.prioVk != EncoderTuning::PriorityVk::Normal;
        options.encodeHevc = codec != Codec::Av1;
        options.encodeAv1 = codec == Codec::Av1;
        std::shared_ptr<vulkan::VulkanDevice> device =
            vulkan::VulkanDevice::open(capture.renderNodePath(), options, error);
        if (!device) return false;
        m_Encoder = std::make_unique<Encoder>();
        if constexpr (std::is_same_v<Encoder, encode::VulkanAv1Encoder>)
            m_Encoder->setClientCrops(m_ClientCrops);
        if (!m_Encoder->init(device, codec, outputWidth > 0 ? outputWidth : capture.width(),
                             outputHeight > 0 ? outputHeight : capture.height(), fps, bitrateKbps,
                             intraRefresh, tuning, error, encode::witnessFromEnvironment()))
            return false;
        const convert::ScaleFilter filter = scaleFilterFromEnvironment(m_ScalerPinned);
        m_Converter = std::make_unique<convert::VulkanConvert>();
        if (!m_Converter->init(device, capture.fourcc(), capture.width(), capture.height(),
                               m_Encoder->input().width, m_Encoder->input().height, filter,
                               error) ||
            !m_Converter->bindTarget(m_Encoder->input(), error))
            return false;
        m_Failed = false;
        return true;
    }
    bool convert(const capture::KmsFrame& frame, const capture::CursorState& cursor,
                 const convert::CursorDraw& draw, std::string& error) override
    {
        if (!m_Converter->convert(frame, cursor, draw, error)) {
            m_Failed = true;
            return false;
        }
        int64_t costUs = 0;
        if (m_Converter->takeResampleCost(costUs) && !convert::ResampleCost::affordable(costUs) &&
            !m_ScalerPinned && m_Converter->dropResample())
            log::info("[native] resample dropped: Lanczos-2 costs " +
                      std::to_string(costUs / 1000) + " ms of GPU a frame here — bilinear");
        return true;
    }
    bool encode(bool forceKeyframe, uint32_t frameNumber, encode::EncoderOutput& out,
                std::string& error) override
    {
        if (!m_Encoder->encode(forceKeyframe, frameNumber, out, error)) {
            m_Failed = true;
            return false;
        }
        return true;
    }
    bool vulkanChainGivenUp() const override { return m_Failed; }
    void releaseOutput() override { m_Encoder->releaseOutput(); }
    bool setBitrate(int kbps, std::string& error) override
    {
        return m_Encoder->setBitrate(kbps, error);
    }
    bool intraRefreshEnabled() const override { return m_Encoder->intraRefreshEnabled(); }
    int intraRefreshFrames() const override { return m_Encoder->intraRefreshFrames(); }
    bool supportsReferenceInvalidation() const override
    {
        return m_Encoder->supportsReferenceInvalidation();
    }
    bool invalidateReference(uint32_t frameNumber, std::string& error) override
    {
        return m_Encoder->invalidateReference(frameNumber, error);
    }
    int outputWidth() const override { return m_Converter->outputWidth(); }
    int outputHeight() const override { return m_Converter->outputHeight(); }
    int copiesPerFrame() const override { return 1; }
    std::string describe(const char* source) const override
    {
        return std::string("via Vulkan Video — ") + source + " → Vulkan compute" +
               (m_Converter->highPriority() ? " (high priority)" : "") + " → " +
               m_Encoder->describe() + ", 1 copy (the bitstream)";
    }

private:
    // The converter goes first, then the encoder, then — with the last of
    // them — the device they share.
    std::unique_ptr<Encoder> m_Encoder;
    std::unique_ptr<convert::VulkanConvert> m_Converter;
    bool m_ClientCrops = false;
    bool m_ScalerPinned = false;
    bool m_Failed = false;
};
#endif

/// KMS → DMA-BUF mmap → CPU → OpenH264: the converter owns the I420 planes, the
/// encoder reads them. Two copies per frame — the pixels into the planes, the
/// bitstream out — on a machine that has no other way.
///
/// Two converters, one per kind of frame. A machine with a render node but no
/// encoder on it (Fedora's own Mesa, a libva that cannot load the driver —
/// issue #34) hands over the GPU's TILED scanout, which only the GPU can read:
/// GlReadback converts it there and reads the planes back. CpuConvert keeps
/// what it was written for: a linear buffer of a render-node-less machine, and
/// the portal's shared memory, which GL cannot import.
class CpuPipeline final : public VideoPipeline
{
public:
    bool init(const capture::IScreenCapture& capture, Codec codec, int outputWidth,
              int outputHeight, int fps, int bitrateKbps, bool intraRefresh,
              const EncoderTuning& tuning, std::string& error) override
    {
        (void)intraRefresh; // OpenH264 has none; reported false
        if (codec != Codec::H264) {
            error = std::string("OpenH264 encodes H.264 only, not ") + toString(codec);
            return false;
        }
        if (!m_Converter.init(capture.fourcc(), capture.width(), capture.height(), outputWidth,
                              outputHeight, error))
            return false;
        m_UseReadback = false;
        m_ReadbackRefusal.clear();
        const std::string node = capture.renderNodePath();
        if (node.empty()) {
            m_ReadbackRefusal = "this machine has no render node";
        } else if (m_Readback.init(node, capture.fourcc(), capture.width(), capture.height(),
                                   outputWidth, outputHeight, m_ReadbackRefusal)) {
            if (m_Readback.outputWidth() == m_Converter.outputWidth() &&
                m_Readback.outputHeight() == m_Converter.outputHeight()) {
                m_UseReadback = true;
            } else {
                m_ReadbackRefusal = "the GPU and CPU converters disagree on the output size";
                m_Readback.stop();
            }
        }
        if (!m_UseReadback && !node.empty())
            log::warning("[native] the GPU cannot convert for the CPU encoder (" +
                         m_ReadbackRefusal + ") — a tiled scanout will end the session");
        return m_Encoder.init(m_Converter.outputWidth(), m_Converter.outputHeight(), fps,
                              bitrateKbps, 0, tuning, error);
    }
    bool convert(const capture::KmsFrame& frame, const capture::CursorState& cursor,
                 const convert::CursorDraw& draw, std::string& error) override
    {
        if (m_UseReadback && convert::GlReadback::takes(frame)) {
            m_LastFromGpu = true;
            return m_Readback.convert(frame, cursor, draw, error);
        }
        m_LastFromGpu = false;
        if (m_Converter.convert(frame, cursor, draw, error)) return true;
        // The one failure a user can act on, said in their terms: the CPU
        // converter was only ever meant for a machine with no GPU encoder AND
        // no tiled buffer — this one has both problems.
        if (frame.modifier != 0 && frame.modifier != DRM_FORMAT_MOD_LINEAR)
            error = "no GPU encoder is available here (VA-API or Vulkan Video did not "
                    "come up — on Fedora, Mesa's H.264/HEVC encoding comes with RPM Fusion's "
                    "freeworld drivers), and the CPU encoder cannot read this GPU's tiled "
                    "screen buffer: " +
                    (m_ReadbackRefusal.empty() ? error : m_ReadbackRefusal);
        return false;
    }
    bool encode(bool forceKeyframe, uint32_t frameNumber, encode::EncoderOutput& out,
                std::string& error) override
    {
        return m_Encoder.encode(m_LastFromGpu ? m_Readback.picture() : m_Converter.picture(),
                                forceKeyframe, frameNumber, out, error);
    }
    void releaseOutput() override { m_Encoder.releaseOutput(); }
    bool setBitrate(int kbps, std::string& error) override
    {
        return m_Encoder.setBitrate(kbps, error);
    }
    bool intraRefreshEnabled() const override { return false; }
    int intraRefreshFrames() const override { return 0; }
    int outputWidth() const override { return m_Converter.outputWidth(); }
    int outputHeight() const override { return m_Converter.outputHeight(); }
    int copiesPerFrame() const override { return 2; }
    std::string describe(const char* source) const override
    {
        // "mapped" covers both ways in: an mmap of a DMA-BUF on the scanout
        // route, memory the portal already mapped on the other. "read back":
        // GL converted, the CPU copied the planes out.
        return std::string("via ") + encode::OpenH264Encoder::version() + " — " + source +
               (m_UseReadback ? " → EGL → read back → OpenH264, 2 copies (the planes, the "
                                "bitstream), "
                              : " → mapped → CPU → OpenH264, 2 copies (the pixels, the "
                                "bitstream), ") +
               std::to_string(m_Converter.threads()) + "+" + std::to_string(m_Encoder.threads()) +
               " threads";
    }
    void detachThread() override
    {
        if (m_UseReadback) m_Readback.detachThread();
    }

private:
    convert::CpuConvert m_Converter;
    convert::GlReadback m_Readback;
    encode::OpenH264Encoder m_Encoder;
    bool m_UseReadback = false;
    bool m_LastFromGpu = false;
    /// Why the GPU does not convert here, for the log and for the error a
    /// tiled frame ends the session with.
    std::string m_ReadbackRefusal;
};

class LinuxSession final : public Session
{
public:
    LinuxSession(const SessionConfig& config, const ResolvedTarget& target,
                 const SessionCallbacks& callbacks)
        : m_Config(config)
        , m_Target(target)
        , m_Callbacks(callbacks)
        , m_SessionCodec(target.codec)
    {}

    ~LinuxSession() override { stop(); }

    bool start(std::string& error) override
    {
        if (m_Running.load()) return true;

        // The Selector hands over the card (nativeHandle = its minor number)
        // and the display's index among that card's connected connectors —
        // the same contract DXGI's "output index within its adapter" fills on
        // Windows. Resolved back to a connector id here.
        m_CardPath = "/dev/dri/card" + std::to_string(m_Target.captureAdapterHandle);
        m_VendorId = pciVendorOf(m_CardPath);
        // Only the scanout route has a connector to resolve. The portal route
        // has no monitor to name — the user picks one in its dialog — so there
        // is nothing here for it to find, and looking would fail on exactly the
        // machine that cannot read the card in the first place.
        if (m_Target.capture != CaptureApi::PipeWire) {
            std::string listError;
            unsigned index = 0;
            bool found = false;
            for (const capture::KmsOutput& out :
                 capture::KmsCapture::listOutputs(m_CardPath, listError)) {
                // The probe's rule (LinuxProbe.cpp): a display KMS shows
                // nothing on is not offered, so it is not counted either.
                if (!out.connected || !out.active) continue;
                if (index == m_Target.outputIndex) {
                    m_ConnectorId = out.connectorId;
                    m_ConnectorName = out.name;
                    found = true;
                    break;
                }
                ++index;
            }
            if (!found) {
                error = "the display is no longer connected to " + m_CardPath +
                        ", or shows nothing any more" +
                        (listError.empty() ? "" : " (" + listError + ")");
                return false;
            }
        } else {
            m_ConnectorName = m_Target.gamescopeSteam  ? "Steam Big Picture"
                              : m_Target.gamescopeApp  ? "gamescope"
                              : m_Target.portalVirtual ? "virtual display"
                                                       : "portal";
        }

        if (!openCapture(error)) return false;
        m_DisplayMilliHz = m_Capture->refreshMilliHz();

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

        // The frame against the monitor the capture really shows. The Selector
        // shaped it on the card's nominal 1920x1080, which a monitor made at
        // the client's size matches — and another stream's, which a guest
        // records as it is, need not: a phone held upright, say.
        FrameSize frame{m_Config.width, m_Config.height};
#if defined(MW_NATIVE_LINUX_PORTAL)
        // gamescope's screen found running is the size it was started at, for
        // the stream that started it: framed the same way.
        if ((m_OnMutter && !m_MadeMonitor) || (m_OnGamescope && !m_GamescopeStarted)) {
            frame = frameForDisplay({m_Capture->width(), m_Capture->height()}, frame,
                                    policyOf(m_Config));
            if (frame.width != m_Config.width || frame.height != m_Config.height)
                log::info("[native] the monitor recorded is " + std::to_string(m_Capture->width()) +
                          "x" + std::to_string(m_Capture->height()) + " — streaming " +
                          std::to_string(frame.width) + "x" + std::to_string(frame.height) +
                          ", its shape");
        }
#endif
        if (!buildPipeline(frame.width, frame.height, error)) return false;

        // Input: keyboard and mouse through uinput, the gamepad beside them.
        // Either refusing is "no input of that kind this session", never no
        // session — the udev rule is what grants both, and its absence is said
        // in words a user can act on. An app in its own gamescope reads its
        // keyboard and mouse from gamescope's EIS socket, never from a device;
        // its games still read the gamepad's.
        const InputRects inputRects = readInputRects();
        {
            std::lock_guard<std::mutex> lock(m_InputMutex);
            std::unique_ptr<input::IInputSink> sink;
            ClickTrace* trace = m_Config.tuning.clickTrace ? &m_ClickTrace : nullptr;
#if defined(MW_NATIVE_LINUX_PORTAL)
            if (m_OnGamescope) {
                auto ei = std::make_unique<input::EiInput>(
                    capture::gamescopeSocketPath(m_Gamescope.endpoints.eisSocket),
                    m_Gamescope.width, m_Gamescope.height);
                ei->setClickTrace(trace);
                sink = std::move(ei);
            }
#endif
            if (!sink) {
                auto uinput = std::make_unique<input::UinputInput>();
                uinput->setClickTrace(trace);
                sink = std::move(uinput);
            }
            std::string inputError;
            if (sink->start(inputError)) {
                applyInputRects(*sink, inputRects);
                m_Input = std::move(sink);
            } else {
                log::warning("[native] input: keyboard and mouse unavailable — " + inputError);
            }
            auto pads = std::make_unique<input::UinputGamepad>([this](const RumbleEvent& rumble) {
                if (m_Callbacks.onRumble) m_Callbacks.onRumble(rumble);
            });
            std::string padError;
            if (pads->start(padError))
                m_Gamepad = std::move(pads);
            else
                log::info("[native] input: no virtual gamepad this session — " + padError);
        }

#if defined(MW_NATIVE_LINUX_AUDIO)
        // Audio, on the same terms as input: wanted only when the consumer
        // gave us somewhere to put it, and never a reason to fail the session.
        // The sink first — it owns the encoder and the 5 ms cadence — then the
        // capture that feeds it. A daemon that is there but has no output to
        // record is the capture's business (it retries); no daemon at all is
        // "no audio this session", said here.
        if (m_Callbacks.onAudio) {
            if (m_Config.muteHostAudio) {
                // Before the tap opens: the "silent output" strategy moves the
                // default sink, and the tap attaches to whatever is default
                // when IT starts.
                std::string how;
                m_HostMute.engage(how);
                log::info(std::string("[native] audio: ") + how);
                // What it achieved is read back into m_Info below: this
                // function clears m_Info AFTER this point (as the macOS one
                // does), so setting the flag here would be quietly wiped.
            }
            auto sink = std::make_unique<audio::PacedOpusSink>(m_Callbacks.onAudio,
                                                               m_Config.tuning.audioFrameSamples());
            std::string audioError;
            if (!sink->start("PipeWire, the default output's monitor, 48 kHz stereo", audioError)) {
                log::warning("[native] audio unavailable, streaming silent: " + audioError);
            } else {
                auto* raw = sink.get();
                auto tap = std::make_unique<audio::PipeWireCapture>(
                    [raw](const float* pcm, size_t frames) { raw->push(pcm, frames); });
                if (tap->start(audioError)) {
                    m_Audio = std::move(sink);
                    m_AudioTap = std::move(tap);
                } else {
                    log::warning("[native] audio unavailable, streaming silent: " + audioError);
                }
            }
        }
#endif

        m_Info = SessionInfo{};
        m_Info.displayId = m_Target.displayId;
        m_Info.width = m_Pipeline->outputWidth();
        m_Info.height = m_Pipeline->outputHeight();
        // What the cap scales from, fixed for the session.
        m_FullWidth = m_Info.width;
        m_FullHeight = m_Info.height;
        m_Info.fps = m_Config.fps;
        // What the pipeline was really built with — see buildPipeline: a CPU
        // pair forced by the portal's shared memory carries the codec down with
        // it, and the client is told H.264 rather than promised HEVC.
        m_Info.codec = m_Codec;
        // The encoder the pipeline actually built, not the one chosen on paper:
        // the portal's shared memory forces the CPU pair whatever the Selector
        // picked, and the client is told what it is really getting.
        m_Info.encoder = m_UsingCpuPair ? EncoderApi::Software : m_Target.encoder;
        m_Info.capture =
            m_Target.capture == CaptureApi::PipeWire ? CaptureApi::PipeWire : CaptureApi::Kms;
        m_Info.gpuName = m_Target.encodeGpuName;
        noteRoute();
        m_Info.hdr = false;
        m_Info.yuv444 = false;
        // No HDR on this platform at all (LinuxProbe), so the display is SDR and
        // no encoder here would carry it.
        m_Info.displayWidth = m_Capture->width();
        m_Info.displayHeight = m_Capture->height();
        {
            std::lock_guard<std::mutex> lock(m_FormatMutex);
            m_LastFormat = DisplayFormat{m_Info.displayWidth,
                                         m_Info.displayHeight,
                                         m_Info.width,
                                         m_Info.height,
                                         false,
                                         false,
                                         false};
        }
        m_Info.intraRefresh = m_Pipeline->intraRefreshEnabled();
        m_Info.intraRefreshFrames = m_Pipeline->intraRefreshFrames();
        m_Info.referenceInvalidation = m_Pipeline->supportsReferenceInvalidation();
        // GPU pair: the scanout buffer is read in place and the encoder's
        // surface written in place, one copy remains (the bitstream leaving
        // VRAM). CPU pair: the pixels into the planes as well.
        m_Info.copiesPerFrame = m_Pipeline->copiesPerFrame();
        m_Info.crossGpuCopy = false;
#if defined(MW_NATIVE_LINUX_AUDIO)
        m_Info.audio = static_cast<bool>(m_Audio);
        m_Info.hostMuted = m_HostMute.strategy() != audio::HostMute::Strategy::None;
#else
        m_Info.audio = false;
#endif

        log::info(std::string("[native] session: ") + m_ConnectorName + " " +
                  std::to_string(m_Info.width) + "x" + std::to_string(m_Info.height) + "@" +
                  std::to_string(m_EncodeFps) + " " + toString(m_Info.codec) + " on " +
                  m_Info.gpuName + " " + m_Pipeline->describe(captureName()) +
                  (m_Info.audio ? ", with the host's audio (PipeWire, 48 kHz stereo)" : ""));

        m_SleepInhibit.engage();

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
            if (m_Gamepad) m_Gamepad->stop();
            m_Gamepad.reset();
        }
#if defined(MW_NATIVE_LINUX_AUDIO)
        // The tap goes first: tearing it down is what guarantees no sample
        // callback is still in flight when the sink it pushes into is freed.
        m_AudioTap.reset();
        m_Audio.reset();
        // After the tap, never before: releasing puts the default output back,
        // and the session manager would walk a running tap onto it.
        m_HostMute.release();
#endif
        m_SleepInhibit.release();
        if (!wasRunning && !m_Pipeline && !m_Capture) return;
        releaseCapture(false);
    }

    const SessionInfo& info() const override { return m_Info; }

    void sendInput(const InputEvent& event) override
    {
        // On the caller's thread, never the capture thread's (§8).
        std::lock_guard<std::mutex> lock(m_InputMutex);
        using Type = InputEvent::Type;
        switch (event.type) {
        case Type::ControllerArrival:
            if (m_Gamepad) m_Gamepad->arrive(event);
            break;
        case Type::ControllerState:
            if (m_Gamepad) m_Gamepad->update(event);
            break;
        case Type::ControllerRemoval:
            if (m_Gamepad) m_Gamepad->remove(event);
            break;
        default:
            if (event.type == Type::MouseMoveRelative)
                m_ViewerPointer.store(2, std::memory_order_relaxed);
            else if (event.type == Type::MouseMoveAbsolute)
                m_ViewerPointer.store(1, std::memory_order_relaxed);
            if (m_Input) m_Input->inject(event);
            break;
        }
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

    /// One wake-up of the capture into the click trace (clicktrace=1): what it
    /// brought, the route's own stamps on it — KMS: the vblank it was read
    /// after and its sequence; the portal: the PipeWire buffer header's pts and
    /// seq, and when its process callback handed the buffer over. Linux has no
    /// compositor timing to read beside it. Capture thread only.
    void traceCapture(capture::AcquireStatus status, const capture::KmsFrame& frame,
                      int64_t startUs, int64_t doneUs)
    {
        ClickTrace::Row row;
        row.kind = ClickTrace::Kind::Capture;
        row.us = doneUs;
        row.startUs = startUs;
        row.status = acquireStatusName(status);
        if (status == capture::AcquireStatus::Ok) {
            row.presentUs = frame.presentUs;
            row.presentRawUs = frame.presentRawUs;
            row.accumulated = frame.accumulated > 0 ? frame.accumulated : -1;
            row.composedFrames = frame.sequence;
            row.deliveredUs = frame.capturedUs;
        }
        m_ClickTrace.add(row);
    }

    void setCompositeCursor(bool composite, int cursorFramePx) override
    {
        const int wanted = cursorFramePx > 0 ? cursorFramePx : 0;
        if (m_CursorFramePx.exchange(wanted) != wanted && composite) m_CursorDirty.store(true);
        if (m_CompositeCursor.exchange(composite) == composite) return;
        log::info(composite ? "[native] cursor: drawn into the picture (gaming)"
                            : "[native] cursor: handed to the client to draw (desktop)");
        m_ResendCursor.store(true);
        if (composite) m_ForceKeyframe.store(true);
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
        if (!m_Info.referenceInvalidation) {
            // The CPU pair, or a pipeline that has not started: the receiver's
            // lost frame costs a keyframe, which is what SessionInfo promised.
            m_ForceKeyframe.store(true);
            return;
        }
        // Stored as +1 so that zero can mean "nothing pending" — frame 0 is a
        // real frame number. When several losses arrive before the next
        // picture, the OLDEST wins: it is the stricter of the two, and healing
        // against a picture older than both is correct for both.
        const uint32_t wanted = frameNumber + 1;
        uint32_t seen = m_PendingInvalidation.load();
        while ((seen == 0 || wanted < seen) &&
               !m_PendingInvalidation.compare_exchange_weak(seen, wanted)) {}
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

    /// Assigned into the callback bundle rather than kept beside it: openCapture
    /// already reads m_Callbacks.onPortalGrant, and two places to look for one
    /// listener is how one of them ends up stale. Registering after start() is
    /// a no-op by construction — openCapture has already run.
    void setPortalGrantCallback(PortalGrantCallback callback) override
    {
        m_Callbacks.onPortalGrant = std::move(callback);
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

    /// Where the pictures come from, as the session's line names it.
    const char* captureName() const
    {
#if defined(MW_NATIVE_LINUX_PORTAL)
        if (m_OnMutter) return "GNOME's screen cast";
        if (m_OnGamescope) return "gamescope";
#endif
        return m_Target.capture == CaptureApi::PipeWire ? "portal" : "KMS";
    }

    bool takeLinkFeedback(LinkFeedback& out)
    {
        std::lock_guard<std::mutex> lock(m_LinkMutex);
        if (!m_LinkPending) return false;
        out = m_LinkFeedback;
        m_LinkPending = false;
        return true;
    }

    bool openCapture(std::string& error)
    {
#if defined(MW_NATIVE_LINUX_PORTAL)
        if (m_Target.gamescopeSteam || m_Target.gamescopeApp) return openGamescope(error);
        if (m_Target.capture == CaptureApi::PipeWire) {
            // The grant as it stands NOW: a restart reopens the portal, and a
            // portal that rotates its tokens has already spent the one the
            // session started with.
            if (m_PortalToken.empty()) m_PortalToken = m_Config.portalRestoreToken;
            // One stream at a time makes, finds or removes the virtual display
            // on this desktop (SharedMonitor.h): each makes the compositor
            // rebuild its monitors, and rebuilds that overlapped crashed
            // gnome-shell. Held until the layout has settled.
            capture::SharedMonitorLock lock;
            if (m_Target.portalVirtual) {
                std::string why;
                if (!lock.take(kSharedLockWaitMs, why))
                    log::warning(
                        "[native] virtual display: going on without the desktop's lock — " + why);
            }
            // One portal session at a time: the old stream goes before the new
            // one is asked for (a restart reaches here with it still open).
            if (m_Capture) releaseCapture(true);
            m_VirtualConnector.clear();
            // GNOME: its own screen cast, with no dialog (C2), the portal
            // behind it for a Mutter that turns the route down outright.
            std::string notMutter;
            if (m_Target.portalVirtual && wantMutter(notMutter)) {
                bool refused = false;
                if (openMutter(error, refused)) {
                    m_PortalModes = capture::KmsCapture::modeSignature(m_CardPath);
                    m_PortalOpenedModes = m_PortalModes;
                    return true;
                }
                if (!refused) return false;
                m_MutterRefusal = "GNOME refused its own screen cast (" + error + ")";
                log::warning("[native] virtual display: " + m_MutterRefusal +
                             " — the portal makes it");
            }
            // A screen of the desktop, with no scanout to read: GNOME records
            // it itself too, rather than the portal asking the user to pick it
            // in a dialog on the host's screen — which a viewer elsewhere never
            // answers, and the stream timed out on (UM790Pro, a build without
            // the launcher's capabilities, 06/10/2026). The primary, when it
            // is a real monitor: what "Screen" shows. The portal remains for a
            // GNOME that refuses, or another desktop.
            if (!m_Target.portalVirtual && wantMutter(notMutter)) {
                capture::DisplayLayout layout;
                uint32_t serial = 0;
                std::string why;
                const std::string screen = capture::MutterDisplayConfig::read(layout, serial, why)
                                               ? capture::screenToRecord(layout)
                                               : std::string();
                bool refused = false;
                if (!screen.empty() && startMutter(screen, error, refused, true)) {
                    // The pointer lands on it by its name (readInputRects).
                    m_VirtualConnector = screen;
                    log::info("[native] screen: " + screen +
                              " recorded by GNOME's own screen cast, no dialog");
                    watchLayout();
                    m_PortalModes = capture::KmsCapture::modeSignature(m_CardPath);
                    m_PortalOpenedModes = m_PortalModes;
                    return true;
                }
                if (refused) m_MutterRefusal = "GNOME refused its own screen cast (" + error + ")";
                log::info("[native] screen: " +
                          (screen.empty() ? std::string("no real monitor in GNOME's layout")
                                          : screen + " not recorded by GNOME (" + error + ")") +
                          " — the portal asks for one");
                error.clear();
            }
            auto portal = std::make_unique<capture::PortalCapture>();
            // A monitor made for this stream, at its size, and at the rate the
            // consumer asks of a display it makes — 240 Hz, faster than the
            // stream, whose own cadence the gate keeps (chooseCadence reads
            // the rate the format settled on). Its consent is a different one
            // from a monitor's: the consumer hands the matching token in, and
            // stores what comes back apart.
            if (m_Target.portalVirtual)
                portal->setVirtualMonitor(m_Config.width, m_Config.height,
                                          m_Config.virtualRefreshHz > 0 ? m_Config.virtualRefreshHz
                                                                        : m_Config.fps);
            // KDE Plasma 6: its portal makes no virtual monitor, KWin does
            // (KwinVirtualOutput.h). The owner's output keeps one name, so
            // KDE's display settings know it from stream to stream; a guest's
            // is its own.
            m_KwinOutputName.clear();
            if (m_Target.portalVirtual && (capture::PortalScreenCast::sourceTypes() &
                                           capture::PortalScreenCast::kSourceVirtual) == 0) {
                m_KwinOutputName = m_Config.virtualPrimary
                                       ? std::string("MoonlightWeb")
                                       : "MoonlightWeb-" + std::to_string(::getpid());
                portal->setKwinVirtualOutput(m_KwinOutputName);
            }
            portal->setRestoreToken(m_PortalToken);
            // The GPU the pair converts and encodes on, whatever the buffers:
            // the Vulkan conversion reads shared memory too (C13.10).
            portal->setRenderNode(capture::KmsCapture::renderNodeFor(m_CardPath));
            // DMA-BUF where this GPU imports what the compositor can make: the
            // GPU pair then reads the portal's buffers in place, as it reads
            // the scanout. Not after one it could not read (leaveDmabuf), nor
            // when the bench asks for shared memory (portaldmabuf=0).
            if (!m_PortalShmOnly && m_Config.tuning.portalDmabuf != EncoderTuning::Choice::Off)
                portal->offerDmabuf(dmabufOffer());
            // GNOME's monitors before this session's own exists: what tells it
            // from another stream's (placeVirtualMonitor).
            std::vector<std::string> monitorsBefore;
            std::string notGnome;
            const bool onGnome = m_Target.portalVirtual && m_KwinOutputName.empty() &&
                                 capture::MutterDisplayConfig::connectors(monitorsBefore, notGnome);
            const bool primaryWanted = onGnome && m_Config.virtualPrimary;
            if (m_Target.portalVirtual && m_Config.virtualPrimary && !primaryWanted &&
                m_KwinOutputName.empty())
                log::info("[native] virtual display: left where the compositor put it — " +
                          notGnome);
            if (!portal->start(error)) return false;
            // A compositor that settles on DMA-BUF again and again and fills
            // nothing — KWin on a renderer that cannot allocate the modifier
            // (the Plasma 6.3 VM, 01/10/2026: hundreds of renegotiations a
            // second) — is asked once more for shared memory, which every one
            // can fill.
            if (portal->dmabuf() && portal->renegotiatingWithoutPicture() && !m_PortalShmOnly) {
                log::warning("[native] the compositor renegotiates DMA-BUF without a picture — "
                             "the capture is asked again in shared memory");
                m_PortalShmOnly = true;
                portal.reset();
                // Taken again by the call below: a lock held here would
                // keep it waiting on itself.
                lock.release();
                return openCapture(error);
            }
            // KWin names its virtual outputs "Virtual-<name>": the pointer
            // mapping finds this one by it (readInputRects).
            if (!m_KwinOutputName.empty()) m_VirtualConnector = "Virtual-" + m_KwinOutputName;
            // A grant only comes back from a start that raised the dialog.
            // Handing it up is what spares the user every later one — the
            // consumer stores it and passes it back in SessionConfig.
            const std::string granted = portal->restoreToken();
            if (!granted.empty() && granted != m_PortalToken) {
                m_PortalToken = granted;
                if (m_Callbacks.onPortalGrant) m_Callbacks.onPortalGrant(granted);
            }
            m_PortalDmabuf = portal->dmabuf();
            m_PortalCapture = portal.get();
            m_Capture = std::move(portal);
            // Before the modes are noted: a new layout may hand the screens
            // other CRTCs, and the watch below is not to take that for a mode
            // change of the user's.
            if (onGnome) placeVirtualMonitor(monitorsBefore, primaryWanted);
            m_PortalModes = capture::KmsCapture::modeSignature(m_CardPath);
            m_PortalOpenedModes = m_PortalModes;
            return true;
        }
#endif
        auto kms = std::make_unique<capture::KmsCapture>(m_CardPath, m_ConnectorId);
        capture::KmsCapture& scanout = *kms;
#if defined(MW_NATIVE_LINUX_PORTAL)
        m_PortalCapture = nullptr;
#endif
        m_Capture = std::move(kms);
        if (!m_Capture->start(error)) return false;
        watchInPlaceDrawing(scanout);
        return true;
    }

#if defined(MW_NATIVE_LINUX_PORTAL)
    /// Steam's Big Picture in its own gamescope (GamescopeSession.h): the
    /// session found running — by another stream, or by one that ended less
    /// than ten minutes ago — else started at this stream's size and rate, then
    /// read on the session's PipeWire. The rate is the stream's own: the game
    /// sees a screen of that rate and renders no more than it.
    bool openGamescope(std::string& error)
    {
        if (m_Capture) releaseCapture(true);
        capture::GamescopeApp app;
        app.card = "steam";
        app.steam = true;
        if (m_Target.gamescopeApp) {
            // One of the user's apps: a session of its own, named after it,
            // its command run by the shell as the owner wrote it.
            if (m_Config.gamescopeCommand.empty()) {
                error = "the app has no command";
                return false;
            }
            app.card = capture::gamescopeCardName(m_Config.gamescopeApp);
            app.steam = false;
            app.command = {"/bin/sh", "-c", m_Config.gamescopeCommand};
        }
        // The bench's stand-in for Steam: the same card, the same route, an
        // app that needs no sign-in (MW_GAMESCOPE_APP="vkcube --wsi xcb").
        else if (const char* bench = std::getenv("MW_GAMESCOPE_APP"); bench && *bench) {
            app.card = "bench";
            app.steam = false;
            std::istringstream words(bench);
            for (std::string word; words >> word;)
                app.command.push_back(word);
            log::warning(std::string("[native] gamescope: MW_GAMESCOPE_APP in effect — \"") +
                         bench + "\" instead of Steam");
        }
        m_GamescopeApp = app.steam               ? std::string("Steam")
                         : m_Target.gamescopeApp ? m_Config.gamescopeApp
                         : app.command.empty()   ? std::string("the app")
                                                 : app.command[0];
        capture::GamescopeSession session;
        if (!capture::openGamescopeSession(app, m_Config.width, m_Config.height,
                                           m_Config.fps > 0 ? m_Config.fps : 60, session, error))
            return false;
        auto cast = std::make_unique<capture::PortalCapture>();
        cast->setGamescope(session.record.node, session.record.endpoints.xDisplay, session.pid);
        cast->setRenderNode(capture::KmsCapture::renderNodeFor(m_CardPath));
        if (!m_PortalShmOnly && m_Config.tuning.portalDmabuf != EncoderTuning::Choice::Off)
            cast->offerDmabuf(dmabufOffer());
        if (!cast->start(error)) return false;
        m_Gamescope = session.record;
        m_GamescopeStarted = session.started;
        m_OnGamescope = true;
        m_PortalDmabuf = cast->dmabuf();
        m_PortalCapture = cast.get();
        m_Capture = std::move(cast);
        keepGamescope();
        return true;
    }

    /// The ten-minute timer armed anew, off the capture thread: two systemctl
    /// calls, which the frames should not wait on.
    void keepGamescope()
    {
        m_NextGamescopeKeepUs = steadyNowUs() + kGamescopeKeepUs;
        const capture::GamescopeRecord record = m_Gamescope;
        // The bench may shorten the wait, to see it end (MW_GAMESCOPE_LINGER_S).
        int linger = kGamescopeLingerS;
        if (const char* bench = std::getenv("MW_GAMESCOPE_LINGER_S"); bench && std::atoi(bench) > 0)
            linger = std::atoi(bench);
        std::thread([record, linger] { capture::keepGamescopeSession(record, linger); }).detach();
    }

    /// The portal's virtual monitor found in GNOME's layout, for the pointer
    /// mapping (readInputRects) — a guest's sits wherever GNOME put it, right of
    /// every other screen, and the portal's own position for it is 0,0 — and,
    /// when @p primary, made the desktop's primary, on the left of the other
    /// screens, none of them switched off (MonitorLayout.h): GNOME's top bar and
    /// dock come to the stream, as the taskbar comes to the virtual display on
    /// Windows. Mutter made the monitor when the format settled (bench §8s.1),
    /// so it is there by now, or within moments; it is told from another
    /// stream's by its size and by not being in @p before. Nothing here undoes
    /// it: the change is Mutter's temporary kind, and the layout comes back
    /// when the monitor goes with the portal session.
    void placeVirtualMonitor(const std::vector<std::string>& before, bool primary)
    {
        const int width = m_Capture->width();
        const int height = m_Capture->height();
        capture::DisplayLayout layout;
        uint32_t serial = 0;
        std::string why;
        std::string connector;
        for (int waitedMs = 0; connector.empty() && waitedMs <= 2000; waitedMs += 50) {
            if (waitedMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!capture::MutterDisplayConfig::read(layout, serial, why)) break;
            connector = capture::findSessionVirtual(layout, before, width, height);
        }
        if (connector.empty()) {
            log::info("[native] virtual display: left where GNOME put it — " +
                      (why.empty() ? "no single new " + std::to_string(width) + "x" +
                                         std::to_string(height) + " virtual monitor in its layout"
                                   : why));
            return;
        }
        // Known from here, whatever comes of the layout: the pointer mapping
        // finds the monitor by this name (readInputRects).
        m_VirtualConnector = connector;
        if (!primary) {
            log::info("[native] virtual display: " + connector +
                      ", left where GNOME put it (a guest's own screen)");
            return;
        }
        std::string how;
        if (capture::MutterDisplayConfig::makePrimary(connector, how))
            log::info("[native] virtual display: " + how);
        else
            log::warning("[native] virtual display: " + connector + " left where GNOME put it — " +
                         how);
    }

    /// How long a stream waits for the desktop's lock: as it starts, another
    /// stream's display being made (a second or two); as it ends, short of
    /// the 5 s the server gives a worker to leave.
    static constexpr int kSharedLockWaitMs = 10000;
    static constexpr int kSharedLockTeardownMs = 2500;
    /// How long Mutter is given to be done rebuilding its monitors.
    static constexpr int kSettleMs = 2000;

    /// Whether GNOME's own screen cast makes the virtual display (C2): Mutter
    /// answers with an API that makes virtual monitors, the bench did not ask
    /// for the portal (mutter=0), and Mutter did not turn this session's
    /// route down outright. @p why says why not.
    bool wantMutter(std::string& why) const
    {
        if (m_Config.tuning.mutterDirect == EncoderTuning::Choice::Off) {
            why = "the bench asks for the portal (mutter=0)";
            return false;
        }
        if (!m_MutterRefusal.empty()) {
            why = m_MutterRefusal;
            return false;
        }
        const int version = capture::MutterScreenCast::version(why);
        if (version >= capture::MutterScreenCast::kVirtualVersion) return true;
        if (version > 0)
            why = "GNOME's screen cast is version " + std::to_string(version) +
                  ", older than its virtual monitors";
        return false;
    }

    /// One of Mutter's screen casts as the capture: a virtual monitor at the
    /// client's size and at the rate a display made for a stream is made at
    /// (240 Hz, as the portal's: chooseCadence keeps the stream's own) — or,
    /// with @p connector, that monitor as it is. @p refused when Mutter turned
    /// the route down outright.
    bool startMutter(const std::string& connector, std::string& error, bool& refused,
                     bool realMonitor = false)
    {
        auto cast = std::make_unique<capture::PortalCapture>();
        if (connector.empty())
            cast->setVirtualMonitor(m_Config.width, m_Config.height,
                                    m_Config.virtualRefreshHz > 0 ? m_Config.virtualRefreshHz
                                                                  : m_Config.fps);
        cast->setMutter(connector, realMonitor);
        cast->setRenderNode(capture::KmsCapture::renderNodeFor(m_CardPath));
        if (!m_PortalShmOnly && m_Config.tuning.portalDmabuf != EncoderTuning::Choice::Off)
            cast->offerDmabuf(dmabufOffer());
        if (!cast->start(error)) {
            refused = cast->mutterRefused();
            return false;
        }
        m_PortalDmabuf = cast->dmabuf();
        m_PortalCapture = cast.get();
        m_Capture = std::move(cast);
        m_OnMutter = true;
        return true;
    }

    /// The virtual display through GNOME's own screen cast, the desktop's lock
    /// held (SharedMonitor.h). The owner's stream — or a guest's with no
    /// monitor to record — makes a monitor at its client's size, makes it the
    /// desktop's primary and records it as the shared one. A guest's records
    /// the shared monitor as it is: the desktop the owner sees, as a guest
    /// sees it on Windows (Bruno, 01/10/2026). @p refused when Mutter turned
    /// the route down outright, for the portal to take over.
    bool openMutter(std::string& error, bool& refused)
    {
        refused = false;
        const bool owner = m_Config.virtualPrimary;
        std::vector<std::string> before;
        std::string why;
        capture::MutterDisplayConfig::connectors(before, why);
        capture::SharedMonitor shared;
        const bool present =
            capture::readSharedMonitor(shared) &&
            std::find(before.begin(), before.end(), shared.connector) != before.end();
        if (capture::planSharedMonitor(owner, shared, present) ==
            capture::SharedMonitorPlan::Record) {
            if (startMutter(shared.connector, error, refused)) {
                m_SharedShown = shared;
                m_SharedMade = false;
                // The pointer lands on it by its name (readInputRects).
                m_VirtualConnector = shared.connector;
                log::info("[native] virtual display: " + shared.connector + ", the " +
                          (shared.owner ? "owner's" : "first guest's") +
                          " — recorded as it is: the same desktop");
                watchLayout();
                return true;
            }
            if (refused) return false;
            // A monitor Mutter would not record: one of this guest's own,
            // beside it, as a guest had before C2 — never the primary, never
            // the shared one.
            log::warning("[native] virtual display: " + shared.connector +
                         " could not be recorded (" + error +
                         ") — this guest gets a monitor of its own beside it");
            if (!startMutter(std::string(), error, refused)) return false;
            m_MadeMonitor = true;
            placeVirtualMonitor(before, false);
            capture::MutterDisplayConfig::settle(std::string(), kSettleMs);
            watchLayout();
            return true;
        }
        if (!startMutter(std::string(), error, refused)) return false;
        m_MadeMonitor = true;
        // Found in the layout and made its primary: the owner's or, with nobody
        // else streaming, this guest's. Then recorded as the desktop's.
        placeVirtualMonitor(before, true);
        if (!m_VirtualConnector.empty()) {
            m_SharedShown = capture::ownSharedMonitor(m_VirtualConnector, owner);
            m_SharedMade = true;
            if (!capture::publishSharedMonitor(m_SharedShown))
                log::warning("[native] virtual display: " + m_VirtualConnector +
                             " could not be recorded as the desktop's — a guest makes its own");
        }
        // Done rebuilding before another stream may change a thing.
        capture::MutterDisplayConfig::settle(std::string(), kSettleMs);
        watchLayout();
        return true;
    }

    /// GNOME's monitors watched from here on, for the pointer's mapping: once
    /// a session, after its own monitor settled, so its own making is not news.
    void watchLayout()
    {
        if (m_LayoutWatch) return;
        auto watch = std::make_unique<capture::MutterLayoutWatch>();
        std::string why;
        if (watch->start(why))
            m_LayoutWatch = std::move(watch);
        else
            log::info("[native] input: GNOME's monitors not watched — " + why);
    }
#endif

    /// The capture goes, and its screen cast with it. A monitor this stream
    /// made through Mutter leaves the desktop's record first, Mutter removes
    /// it, and its layout settles before another stream may change it — the
    /// desktop's lock held, taken here unless @p locked. A monitor going makes
    /// Mutter lay the desktop out anew, and the shared monitor of another
    /// stream would not be the primary any more: it is made so again.
    void releaseCapture(bool locked)
    {
        m_Pipeline.reset();
#if defined(MW_NATIVE_LINUX_PORTAL)
        m_PortalCapture = nullptr;
        const std::string made = m_OnMutter && m_MadeMonitor ? m_VirtualConnector : std::string();
        m_OnMutter = false;
        m_MadeMonitor = false;
        m_SharedMade = false;
        m_SharedShown = capture::SharedMonitor{};
        if (made.empty()) {
            m_Capture.reset();
            return;
        }
        capture::SharedMonitorLock lock;
        std::string why;
        if (!locked && !lock.take(kSharedLockTeardownMs, why))
            log::warning("[native] virtual display: " + made +
                         " removed without the desktop's lock — " + why);
        capture::withdrawSharedMonitor(made);
        m_Capture.reset();
        const int waited = capture::MutterDisplayConfig::settle(made, kSettleMs);
        log::info("[native] virtual display: " + made + " removed (" + std::to_string(waited) +
                  " ms for GNOME to settle)");
        capture::SharedMonitor shared;
        if (capture::readSharedMonitor(shared) && shared.connector != made) {
            std::string how;
            if (capture::MutterDisplayConfig::makePrimary(shared.connector, how))
                log::info("[native] virtual display: " + how + " — the desktop's shared monitor");
            else
                log::warning("[native] virtual display: the shared monitor " + shared.connector +
                             " was not made primary again — " + how);
            capture::MutterDisplayConfig::settle(std::string(), kSettleMs);
        }
#else
        (void)locked;
        m_Capture.reset();
#endif
    }

    /// An X server does not always flip: it draws into the buffer it scans out
    /// when a flip cannot show the picture — a game presenting with its sync
    /// off, any window of a desktop over several screens — and the scanout then
    /// shows one buffer while its content changes. Its damage events say when;
    /// the capture reads the same buffer again then (X11Damage). A Wayland
    /// compositor and a headless host flip, and have none of this to ask.
    void watchInPlaceDrawing(capture::KmsCapture& scanout)
    {
        const capture::DesktopRect r = scanout.desktopRect();
        if (!m_Damage) m_Damage = std::make_shared<capture::X11Damage>();
        std::string why;
        if (!m_Damage->watch(r.left, r.top, r.right, r.bottom, why)) {
            log::debug("[native] KMS: no X damage to watch (" + why + ")");
            m_Damage.reset();
            return;
        }
        scanout.setInPlaceChanges([damage = m_Damage] { return damage->takeChanged(); });
    }

#if defined(MW_NATIVE_LINUX_PORTAL)
    /// What EGL on this card's render node imports, offered to the portal
    /// for DMA-BUF (C13.3 bis): GL takes over from every other conversion,
    /// so its list is the one a buffer must fit. Asked once a session — the
    /// answer is the driver's, not the stream's. Empty, and the portal asked
    /// for shared memory as before, when EGL lists nothing.
    capture::PortalCapture::DmabufOffer dmabufOffer()
    {
        if (m_DmabufOfferAsked) return m_DmabufOffer;
        m_DmabufOfferAsked = true;
        const std::string node = capture::KmsCapture::renderNodeFor(m_CardPath);
        if (node.empty()) {
            log::info("[native] no render node behind " + m_CardPath +
                      " — the portal is asked for shared memory");
            return m_DmabufOffer;
        }
        capture::PortalCapture::DmabufOffer offer;
        std::string counts;
        for (const uint32_t fourcc :
             {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888}) {
            std::vector<uint64_t> modifiers;
            std::string why;
            if (!convert::GlConvert::importableModifiers(node, fourcc, modifiers, why)) {
                log::info("[native] " + why + " — the portal is asked for shared memory");
                return m_DmabufOffer;
            }
            if (modifiers.empty()) continue;
            counts += (counts.empty() ? "" : ", ") + std::to_string(modifiers.size());
            offer.modifiers.emplace_back(fourcc, std::move(modifiers));
        }
        if (offer.modifiers.empty()) {
            log::info("[native] EGL on " + node +
                      " imports none of the portal's formats — it is asked for shared memory");
            return m_DmabufOffer;
        }
        offer.renderNode = node;
        offer.evenWithTrails = m_Config.tuning.portalDmabuf == EncoderTuning::Choice::On;
        log::info("[native] the portal is offered DMA-BUF on " + node + " (" + counts +
                  " modifiers for XR24, AR24, XB24, AB24), then shared memory");
        m_DmabufOffer = std::move(offer);
        return m_DmabufOffer;
    }
#endif

#if defined(MW_NATIVE_LINUX_PORTAL)
    /// The chain buildPipeline would pick for the buffers the portal hands
    /// now is the one running — as the Vulkan conversion, which reads shared
    /// memory and DMA-BUF alike, keeps it.
    bool routeHoldsForPortalBuffers()
    {
        LinuxRouteFacts facts = routeFacts(!m_PortalDmabuf);
#if defined(MW_NATIVE_LINUX_VULKAN)
        if (linuxRouteWantsVulkanVideo(facts))
            facts.vulkanEncoderRefusal = proofRefusal(m_Info.width, m_Info.height);
#endif
        return m_Pipeline && chooseLinuxRoute(facts).route == m_Route.route;
    }

    /// GNOME's virtual monitor before GNOME 48 (PortalCapture::
    /// switchesDmabufWithPointer): DMA-BUF while the viewer's pointer is
    /// locked, shared memory while it moves over the desktop. Its DMA-BUF
    /// frames keep trails of a pointer that moves over a still desktop; a
    /// game that hides the pointer has the viewer lock theirs, and no pointer
    /// moves on the host. What it saves: ~3 ms of a click on average, 1.5 of
    /// them Mutter's copy into shared memory, the rest the CPU's copy behind
    /// it (design click-waits §9.5). Decided by Bruno, 09/10/2026.
    void followPointerForDmabuf()
    {
        if (!m_PortalCapture || !m_PortalCapture->switchesDmabufWithPointer()) return;
        m_PortalCapture->setDmabufWhilePointerHidden(
            m_ViewerPointer.load(std::memory_order_relaxed) == 2);
    }
#endif

    /// The portal's DMA-BUF could not be converted, where no Vulkan step down
    /// was left: GL itself refused it. Shared memory from here — the portal
    /// reopened without the offer, on the grant (nobody at the host is asked
    /// again), and the CPU pair reads it. False, nothing done, on any other
    /// route or failure: the session ends on those as it always did.
    bool leaveDmabuf(const std::string& error)
    {
        if (m_Target.capture != CaptureApi::PipeWire || !m_PortalDmabuf || m_PortalShmOnly)
            return false;
        m_PortalShmOnly = true;
        log::warning("[native] the portal's DMA-BUF could not be converted (" + error +
                     ") — reopening it for shared memory, encoded on the CPU");
        return true;
    }

    /// The two rectangles an absolute pointer needs: the display being captured,
    /// and the desktop it sits on — the union of every active output.
    ///
    /// The union matters because uinput's absolute device reports a fraction of
    /// its own axis and the compositor spreads that over the whole desktop. With
    /// the display alone, a second monitor is aimed at as if it were the only
    /// one, and the pointer lands somewhere else entirely.
    ///
    /// Read outside the input lock on purpose: this opens the DRM card, and
    /// inject() waits on that same lock. Only this card's outputs are counted
    /// by KMS — a desktop spanning two GPUs is X's to describe (readX11Rects),
    /// and a Wayland compositor's (below).
    struct InputRects
    {
        capture::DesktopRect display;
        capture::DesktopRect desktop;
    };

    InputRects readInputRects() const
    {
        InputRects rects;
        rects.display = m_Capture->desktopRect();
        rects.desktop = rects.display;
#if defined(MW_NATIVE_LINUX_PORTAL)
        // gamescope's screen is on nobody's desktop: its input goes by libei,
        // in its own pixels.
        if (m_OnGamescope) return rects;
#endif

        std::string listError;
        bool any = false;
        input::KmsIdentity identity;
        for (const capture::KmsOutput& out :
             capture::KmsCapture::listOutputs(m_CardPath, listError)) {
            if (m_ConnectorId != 0 && out.connectorId == m_ConnectorId) {
                identity.edid = out.edid;
                identity.connectorId = out.connectorId;
                identity.x = out.x;
                identity.y = out.y;
                identity.width = out.width;
                identity.height = out.height;
            }
            if (!out.active || out.width <= 0 || out.height <= 0) continue;
            const capture::DesktopRect r{out.x, out.y, out.x + out.width, out.y + out.height};
            if (!any) {
                rects.desktop = r;
                any = true;
                continue;
            }
            rects.desktop.left = std::min(rects.desktop.left, r.left);
            rects.desktop.top = std::min(rects.desktop.top, r.top);
            rects.desktop.right = std::max(rects.desktop.right, r.right);
            rects.desktop.bottom = std::max(rects.desktop.bottom, r.bottom);
        }

        if (readX11Rects(identity, rects)) return rects;

        // A Wayland compositor scans every output out of its own buffer, so
        // the CRTC positions above are all (0, 0) there and the union is the
        // biggest screen, not the desktop — the layout lives in the compositor
        // alone. Ask it: xdg-output gives each output's logical rectangle, in
        // the space the compositor stretches an absolute device across. When
        // the captured connector is among them, that layout replaces KMS's
        // (issue #18, a pointer moving on the screen the viewer was not
        // watching). When it is not — an X11 host, a headless one, the portal
        // route with no connector to name — KMS keeps the last word.
        std::vector<input::WaylandOutput> outputs;
        std::string socketUsed;
        std::string why;
        if (input::WaylandLayout::read(outputs, socketUsed, why)) {
            InputRects wl = rects;
            if (m_Target.capture == CaptureApi::PipeWire) {
                // The portal route: no connector to name, but the portal said
                // where its monitor sits (PortalCapture::desktopRect, in the
                // compositor's space already) and the compositor says how big
                // the desktop is around it. A portal that named no position
                // left the picture at the origin; an output of exactly that
                // size, when there is one, is then taken as the monitor.
                // GNOME named the virtual monitor it made for this session
                // (placeVirtualMonitor): that output, wherever it sits.
                std::string name;
                bool placed = !m_VirtualConnector.empty() &&
                              input::pickWaylandRects(
                                  outputs, m_VirtualConnector, wl.display.left, wl.display.top,
                                  wl.display.right, wl.display.bottom, wl.desktop.left,
                                  wl.desktop.top, wl.desktop.right, wl.desktop.bottom);
                if (placed)
                    name = m_VirtualConnector;
                else
                    placed = input::findWaylandOutputAt(outputs, wl.display.left, wl.display.top,
                                                        wl.display.right, wl.display.bottom, name);
                if (!placed && wl.display.left == 0 && wl.display.top == 0) {
                    const input::WaylandOutput* only = nullptr;
                    for (const input::WaylandOutput& out : outputs) {
                        if (out.width != wl.display.right || out.height != wl.display.bottom)
                            continue;
                        only = only ? nullptr : &out;
                        if (!only) break;
                    }
                    if (only) {
                        wl.display = {only->x, only->y, only->x + only->width,
                                      only->y + only->height};
                        name = only->name;
                        placed = true;
                    }
                }
                if (input::waylandDesktopUnion(outputs, wl.desktop.left, wl.desktop.top,
                                               wl.desktop.right, wl.desktop.bottom)) {
                    rects = wl;
                    log::info("[native] input: pointer mapped on the Wayland layout (" +
                              socketUsed + "): portal display " + rectText(rects.display) +
                              (placed ? " = " + name : std::string(" (no output matches it)")) +
                              ", desktop " + rectText(rects.desktop));
                }
            } else if (input::pickWaylandRects(outputs, m_ConnectorName, wl.display.left,
                                               wl.display.top, wl.display.right, wl.display.bottom,
                                               wl.desktop.left, wl.desktop.top, wl.desktop.right,
                                               wl.desktop.bottom)) {
                rects = wl;
                log::info("[native] input: pointer mapped on the Wayland layout (" + socketUsed +
                          "): display " + rectText(rects.display) + ", desktop " +
                          rectText(rects.desktop));
            } else {
                log::info("[native] input: Wayland layout read (" + socketUsed +
                          ") but no output is named " + m_ConnectorName +
                          " — pointer mapped on the KMS layout");
            }
        } else {
            log::info("[native] input: no Wayland layout: " + why +
                      " — pointer mapped on the KMS layout");
        }
        return rects;
    }

    /// An X11 session: X stretches an absolute device over its whole root,
    /// every GPU's monitors included, and only RandR knows where a monitor of
    /// a second GPU sits (X11Layout.h — the centre of the picture sent the
    /// pointer to 2241,720 instead of 3520,540 on the UM790Pro, 30/09/2026).
    /// The captured display is found among X's outputs by what the driver
    /// read from KMS; the portal route's by the place the portal named, which
    /// GNOME's X11 backend gives in root coordinates.
    /// False, @p rects untouched, when there is no X server to ask — a Wayland
    /// session, a headless host — or the display is not recognised.
    bool readX11Rects(const input::KmsIdentity& identity, InputRects& rects) const
    {
        std::vector<input::X11Output> outputs;
        int rootWidth = 0;
        int rootHeight = 0;
        std::string displayUsed;
        std::string why;
        if (!input::X11Layout::read(outputs, rootWidth, rootHeight, displayUsed, why)) {
            log::debug("[native] input: no X layout: " + why);
            return false;
        }
        const capture::DesktopRect root{0, 0, rootWidth, rootHeight};
        const auto rectOf = [](const input::X11Output& out) {
            return capture::DesktopRect{out.x, out.y, out.x + out.width, out.y + out.height};
        };

        if (m_Target.capture == CaptureApi::PipeWire) {
            InputRects x = rects;
            int index = input::findX11OutputAt(outputs, x.display.left, x.display.top,
                                               x.display.right, x.display.bottom);
            // A portal that named no place left its picture at the origin; the
            // one output of that size, when there is one, is then its monitor.
            if (index < 0 && x.display.left == 0 && x.display.top == 0) {
                index = input::findX11OutputSized(outputs, x.display.right, x.display.bottom);
                if (index >= 0) x.display = rectOf(outputs[index]);
            }
            x.desktop = root;
            rects = x;
            log::info("[native] input: pointer mapped on the X layout (\"" + displayUsed +
                      "\"): portal display " + rectText(rects.display) +
                      (index >= 0 ? " = " + outputs[index].name
                                  : std::string(" (no output matches it)")) +
                      ", root " + rectText(rects.desktop));
            return true;
        }

        std::string how;
        const int index = input::findX11Output(outputs, identity, how);
        if (index < 0) {
            log::info("[native] input: X layout read (\"" + displayUsed + "\", " +
                      std::to_string(outputs.size()) + " outputs) but none is recognised as " +
                      m_ConnectorName + " — pointer mapped on the KMS layout");
            return false;
        }
        rects.display = rectOf(outputs[index]);
        rects.desktop = root;
        log::info("[native] input: pointer mapped on the X layout (\"" + displayUsed +
                  "\"): " + m_ConnectorName + " is X's " + outputs[index].name + " (by " + how +
                  ") at " + rectText(rects.display) + ", root " + rectText(rects.desktop));
        return true;
    }

    static std::string rectText(const capture::DesktopRect& r)
    {
        return std::to_string(r.left) + "," + std::to_string(r.top) + " " +
               std::to_string(r.right - r.left) + "x" + std::to_string(r.bottom - r.top);
    }

    static void applyInputRects(input::IInputSink& sink, const InputRects& rects)
    {
        sink.setDisplayRect(rects.display.left, rects.display.top, rects.display.right,
                            rects.display.bottom);
        sink.setDesktopRect(rects.desktop.left, rects.desktop.top, rects.desktop.right,
                            rects.desktop.bottom);
    }

    /// Converter and encoder against what the capture is handing out right
    /// now — the pair the Selector chose, not one guessed from the display.
    bool buildPipeline(int outputWidth, int outputHeight, std::string& error)
    {
        m_Pipeline.reset();
        // ⚠️ The portal may hand over SHARED MEMORY rather than a DMA-BUF —
        // which compositor and which driver decides, not us. EGL cannot import
        // that: the Vulkan conversion can (C13.10), where the vendor table or
        // the bench asks for it, and the CPU pair otherwise
        // (LinuxRouteChoice.h). Deciding here rather than at selection time
        // because the answer is not known until the stream has negotiated.
        const bool sharedMemory = m_Target.capture == CaptureApi::PipeWire && !m_PortalDmabuf;
        if (sharedMemory && m_Target.encoder != EncoderApi::Software && !m_LoggedSharedMemory) {
            m_LoggedSharedMemory = true;
            log::info("[native] the portal gives shared memory, not DMA-BUF — GL cannot read it: "
                      "the Vulkan conversion or the CPU pair does");
        }
        // The chain, chosen again for every build (LinuxRouteChoice.h): what
        // failed before is a refusal the choice now carries, so each pass
        // below can only move down — Vulkan → GL, VA-API → the CPU — and the
        // loop ends on a pair that came up or on the CPU's own failure.
        for (;;) {
            LinuxRouteFacts facts = routeFacts(sharedMemory);
#if defined(MW_NATIVE_LINUX_VULKAN)
            // Vulkan Video only on the pixel's word (VulkanHevcProof), asked
            // only where the chain would be taken.
            if (linuxRouteWantsVulkanVideo(facts))
                facts.vulkanEncoderRefusal = proofRefusal(outputWidth, outputHeight);
#endif
            m_Route = chooseLinuxRoute(facts);
            // ⚠️ A codec only Vulkan Video encodes (AV1, C13.12), and the chain
            // will not carry it. Before the stream: the client's next codec
            // the GPU encodes without it, chosen again from the top — its own
            // chain may well be Vulkan Video. While streaming: nothing here
            // encodes AV1 another way, and a stream cannot change its codec,
            // so it ends, saying why.
            if (linuxVulkanOnlyCodec(m_SessionCodec) &&
                m_Route.encoder != LinuxRoute::Encoder::Vulkan) {
                const std::string why =
                    std::string(toString(m_SessionCodec)) +
                    " runs through Vulkan Video alone here, which cannot: " + m_Route.reason;
                if (m_PairBuilt) {
                    error = why + " — and the stream cannot change its codec";
                    return false;
                }
                Codec next = m_SessionCodec;
                for (Codec c : m_Config.clientCodecs)
                    if (std::find(m_Target.codecsWithoutOffer.begin(),
                                  m_Target.codecsWithoutOffer.end(),
                                  c) != m_Target.codecsWithoutOffer.end()) {
                        next = c;
                        break;
                    }
                if (next == m_SessionCodec) {
                    error = why + "; the client decodes nothing else this GPU encodes";
                    return false;
                }
                log::info("[native] " + why + " — streaming " + toString(next) +
                          ", the client's next codec");
                m_SessionCodec = next;
                continue;
            }
            m_UsingCpuPair = m_Route.encoder == LinuxRoute::Encoder::Cpu;
            if (buildPair(outputWidth, outputHeight, error)) break;
            // The Vulkan Video chain did not come up — a device, an encoder
            // or a conversion that refused: VA-API encodes, as it always has.
            if (m_Pipeline && m_Pipeline->vulkanChainGivenUp()) {
                m_VulkanEncoderRefusal = "the Vulkan Video chain could not start (" + error + ")";
                log::warning("[native] " + m_VulkanEncoderRefusal + " — encoding through VA-API");
                continue;
            }
            // The split route's Vulkan half did not come up: no loader, no
            // Vulkan 1.3, a modifier it cannot import. GL converts instead, as
            // it always has — never a stream refused for want of Vulkan.
            if (m_Pipeline && m_Pipeline->conversionGivenUp()) {
                m_VulkanConvertRefusal = "the Vulkan conversion could not start (" + error + ")";
                log::warning("[native] " + m_VulkanConvertRefusal + " — " + convertingNext(false));
                continue;
            }
            // ⚠️ The one hardware failure worth surviving: a driver that encodes
            // but writes no VPS/SPS/PPS (a Radeon 610M does exactly that — see
            // VaapiEncoder.h). Nothing downstream can work around it, and the
            // CPU pair always can, so take that road rather than end the stream
            // on a machine whose only fault is its driver. Remembered, so the
            // rebuilds the load cap asks for do not pay for the discovery again.
            if (!m_UsingCpuPair &&
                error.find(encode::VaapiEncoder::kNoParameterSets) != std::string::npos) {
                log::info("[native] " + error +
                          ". Encoding on the CPU instead, which writes its own");
                m_GpuEncoderUnusable = true;
                continue;
            }
            return false;
        }
        if (m_Route.route != m_LoggedRoute) {
            m_LoggedRoute = m_Route.route;
            log::info("[native] route: " + m_Route.route + " (" + m_Route.reason + ")");
        }
        noteRoute();
        m_PairBuilt = true;
        m_PipelineCaptureWidth = m_Capture->width();
        m_PipelineCaptureHeight = m_Capture->height();
        return true;
    }

    /// Where the picture goes once the Vulkan conversion is out: GL — but GL
    /// cannot read the portal's shared memory, and the CPU pair takes it then.
    std::string convertingNext(bool fromHere) const
    {
        const std::string when = fromHere ? " from here" : "";
        return m_Target.capture == CaptureApi::PipeWire && !m_PortalDmabuf
                   ? "the CPU pair" + when + ", as GL cannot read the portal's shared memory"
                   : "converting through EGL" + when;
    }

    /// What this build of the session knows when it picks its chain.
    LinuxRouteFacts routeFacts(bool sharedMemory) const
    {
        LinuxRouteFacts f;
        f.benchKey = m_Config.tuning.pipeline;
        f.setting = m_Config.videoPipeline;
        f.convertKey = m_Config.tuning.convertLinux;
        f.encoder = m_Target.encoder;
        f.codec = m_SessionCodec;
        f.vendorId = m_VendorId;
        f.portal = m_Target.capture == CaptureApi::PipeWire;
        f.sharedMemory = sharedMemory;
        f.vaapiUnusable = m_GpuEncoderUnusable;
#if defined(MW_NATIVE_LINUX_VULKAN)
        f.vulkanConvertBuilt = true;
        f.vulkanEncoderBuilt = true;
#endif
        f.vulkanConvertRefusal = m_VulkanConvertRefusal;
        f.vulkanEncoderRefusal = m_VulkanEncoderRefusal;
        return f;
    }

#if defined(MW_NATIVE_LINUX_VULKAN)
    /// Why this GPU's Vulkan Video encoder is not trusted at this size, or "":
    /// the pixel proof's verdict, kept for the session per size (and across
    /// sessions in the user's cache, VulkanHevcProof).
    std::string proofRefusal(int outputWidth, int outputHeight)
    {
        const int width = outputWidth > 0 ? outputWidth : m_Capture->width();
        const int height = outputHeight > 0 ? outputHeight : m_Capture->height();
        if (!m_ProofDone || width != m_ProofWidth || height != m_ProofHeight ||
            m_SessionCodec != m_ProofCodec) {
            const encode::VulkanHevcProof proof =
                m_SessionCodec == Codec::Av1
                    ? encode::vulkanAv1Verdict(m_Capture->renderNodePath(), width, height,
                                               m_EncodeFps, m_Config.tuning)
                    : encode::vulkanHevcVerdict(m_Capture->renderNodePath(), width, height,
                                                m_EncodeFps, m_Config.tuning);
            m_ProofDone = true;
            m_ProofWidth = width;
            m_ProofHeight = height;
            m_ProofCodec = m_SessionCodec;
            m_ProofRefusal = proof.passed ? std::string()
                             : proof.ran  ? "the pixel proof failed: " + proof.summary
                                          : "the pixel proof could not run: " + proof.summary;
        }
        return m_ProofRefusal;
    }
#endif

    /// The chain in SessionInfo — the log's line, the stats overlay, the
    /// bench's rows. After every build: a fallback changes it mid-stream.
    void noteRoute()
    {
        m_Info.videoPipeline = m_Route.pipeline;
        m_Info.videoRoute = m_Route.route;
        m_Info.videoPipelineReason = m_Route.reason;
        m_Info.videoPipelineRefused = m_Route.refused;
    }

    /// The picture through the pair; and when a Vulkan half gives up while
    /// streaming — its device lost, a buffer it cannot import — the same
    /// picture again through the pair a step down, rebuilt at the same size.
    /// Possibly twice: on AMD the Vulkan Video chain's step down is the split
    /// route (§9-20), whose import may refuse the same buffer. Each step only
    /// moves down — the chain, the split route, GL — so this ends on a pair
    /// that converted or on GL's own failure. For the rest of the session:
    /// m_RouteChanged tells the loop to put its bitrate back on the new
    /// encoder, and a keyframe starts it.
    bool convertPicture(const capture::KmsFrame& frame, const capture::CursorState& cursor,
                        std::string& error)
    {
        while (!m_Pipeline->convert(frame, cursor, cursorDraw(), error))
            if (!leaveVulkan(error)) return false;
        return true;
    }

    /// A Vulkan half gave up while streaming — the Vulkan Video chain, or the
    /// split route's conversion: remembered as a refusal for the rest of the
    /// session, and the pair rebuilt at the same size with the next chain
    /// down. False, @p error untouched, for any other failure. The loop puts
    /// its bitrate back on the new encoder (m_RouteChanged); a keyframe
    /// starts it.
    bool leaveVulkan(std::string& error)
    {
        if (m_Pipeline->vulkanChainGivenUp()) {
            m_VulkanEncoderRefusal =
                "the Vulkan Video chain gave up while streaming (" + error + ")";
            log::warning(
                "[native] " + m_VulkanEncoderRefusal +
                (linuxVulkanOnlyCodec(m_SessionCodec)
                     ? std::string(" — and nothing else here encodes ") + toString(m_SessionCodec)
                     : std::string(" — encoding through VA-API from here")));
        } else if (m_Pipeline->conversionGivenUp()) {
            m_VulkanConvertRefusal =
                "the Vulkan conversion gave up while streaming (" + error + ")";
            log::warning("[native] " + m_VulkanConvertRefusal + " — " + convertingNext(true));
        } else {
            return false;
        }
        m_Pipeline->detachThread();
        std::string rebuildError;
        if (!buildPipeline(m_Info.width, m_Info.height, rebuildError)) {
            error = "no pair after Vulkan gave up: " + rebuildError;
            return false;
        }
        noteRoute();
        m_RouteChanged = true;
        m_ForceKeyframe.store(true);
        // A new encoder holds no reconstructions: a loss named against the
        // old one means nothing.
        m_PendingInvalidation.store(0);
        return true;
    }

    /// The pair itself, once m_UsingCpuPair is settled: pick the codec that pair
    /// can produce, build it, start it. Separate because it is run twice when a
    /// GPU encoder turns out to be unusable.
    bool buildPair(int outputWidth, int outputHeight, std::string& error)
    {
        m_Pipeline.reset();
        // ⚠️ The codec has to follow the pair. The Selector picked HEVC because
        // the GPU offers it, and it was right about the GPU — but a pair that
        // encodes on the CPU encodes with OpenH264, which does H.264 and
        // nothing else. Left alone, a browser that prefers HEVC (Chrome does)
        // gets "OpenH264 encodes H.264 only" and no session at all: measured on
        // 08/09/2026, the first real browser session through the portal.
        //
        // Not a decision that could have been taken at selection time, for the
        // same reason the pair could not: whether the compositor hands over a
        // DMA-BUF or shared memory is known only once the stream has
        // negotiated, and on a DMA-BUF the Selector's HEVC is exactly right.
        m_Codec = m_SessionCodec;
        if (m_UsingCpuPair && m_Codec != Codec::H264) {
            // Asked, not assumed. Every browser decodes H.264 and the list is
            // never empty here (the Selector rejects that before a session
            // exists), but a route that silently sends a codec the client did
            // not name is how a black picture with no error happens.
            const bool clientTakesH264 =
                std::find(m_Config.clientCodecs.begin(), m_Config.clientCodecs.end(),
                          Codec::H264) != m_Config.clientCodecs.end();
            if (!clientTakesH264) {
                error = std::string("this route encodes on the CPU, which can only produce H.264, "
                                    "and the client asked for ") +
                        toString(m_Codec) + " without it";
                return false;
            }
            if (!m_LoggedCodecDowngrade) {
                m_LoggedCodecDowngrade = true;
                log::info(std::string("[native] ") + toString(m_Codec) +
                          " was chosen for the GPU, but this route encodes on the CPU — "
                          "streaming H.264, which is what OpenH264 produces");
            }
            m_Codec = Codec::H264;
        }

        if (m_UsingCpuPair) m_Pipeline = std::make_unique<CpuPipeline>();
#if defined(MW_NATIVE_LINUX_VULKAN)
        else if (m_Route.encoder == LinuxRoute::Encoder::Vulkan && m_Codec == Codec::Av1)
            m_Pipeline = std::make_unique<VulkanPipeline<encode::VulkanAv1Encoder>>(
                m_Config.clientCropsToFrame);
        else if (m_Route.encoder == LinuxRoute::Encoder::Vulkan)
            m_Pipeline = std::make_unique<VulkanPipeline<encode::VulkanHevcEncoder>>();
        else if (m_Route.conversion == LinuxRoute::Conversion::Vulkan)
            m_Pipeline = std::make_unique<VaapiPipeline<convert::VulkanConvert>>();
#endif
        else
            m_Pipeline = std::make_unique<GpuPipeline>();
        return m_Pipeline->init(*m_Capture, m_Codec, outputWidth, outputHeight, m_EncodeFps,
                                m_Config.bitrateKbps, m_Config.intraRefresh, m_Config.tuning,
                                error);
    }

    convert::CursorDraw cursorDraw() const
    {
        convert::CursorDraw draw;
        const int wanted = m_CursorFramePx.load();
        const capture::CursorState& cursor = m_Capture->cursor();
        // Sized on the ink, not the canvas: see CursorState::inkWidth. Scaled
        // by the frame/desktop ratio so the request is in frame pixels.
        if (wanted > 0 && cursor.inkWidth > 0 && m_Capture->width() > 0 && m_Pipeline &&
            m_Pipeline->outputWidth() > 0) {
            const float desktopPerFrame = static_cast<float>(m_Capture->width()) /
                                          static_cast<float>(m_Pipeline->outputWidth());
            const float target = static_cast<float>(wanted) * desktopPerFrame;
            const float magnify = target / static_cast<float>(cursor.inkWidth);
            if (magnify > 1.0f) draw.magnify = magnify;
        }
        // The hotspot where the capture knows it (a virtual machine's cursor
        // plane carries one); 0,0 elsewhere, so the image grows around its
        // top-left, which for the arrow IS the hotspot.
        draw.hotspotX = cursor.hotspotX;
        draw.hotspotY = cursor.hotspotY;
        return draw;
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
            if (openCapture(error)) break;
            failures++;
            if (failures == 1)
                log::info("[native] display is away (reconfiguring, or off), waiting for it: " +
                          error);
            std::this_thread::sleep_for(std::chrono::milliseconds(restartRetryDelayMs(failures)));
        }
        if (failures > 0)
            log::info("[native] display is back after " + std::to_string(failures) + " attempt" +
                      (failures > 1 ? "s" : ""));
        m_DisplayMilliHz = m_Capture->refreshMilliHz();
        if (!rebuildForCapture(error)) return Restart::Failed;
        return Restart::Restarted;
    }

    /// Rebuild everything behind the capture for the size it delivers now —
    /// after a restart, or when the portal renegotiated a new size under a
    /// running stream (see the loop). The capture itself is left alone.
    bool rebuildForCapture(std::string& error)
    {
        // The frame keeps its size unless the viewer follows the display's
        // shape and the mode change moved it — see SessionConfig::
        // followDisplayShape, and the Windows session, which does the same.
        // Followed or not, it is never larger than the display.
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
            const FrameSize base = m_Config.width > 0 && m_Config.height > 0
                                       ? FrameSize{m_Config.width, m_Config.height}
                                       : full;
            full = frameForDisplay(display, base, policyOf(m_Config));
            if (full.width != m_FullWidth || full.height != m_FullHeight) {
                log::info("[native] the display is now " + std::to_string(m_Capture->width()) +
                          "x" + std::to_string(m_Capture->height()) + " — the stream follows it: " +
                          std::to_string(m_FullWidth) + "x" + std::to_string(m_FullHeight) +
                          " -> " + std::to_string(full.width) + "x" + std::to_string(full.height));
                frameWidth = encode::EncodeLoadCap::scaled(full.width, m_LoadCap.percent());
                frameHeight = encode::EncodeLoadCap::scaled(full.height, m_LoadCap.percent());
            }
        }
        if (!buildPipeline(frameWidth, frameHeight, error)) {
            if (frameWidth == m_Info.width && frameHeight == m_Info.height) return false;
            log::warning("[native] cannot encode at " + std::to_string(frameWidth) + "x" +
                         std::to_string(frameHeight) + " (" + error + ") — staying at " +
                         std::to_string(m_Info.width) + "x" + std::to_string(m_Info.height));
            if (!buildPipeline(m_Info.width, m_Info.height, error)) return false;
        } else {
            m_FullWidth = full.width;
            m_FullHeight = full.height;
        }
        m_Info.width = m_Pipeline->outputWidth();
        m_Info.height = m_Pipeline->outputHeight();
        // A new encoder holds no reconstructions: a loss named against the old
        // one means nothing, and the first picture is a keyframe regardless.
        m_PendingInvalidation.store(0);
        {
            const InputRects rects = readInputRects();
            std::lock_guard<std::mutex> lock(m_InputMutex);
            if (m_Input) applyInputRects(*m_Input, rects);
        }
        m_ResendCursor.store(true);
        reportDisplayFormat();
        return true;
    }

    /// Tell the viewer what the display became — only when its size or the
    /// frame's moved. See DisplayFormat; HDR never changes here.
    void reportDisplayFormat()
    {
        const DisplayFormat format{m_Capture->width(),
                                   m_Capture->height(),
                                   m_Info.width,
                                   m_Info.height,
                                   false,
                                   false,
                                   false};
        std::lock_guard<std::mutex> lock(m_FormatMutex);
        if (format.displayWidth == m_LastFormat.displayWidth &&
            format.displayHeight == m_LastFormat.displayHeight &&
            format.frameWidth == m_LastFormat.frameWidth &&
            format.frameHeight == m_LastFormat.frameHeight)
            return;
        m_LastFormat = format;
        log::info("[native] display format: " + std::to_string(format.displayWidth) + "x" +
                  std::to_string(format.displayHeight) + ", streaming " +
                  std::to_string(format.frameWidth) + "x" + std::to_string(format.frameHeight));
        if (m_OnDisplayFormat) m_OnDisplayFormat(format);
    }

    void setDisplayFormatCallback(DisplayFormatCallback callback) override
    {
        std::lock_guard<std::mutex> lock(m_FormatMutex);
        m_OnDisplayFormat = std::move(callback);
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
        // The EGL context followed this thread; give it back so stop(), on
        // the caller's thread, can bind it to tear the converter down.
        if (m_Pipeline) m_Pipeline->detachThread();
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
        // Whether the frame the capture last handed out is still valid to
        // re-convert (it is until the next export, see KmsCapture::acquire).
        bool haveFrame = false;
        constexpr int64_t kModeCheckUs = 1000 * 1000;
        int64_t nextModeCheckUs = steadyNowUs() + kModeCheckUs;
        // A mode change seen under the portal and not settled yet: when it was
        // seen, the modes it moved to, and the frames the stream has delivered
        // since. See the check in the loop.
        constexpr int64_t kPortalFollowGraceUs = 2000 * 1000;
        constexpr int kPortalAliveFrames = 5;
        int64_t portalModeSeenUs = 0;
        std::string portalModesSeen;
        int portalFramesSince = 0;
        bool reopenPortal = false;
        // The shared monitor's record, read this often by a guest's stream;
        // GNOME's word on its monitors, asked this often, and the pointer
        // mapped again this long after it.
        [[maybe_unused]] constexpr int64_t kShareCheckUs = 1000 * 1000;
        [[maybe_unused]] int64_t nextShareCheckUs = steadyNowUs() + kShareCheckUs;
        [[maybe_unused]] constexpr int64_t kLayoutCheckUs = 250 * 1000;
        [[maybe_unused]] constexpr int64_t kRemapDelayUs = 300 * 1000;
        [[maybe_unused]] int64_t nextLayoutCheckUs = 0;
        [[maybe_unused]] int64_t remapAtUs = 0;
        capture::KmsFrame frame;

        auto floorIntervalUs = [this, kIdleFloorUs]() -> int64_t {
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
            if (!m_Pipeline->setBitrate(effective.scaledKbps(kbps), error))
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
        // Re-convert the held frame with the pointer where it is now, and
        // emit. The KMS equivalent of the Windows desktop copy, without one.
        // The pair was rebuilt under the loop — the split route's Vulkan
        // conversion gave up, GL converts now (convertPicture): the new
        // encoder takes the bitrate the loop had set.
        auto followRouteChange = [&]() {
            if (!m_RouteChanged) return;
            m_RouteChanged = false;
            boosted = false;
            applyBitrate(baseKbps);
        };
        auto reconvertHeld = [&](const FrameStamps& stamps) -> bool {
            static const capture::CursorState kNoPointer;
            if (!convertPicture(frame, drawsPointer() ? m_Capture->cursor() : kNoPointer, error)) {
                // The portal reopened for shared memory at the next turn.
                if (leaveDmabuf(error)) {
                    haveFrame = false;
                    reopenPortal = true;
                    return true;
                }
                finish("colour conversion failed: " + error);
                return false;
            }
            followRouteChange();
            return emitPicture(stamps);
        };

        m_LoopStartUs = steadyNowUs();
        m_LoadCap.start(m_LoopStartUs);
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
            const int timeoutMs = refineSoon ? refineTimeoutMs : idleTimeoutMs;

            // Between frames, so the encoder is not holding anything.
            //
            // A new pipeline has converted nothing: its encoder reads a zeroed
            // picture, which is flat green once decoded (Y = U = V = 0). On a
            // still screen no capture comes to fill it, and the keyframe the
            // resize asks for, the proactive one after it and every floor pass
            // were encoded from that — green until the screen next moved (issue
            // #15, reproduced 15/09/2026 on an AMD client: 1 820-byte keyframes
            // at 960x600 that every decoder faithfully painted green). The held
            // picture goes in first.
            if (m_PendingResize.exchange(false) && applyLoadCap() && haveFrame) {
                if (!reconvertHeld(resendStamps(steadyNowUs()))) return;
            }
            // Same for the pair VA-API brought in when the Vulkan Video chain
            // gave up encoding (emit).
            if (m_ReconvertHeld) {
                m_ReconvertHeld = false;
                if (haveFrame && !reconvertHeld(resendStamps(steadyNowUs()))) return;
            }

            // The portal route: a mode change may never reach the stream. GNOME
            // 42 simply stops delivering frames at one — measured on the
            // UM790Pro, 15/09/2026: the desktop at 1280x960 on the scanout, not
            // one frame out of the portal — where a fresh portal session opens
            // on the new mode without a dialog, replaying the grant. KWin
            // (Plasma 5.24, same machine, same day) does the opposite: its
            // stream renegotiates the new size in place, and its portal keeps
            // no grant, so a reopen raised the dialog again and froze the
            // stream until someone AT the host clicked Share. So the CRTC modes
            // are watched, and a change is given a moment to reach the stream:
            // one that keeps delivering (a new size, or a few frames) is left
            // alone; one that falls silent is handled as a loss and reopened.
            //
            // Not on a virtual display: it has no CRTC, and a screen's mode
            // changing beside it is nothing to its stream — which a static
            // desktop leaves silent, and the wait below would have reopened,
            // remaking the very monitor a guest records.
            bool portalModeChanged = false;
            if (m_Target.capture == CaptureApi::PipeWire && !m_Target.portalVirtual &&
                steadyNowUs() >= nextModeCheckUs) {
                nextModeCheckUs = steadyNowUs() + kModeCheckUs;
                const std::string modes = capture::KmsCapture::modeSignature(m_CardPath);
                if (!modes.empty() && !m_PortalModes.empty() && modes != m_PortalModes &&
                    modes != portalModesSeen) {
                    log::info("[native] a display mode changed under the portal — waiting for its "
                              "stream to follow");
                    portalModeSeenUs = steadyNowUs();
                    portalModesSeen = modes;
                    portalFramesSince = 0;
                }
            }
            if (portalModeSeenUs != 0 && steadyNowUs() - portalModeSeenUs >= kPortalFollowGraceUs) {
                log::info("[native] the portal stream went silent after the mode change — "
                          "reopening it");
                portalModeSeenUs = 0;
                portalModesSeen.clear();
                portalModeChanged = true;
            }
#if defined(MW_NATIVE_LINUX_PORTAL)
            // A guest's stream shows the desktop's shared monitor (SharedMonitor.h):
            // when another took its place — the owner's stream came — or the one
            // it records lost its maker, it starts over on the one there is now.
            if (m_OnMutter && m_Target.portalVirtual && !m_Config.virtualPrimary &&
                steadyNowUs() >= nextShareCheckUs) {
                nextShareCheckUs = steadyNowUs() + kShareCheckUs;
                capture::SharedMonitor now;
                capture::readSharedMonitor(now);
                if (capture::sharedMonitorMoved(false, m_SharedMade, m_SharedShown, now)) {
                    log::info("[native] virtual display: the desktop's shared monitor is now " +
                              (now.valid() ? now.connector + ", the " +
                                                 (now.owner ? "owner's" : "first guest's")
                                           : std::string("none")) +
                              " — this guest's stream starts over on it");
                    reopenPortal = true;
                }
            }
            // GNOME's monitors changed beside the virtual display — another
            // stream's came or went, a screen was plugged — and with them the
            // desktop an absolute pointer spans: mapped again once Mutter is
            // done, a moment after its word.
            if (m_LayoutWatch && steadyNowUs() >= nextLayoutCheckUs) {
                nextLayoutCheckUs = steadyNowUs() + kLayoutCheckUs;
                if (m_LayoutWatch->changed()) remapAtUs = steadyNowUs() + kRemapDelayUs;
            }
            // Still here: the gamescope session's ten minutes start over.
            if (m_OnGamescope && steadyNowUs() >= m_NextGamescopeKeepUs) keepGamescope();
            if (remapAtUs != 0 && steadyNowUs() >= remapAtUs) {
                remapAtUs = 0;
                const InputRects rects = readInputRects();
                std::lock_guard<std::mutex> lock(m_InputMutex);
                if (m_Input) applyInputRects(*m_Input, rects);
            }
#endif
            if (reopenPortal) {
                reopenPortal = false;
                portalModeChanged = true;
            }

#if defined(MW_NATIVE_LINUX_PORTAL)
            followPointerForDmabuf();
#endif
            capture::KmsFrame fresh;
            const int64_t acquireStartUs = steadyNowUs();
            const capture::AcquireStatus status = portalModeChanged
                                                      ? capture::AcquireStatus::Lost
                                                      : m_Capture->acquire(timeoutMs, fresh);
            if (m_Config.tuning.clickTrace && !portalModeChanged)
                traceCapture(status, fresh, acquireStartUs, steadyNowUs());

            if (status != capture::AcquireStatus::Timeout && boosted) {
                boosted = false;
                applyBitrate(baseKbps);
            }

            reportCursor();
            reportCursorPosition();

            if (status == capture::AcquireStatus::Timeout) {
                if (m_CursorDirty.exchange(false) && drawsPointer() && haveFrame) {
                    const int64_t now = steadyNowUs();
                    if (!reconvertHeld(resendStamps(now))) return;
                    continue;
                }
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

            if (status == capture::AcquireStatus::PointerOnly) {
                if (!drawsPointer() || !haveFrame) continue;
                m_CursorDirty.store(false);
                const int64_t submittedUs = steadyNowUs();
                if (!reconvertHeld(
                        FrameStamps{submittedUs, submittedUs, submittedUs, steadyNowUs()}))
                    return;
                continue;
            }

            if (status == capture::AcquireStatus::Lost) {
                haveFrame = false;
                closeBurst("display lost");
#if defined(MW_NATIVE_LINUX_PORTAL)
                // gamescope goes when its app quits — Steam left Big Picture
                // for good. Nothing to reopen: starting it again would bring
                // back what the viewer just closed.
                if (m_OnGamescope && !capture::gamescopeSessionAlive(m_Gamescope)) {
                    finish(m_GamescopeApp + " quit, and its gamescope with it");
                    return;
                }
#endif
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

#if defined(MW_NATIVE_LINUX_PORTAL)
            // The buffers changed kind under a renegotiation of ours
            // (followPointerForDmabuf). The Vulkan conversion reads both,
            // frame by frame: on a chain that stays the same, nothing is
            // built again and the encoder goes on — the switch costs only
            // Mutter's renegotiation. Another chain (GL reads no shared
            // memory, the CPU pair no DMA-BUF) is rebuilt at the same size,
            // and a keyframe starts the new encoder.
            if (m_PortalCapture && m_PortalCapture->dmabuf() != m_PortalDmabuf) {
                m_PortalDmabuf = m_PortalCapture->dmabuf();
                const std::string kind = m_PortalDmabuf ? "DMA-BUF" : "shared memory";
                if (routeHoldsForPortalBuffers()) {
                    log::info("[native] the portal now hands " + kind +
                              " — the same chain reads it");
                } else {
                    log::info("[native] the portal now hands " + kind +
                              " — rebuilding the pair behind it");
                    closeBurst("buffers changed");
                    if (!buildPipeline(m_Info.width, m_Info.height, error)) {
                        finish("the pipeline could not follow the portal's buffers: " + error);
                        return;
                    }
                    noteRoute();
                    m_RouteChanged = true;
                    m_ForceKeyframe.store(true);
                    m_PendingInvalidation.store(0);
                    lastRealUs = steadyNowUs();
                    resetBurst();
                }
            }
#endif
            // The portal renegotiates a new size in place — a resolution
            // change on the compositor's side — where KMS reports the display
            // lost. The frames simply arrive bigger or smaller, and a pipeline
            // built for the old size would import them at the wrong one. Same
            // rebuild as after a restart, minus reopening a capture that is
            // perfectly alive: a new portal session would ask the user again.
            const bool resized =
                fresh.width != m_PipelineCaptureWidth || fresh.height != m_PipelineCaptureHeight;
            // A stream still delivering after a mode change (see the mode check
            // above). At a new size it followed: settled. At its old size it
            // did not — KWin 5.24 keeps its 1920x1080 buffer and paints a
            // 1280x1024 desktop into it, doubled at the right, striped below.
            // With a grant the portal reopens on the new mode without a word;
            // without one a reopen asks at the host, and a stream frozen until
            // someone there clicks is worse than a wrong picture its viewer can
            // put right by setting the mode back. So it is kept, and said.
            if (portalModeSeenUs != 0 && (resized || ++portalFramesSince >= kPortalAliveFrames)) {
                if (resized) {
                    log::info("[native] the portal stream followed the mode change");
                } else if (portalModesSeen == m_PortalOpenedModes) {
                    log::info("[native] the display is back on the mode the portal opened on");
                } else if (!m_PortalToken.empty()) {
                    log::info("[native] the portal stream kept its old size through the mode "
                              "change — reopening it on the grant");
                    reopenPortal = true;
                } else {
                    log::warning("[native] the portal stream kept its old size through the mode "
                                 "change, and there is no grant to reopen it without asking at "
                                 "the host — keeping it; the picture is wrong until the mode "
                                 "is back");
                }
                m_PortalModes = portalModesSeen;
                portalModeSeenUs = 0;
                portalModesSeen.clear();
            }
            if (resized) {
                log::info("[native] the capture now delivers " + std::to_string(fresh.width) + "x" +
                          std::to_string(fresh.height) + " (was " +
                          std::to_string(m_PipelineCaptureWidth) + "x" +
                          std::to_string(m_PipelineCaptureHeight) + ") — rebuilding behind it");
                closeBurst("display resized");
                if (!rebuildForCapture(error)) {
                    finish("the pipeline could not follow the display's new size: " + error);
                    return;
                }
                m_ForceKeyframe.store(true);
                boosted = false;
                applyBitrate(baseKbps);
                lastRealUs = steadyNowUs();
                resetBurst();
            }

            m_PresentsSeen++;
            frame = fresh;
            haveFrame = true;
            const int64_t submittedUs = steadyNowUs();

            static const capture::CursorState kNoCursor;
            const bool composite = drawsPointer();
            m_CursorDirty.store(false);
            if (!convertPicture(frame, composite ? m_Capture->cursor() : kNoCursor, error)) {
                if (leaveDmabuf(error)) {
                    haveFrame = false;
                    reopenPortal = true;
                    continue;
                }
                finish("colour conversion failed: " + error);
                return;
            }
            followRouteChange();
            // Not released: the buffer stays held for the pointer-only path,
            // and acquire() closes it when the next one replaces it.
            if (!emitPicture(
                    FrameStamps{frame.presentUs, frame.capturedUs, submittedUs, steadyNowUs()}))
                return;
        }
    }

    bool emit(uint32_t& frameNumber, const FrameStamps& stamps, std::string& error)
    {
        // Losses are named by the relay thread and applied here, on the thread
        // that owns the encoder — the same shape as the keyframe request, and
        // for the same reason: the reference list is encoder state.
        if (const uint32_t lost = m_PendingInvalidation.exchange(0); lost > 0) {
            std::string why;
            if (m_Pipeline->invalidateReference(lost - 1, why)) {
                log::info("[native] reference invalidated: frame " + std::to_string(lost - 1) +
                          " never reached the receiver, healing with a delta");
            } else {
                log::info("[native] cannot heal frame " + std::to_string(lost - 1) +
                          " with a delta (" + why + ") — sending a keyframe");
                m_ForceKeyframe.store(true);
            }
        }
        const bool forceKeyframe = m_ForceKeyframe.exchange(false);
        encode::EncoderOutput encoded;
        if (!m_Pipeline->encode(forceKeyframe, frameNumber, encoded, error)) {
            // The Vulkan Video chain gave up on this picture: VA-API takes
            // over, and the loop sends the held picture through it first (it
            // has converted nothing yet). Nothing went out under this number.
            if (leaveVulkan(error)) {
                m_ReconvertHeld = true;
                return true;
            }
            finish("encode failed: " + error);
            return false;
        }
        if (encoded.keyframe && !m_LoggedFirstKeyframe) {
            m_LoggedFirstKeyframe = true;
            log::info("[native] first keyframe: " + std::to_string(encoded.size / 1024) + " KB (" +
                      std::to_string(m_Info.width) + "x" + std::to_string(m_Info.height) + ")");
        }
        m_LastEmitBytes = encoded.size;
        m_LastEmitQp = encoded.avgQp;
        m_Link.sent(steadyNowUs(), encoded.size, m_LinkKbps);

        if (encoded.data && encoded.size > 0 && m_Callbacks.onVideo) {
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
            noteEncodeLoad(out, stamps);
        }
        m_Pipeline->releaseOutput();
        return true;
    }

    /// How long the encoder took, given to the cap — on the CPU tier only.
    ///
    /// On a hardware encoder this must stay silent: a few milliseconds against
    /// a frame interval is never the problem, and E4 and the link governor
    /// already own that space. It is the machine with no encoder at all where
    /// the encode duration IS the latency, and where trading pixels for it is
    /// the right bargain (EncodeLoadCap.h says why that way round).
    ///
    /// Only new pictures encoded as deltas count. A keyframe costs several
    /// deltas, and the still-picture refinement re-encodes the held frame at a
    /// raised bitrate, pass after pass — neither is the steady cost of keeping
    /// up. Counted, they resized a 2-core guest within a second of every start
    /// and every resize (1280×800 → 960×600 → 640×400 in two seconds, issue
    /// #15, 13/09/2026): each resize is a keyframe plus a refinement burst,
    /// which read as overload and triggered the next step down.
    void noteEncodeLoad(const EncodedFrame& out, const FrameStamps& stamps)
    {
        if (m_Target.encoder != EncoderApi::Software) return;
        if (out.keyframe || stamps.resend) return;
        const int64_t convertedUs = out.convertedUs;
        const int64_t encodedUs = out.encodedUs;
        if (encodedUs <= convertedUs) return;
        // The STREAM's interval, not the gate's. The gate is off whenever the
        // stream runs at the display's own rate — 60 fps on a 60 Hz screen,
        // the common case — and its interval then reads zero, which the cap
        // takes for "unpaced" and ignores: a machine that could not keep up
        // was never resized at all, and one on a 75 Hz screen was (12/09/2026).
        const int64_t intervalUs = m_Cadence.enabled() ? m_Cadence.intervalUs()
                                   : m_CadenceFps > 0  ? 1000000 / m_CadenceFps
                                                       : 0;
        if (m_LoadCap.note(encodedUs - convertedUs, intervalUs, encodedUs))
            m_PendingResize.store(true);
    }

    /// Rebuild the pipeline at the size the cap now asks for. Called between
    /// frames, never inside emit(): the encoder there is holding a bitstream
    /// the sender has not finished with.
    ///
    /// True when the pipeline was rebuilt — at the new size, or back at the old
    /// one — and so holds no picture yet.
    bool applyLoadCap()
    {
        const int width = encode::EncodeLoadCap::scaled(m_FullWidth, m_LoadCap.percent());
        const int height = encode::EncodeLoadCap::scaled(m_FullHeight, m_LoadCap.percent());
        if (width == m_Info.width && height == m_Info.height) return false;

        std::string error;
        const int wasWidth = m_Info.width;
        const int wasHeight = m_Info.height;
        if (!buildPipeline(width, height, error)) {
            // Keep streaming at the size that worked rather than ending the
            // session over an optimisation: put the old one back, and if even
            // that fails there is nothing left to save.
            log::warning("[native] cpu cap: cannot encode at " + std::to_string(width) + "x" +
                         std::to_string(height) + " (" + error + ") — staying at " +
                         std::to_string(wasWidth) + "x" + std::to_string(wasHeight));
            if (!buildPipeline(wasWidth, wasHeight, error)) {
                finish("colour conversion failed: " + error);
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

    /// The pointer for a client that draws its own. KMS gives the image and no
    /// name; the hotspot only on a virtual machine's cursor plane (see
    /// KmsCapture.h). Elsewhere it is 0,0 and the client places the image by
    /// its top-left, which for the arrow is right and for a crosshair is a few
    /// pixels off.
    /// The pointer drawn into the picture by this session: in gaming mode, and
    /// only where the pictures do not show one already (cursorInPicture).
    bool drawsPointer() const { return m_CompositeCursor.load() && !m_Capture->cursorInPicture(); }

    void reportCursor()
    {
        if (!m_Callbacks.onCursor || m_CompositeCursor.load()) return;
        const capture::CursorState& cursor = m_Capture->cursor();
        // GNOME's own pointer in the pictures (cursorInPicture): the client is
        // told there is none for it to draw, or the viewer sees two, a step
        // apart (UM790Pro, GNOME 46, 01/10/2026).
        const bool painted = m_Capture->cursorInPicture();
        const bool forced = m_ResendCursor.exchange(false);
        if (!forced && cursor.shapeVersion == m_ReportedShape &&
            cursor.visible == m_ReportedVisible && painted == m_ReportedPainted)
            return;
        if (painted && !m_ReportedPainted)
            log::info("[native] cursor: GNOME paints it into every picture of the virtual "
                      "display (before GNOME 48) — the client is told to draw none");
        m_ReportedShape = cursor.shapeVersion;
        m_ReportedVisible = cursor.visible;
        m_ReportedPainted = painted;

        CursorUpdate update;
        update.visible = !painted && cursor.visible && cursor.width > 0 && cursor.height > 0;
        update.width = cursor.width;
        update.height = cursor.height;
        update.hotspotX = cursor.hotspotX;
        update.hotspotY = cursor.hotspotY;
        update.kind = "";
        update.scale =
            (m_Capture->width() > 0 && m_Info.width > 0)
                ? static_cast<float>(m_Info.width) / static_cast<float>(m_Capture->width())
                : 1.0f;
        update.pixels = update.visible ? cursor.pixels.data() : nullptr;
        m_Callbacks.onCursor(update);
    }

    /// Where the pointer is, for a client that draws it without a pointer device
    /// of its own to know. Throttled by the gate — see CursorUpdate::positionOnly
    /// and the Windows session, which this mirrors.
    void reportCursorPosition()
    {
        if (!m_Callbacks.onCursor) return;
        if (m_CompositeCursor.load()) {
            m_PositionGate.reset();
            return;
        }
        const capture::CursorState& cursor = m_Capture->cursor();
        const float scale =
            (m_Capture->width() > 0 && m_Info.width > 0)
                ? static_cast<float>(m_Info.width) / static_cast<float>(m_Capture->width())
                : 1.0f;
        const float fx = static_cast<float>(cursor.x + cursor.hotspotX) * scale;
        const float fy = static_cast<float>(cursor.y + cursor.hotspotY) * scale;
        // Not one to draw when the pictures show it already (reportCursor).
        const bool visible = cursor.visible && !m_Capture->cursorInPicture();
        if (!m_PositionGate.due(visible, static_cast<int>(fx), static_cast<int>(fy), steadyNowUs()))
            return;
        CursorUpdate update;
        update.positionOnly = true;
        update.visible = visible;
        update.x = fx;
        update.y = fy;
        m_Callbacks.onCursor(update);
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
    SleepInhibit m_SleepInhibit;

    std::string m_CardPath;
    uint32_t m_ConnectorId = 0;
    std::string m_ConnectorName;

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

    /// Whichever route is giving us pictures — the scanout reader, or the
    /// portal on a machine that may not read it (IScreenCapture.h).
    std::unique_ptr<capture::IScreenCapture> m_Capture;
    std::unique_ptr<VideoPipeline> m_Pipeline;
    /// An X server's damage on the captured display, for the scanout reader
    /// (X11Damage): shared with the capture, which asks it at each vblank.
    std::shared_ptr<capture::X11Damage> m_Damage;

    /// The bench's click trace (EncoderTuning::clickTrace). Capture thread,
    /// and the input's. Ahead of the sinks, which write to it.
    ClickTrace m_ClickTrace;
    std::mutex m_InputMutex;
    /// uinput on the desktop; libei into gamescope (EiInput.h), whose app no
    /// uinput device reaches.
    std::unique_ptr<input::IInputSink> m_Input;

    /// See setDisplayFormatCallback, and the last format said. Guarded by
    /// m_FormatMutex: set on the consumer's thread, read on the capture thread.
    std::mutex m_FormatMutex;
    DisplayFormatCallback m_OnDisplayFormat;
    DisplayFormat m_LastFormat;
    std::unique_ptr<input::UinputGamepad> m_Gamepad;

#if defined(MW_NATIVE_LINUX_AUDIO)
    std::unique_ptr<audio::PacedOpusSink> m_Audio;
    std::unique_ptr<audio::PipeWireCapture> m_AudioTap;
    /// Releases in its destructor too, so a session torn down without stop()
    /// does not leave the machine silent.
    audio::HostMute m_HostMute;
#endif

    std::thread m_Thread;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_ForceKeyframe{true};
    std::atomic<bool> m_CompositeCursor{true};
    std::atomic<int> m_CursorFramePx{0};
    std::atomic<int> m_FloorFps{0};
    std::atomic<bool> m_CursorDirty{false};
    std::atomic<bool> m_ResendCursor{false};
    std::atomic<int> m_PendingBitrate{0};
    /// The frame the receiver says it never got, plus one; 0 means none.
    std::atomic<uint32_t> m_PendingInvalidation{0};

    /// Whether the portal handed over DMA-BUF (the GPU pair can import it) or
    /// shared memory (only the CPU pair can read it). Meaningless on the KMS
    /// route, which is always DMA-BUF.
    bool m_PortalDmabuf = false;
    /// A portal DMA-BUF that GL could not convert (leaveDmabuf): the portal is
    /// asked for shared memory only, for the rest of the session.
    bool m_PortalShmOnly = false;
#if defined(MW_NATIVE_LINUX_PORTAL)
    /// The DMA-BUF offered to the portal (dmabufOffer), asked once.
    capture::PortalCapture::DmabufOffer m_DmabufOffer;
    bool m_DmabufOfferAsked = false;
    /// m_Capture when it is one of the portal's or Mutter's streams.
    capture::PortalCapture* m_PortalCapture = nullptr;
#endif
    /// The viewer's last pointer move: 0 none yet, 1 absolute (a desktop, a
    /// pointer on the screen), 2 relative (the viewer's pointer locked, which
    /// a game that hides the host's pointer asks for). Input thread → capture
    /// thread (followPointerForDmabuf).
    std::atomic<int> m_ViewerPointer{0};
    /// The portal grant to replay on the next open — the session's own, then
    /// whatever the portal handed back. See openCapture.
    std::string m_PortalToken;
    /// The compositor's name for the virtual monitor of this session — GNOME's
    /// "Meta-0" (placeVirtualMonitor), KWin's "Virtual-<name>"; empty when there
    /// is none, or it is not known.
    std::string m_VirtualConnector;
    /// The name KWin's virtual output was asked under (KDE Plasma 6); empty
    /// when the portal makes the monitor, or there is none.
    std::string m_KwinOutputName;
#if defined(MW_NATIVE_LINUX_PORTAL)
    /// The capture is one of GNOME's own screen casts (C2): the shared
    /// monitor's rules apply to it (SharedMonitor.h).
    bool m_OnMutter = false;
    /// It made a monitor rather than recording another stream's — which its
    /// end removes, under the desktop's lock.
    bool m_MadeMonitor = false;
    /// That monitor is the desktop's shared one: the owner's stream's, or a
    /// guest's with nothing to record.
    bool m_SharedMade = false;
    /// The shared monitor it shows: the record it made, or the one it
    /// records. Invalid off that route, and for a guest's monitor of its own.
    capture::SharedMonitor m_SharedShown;
    /// Why GNOME's own screen cast is not asked again this session — Mutter
    /// turned it down outright, the portal makes the display — or "".
    std::string m_MutterRefusal;
    /// GNOME's word that its monitors changed, for the pointer's mapping;
    /// on that route only, and kept across restarts. The capture thread's.
    std::unique_ptr<capture::MutterLayoutWatch> m_LayoutWatch;
    /// The capture is an app's own gamescope (openGamescope): this session,
    /// whether this stream started it, and when its timer is next re-armed.
    bool m_OnGamescope = false;
    bool m_GamescopeStarted = false;
    capture::GamescopeRecord m_Gamescope;
    /// What runs in it, for the line that ends the session: "Steam".
    std::string m_GamescopeApp;
    int64_t m_NextGamescopeKeepUs = 0;
    /// How long a gamescope session outlives its last stream (Bruno,
    /// 01/10/2026), and how often a stream on it says it is still there.
    static constexpr int kGamescopeLingerS = 600;
    static constexpr int64_t kGamescopeKeepUs = 60 * 1000 * 1000;
#endif
    /// The CRTC modes when the portal was opened (KmsCapture::modeSignature).
    std::string m_PortalModes;
    /// The same, as they were when this portal session opened: a mode change
    /// that comes back to them needs nothing from the stream.
    std::string m_PortalOpenedModes;
    bool m_LoggedSharedMemory = false;
    /// The codec the pipeline was really built with. Starts as the Selector's
    /// choice and is lowered to H.264 when the portal forces the CPU pair —
    /// see buildPipeline. Read by SessionInfo, so the client is never promised
    /// a codec the route cannot produce.
    Codec m_Codec = Codec::H264;
    /// The codec every build starts from: the Selector's, unless it was one
    /// only Vulkan Video encodes (AV1, C13.12) and that chain was refused
    /// before the stream began — then the client's next codec the GPU
    /// encodes without it. Never changed once a pair has streamed.
    Codec m_SessionCodec = Codec::H264;
    /// A pair came up: the stream has begun under m_SessionCodec.
    bool m_PairBuilt = false;
    /// Said once per session, like the shared-memory line beside it.
    bool m_LoggedCodecDowngrade = false;

    /// Which pair buildPipeline actually made. Not derivable from m_Target: the
    /// portal can force the CPU pair on a machine whose GPU could have encoded.
    bool m_UsingCpuPair = false;

    /// Set when the GPU encoder was tried and found to write no parameter sets.
    /// Every later rebuild then goes straight to the CPU pair.
    bool m_GpuEncoderUnusable = false;

    /// The chain the last build runs, and why (LinuxRouteChoice.h); the route
    /// last said in the log, so a rebuild on the same chain says nothing.
    LinuxRoute m_Route;
    std::string m_LoggedRoute;
    /// The PCI vendor of the display's GPU: the vendor table's key.
    uint32_t m_VendorId = 0;
    /// Why the split route's Vulkan conversion was given up for this session —
    /// it did not start, or it failed while streaming — and "" while it has
    /// not: a refusal every later build carries, so GL converts from then on.
    std::string m_VulkanConvertRefusal;
    /// The same for the Vulkan Video chain: VA-API encodes from then on.
    std::string m_VulkanEncoderRefusal;
    /// The pixel proof's verdict for this session, and the size and codec it
    /// was for.
    std::string m_ProofRefusal;
    int m_ProofWidth = 0;
    int m_ProofHeight = 0;
    Codec m_ProofCodec = Codec::Hevc;
    bool m_ProofDone = false;
    /// The pair was rebuilt under the loop (leaveVulkan): the loop puts its
    /// bitrate back on the new encoder.
    bool m_RouteChanged = false;
    /// The Vulkan Video chain gave up encoding a picture (emit): the loop
    /// converts the held one into the new pair and sends it.
    bool m_ReconvertHeld = false;

    /// The size the session was opened at, which the cap scales FROM — never
    /// from the current one, or a run of reductions would compound.
    int m_FullWidth = 0;
    /// The capture size the pipeline was last built for — what a portal frame
    /// of another size is told apart by.
    int m_PipelineCaptureWidth = 0;
    int m_PipelineCaptureHeight = 0;
    int m_FullHeight = 0;
    encode::EncodeLoadCap m_LoadCap;
    std::atomic<bool> m_PendingResize{false};

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
    uint64_t m_ReportedShape = 0;
    bool m_ReportedVisible = false;
    /// The client was last told the pictures show the pointer themselves.
    bool m_ReportedPainted = false;
    /// When the pointer's position last went out to a self-drawing client.
    CursorPositionGate m_PositionGate;
};

} // namespace

namespace detail {

std::unique_ptr<Session> createPlatformSession(const SessionConfig& config,
                                               const ResolvedTarget& target,
                                               const SessionCallbacks& callbacks,
                                               std::string& error)
{
    if (target.encoder != EncoderApi::VaApi && target.encoder != EncoderApi::Software) {
        error = std::string("no Linux encoder for ") + toString(target.encoder);
        return nullptr;
    }
    return std::make_unique<LinuxSession>(config, target, callbacks);
}

} // namespace detail
} // namespace mw::native
