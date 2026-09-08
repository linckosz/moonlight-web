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

#include "VaapiEncoder.h"

#include "../../core/Log.h"
#include "../RateControl.h"

#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>

#include <cstring>

namespace mw::native::encode {
namespace {

std::string vaText(VAStatus status)
{
    return vaErrorStr(status);
}

/// Five reconstructed pictures: the one being written and four that may still
/// be referenced. Two would be enough for a stream that always predicts from
/// the previous frame — but a receiver that names a frame it never got needs
/// the encoder to reach FURTHER BACK, to a picture it still has, and the depth
/// of that reach is the number of frames a loss report may lag behind. Four is
/// what the three Windows encoders were measured with (design §9.10), and the
/// cost there was nil: no B-frames and no lookahead means nothing waits.
constexpr int kReconSurfaces = 5;

/// How many of them a picture may choose between — the DPB the sequence
/// announces.
constexpr int kMaxReferences = kReconSurfaces - 1;

/// A coded buffer sized for the worst frame, not the average: an I-frame at a
/// high bitrate on a busy desktop. Undersizing this fails on exactly that frame.
size_t codedBufferSize(int width, int height)
{
    return static_cast<size_t>(width) * height * 3 / 2 + 65536;
}

/// A misc-parameter buffer: the VA header followed by the payload.
///
/// Built as bytes because VAEncMiscParameterBuffer ends in a flexible array
/// member, which C++ will not let sit at the head of a wrapper struct. Same
/// construction FFmpeg uses; the alternative, a struct with the header first,
/// does not compile.
template <typename T>
bool createMisc(VADisplay display, VAContextID context, VAEncMiscParameterType type,
                const T& payload, VABufferID& out)
{
    std::vector<uint8_t> bytes(sizeof(VAEncMiscParameterBuffer) + sizeof(T), 0);
    auto* header = reinterpret_cast<VAEncMiscParameterBuffer*>(bytes.data());
    header->type = type;
    std::memcpy(header->data, &payload, sizeof(T));
    return vaCreateBuffer(display, context, VAEncMiscParameterBufferType,
                          static_cast<unsigned>(bytes.size()), 1, bytes.data(),
                          &out) == VA_STATUS_SUCCESS;
}

/// One frame's band of the rolling intra-refresh wave. The unit is the codec's
/// own block — macroblocks for H.264, CTBs for HEVC — which is why the caller
/// passes the width already counted in them, and why this is shared: the wave
/// itself is the same idea on both, and one copy keeps it that way.
VABufferID refreshBand(VADisplay display, VAContextID context, int position, int bandWidth)
{
    VAEncMiscParameterRIR rir = {};
    rir.rir_flags.bits.enable_rir_column = 1;
    rir.intra_insertion_location = static_cast<uint32_t>(position);
    rir.intra_insert_size = static_cast<uint32_t>(bandWidth);
    rir.qp_delta_for_inserted_intra = 0;
    VABufferID id = VA_INVALID_ID;
    if (!createMisc(display, context, VAEncMiscParameterTypeRIR, rir, id)) return VA_INVALID_ID;
    return id;
}

} // namespace

struct VaapiEncoder::Impl
{
    int renderFd = -1;
    VADisplay display = nullptr;
    VAProfile profile = VAProfileNone;
    VAConfigID config = VA_INVALID_ID;
    VAContextID context = VA_INVALID_ID;

    VASurfaceID input = VA_INVALID_SURFACE;
    VASurfaceID recon[kReconSurfaces] = {};
    int reconCurrent = 0;
    VABufferID coded = VA_INVALID_ID;

    /// What each reconstruction surface holds. `engineFrame` is the number the
    /// RECEIVER knows a picture by (EncodedFrame::frameNumber) — the only name
    /// it can use to say which one it never got; `picNum` is the number the
    /// BITSTREAM knows it by (frame_num for H.264, POC for HEVC), which counts
    /// from the last IDR. Keeping both is the whole trick: invalidation speaks
    /// the first language and the parameter buffers speak the second.
    struct ReconState
    {
        uint32_t engineFrame = 0;
        uint32_t picNum = 0;
        bool valid = false;
    };
    ReconState reconState[kReconSurfaces] = {};

    /// The slot the picture being encoded predicts from, chosen by encode()
    /// before the parameters are built. -1 for an IDR, which predicts from
    /// nothing.
    int referenceSlot = -1;

    /// The exported DMA-BUF descriptor of the input surface: the fds are ours
    /// to close at stop().
    VADRMPRIMESurfaceDescriptor exported = {};
    bool haveExport = false;

    bool packedHeadersRequired = false;
    bool rollingColumnRefresh = false;
};

VaapiEncoder::VaapiEncoder()
    : d(std::make_unique<Impl>())
{}

VaapiEncoder::~VaapiEncoder()
{
    stop();
}

bool VaapiEncoder::openDisplay(const std::string& renderNode, std::string& error)
{
    d->renderFd = ::open(renderNode.c_str(), O_RDWR | O_CLOEXEC);
    if (d->renderFd < 0) {
        error = "cannot open the render node " + renderNode;
        return false;
    }
    d->display = vaGetDisplayDRM(d->renderFd);
    if (!d->display) {
        error = "VA-API has no display for " + renderNode;
        return false;
    }
    int major = 0, minor = 0;
    const VAStatus status = vaInitialize(d->display, &major, &minor);
    if (status != VA_STATUS_SUCCESS) {
        error = "VA-API failed to initialize: " + vaText(status);
        d->display = nullptr;
        return false;
    }
    log::info(std::string("[native] VA-API ") + std::to_string(major) + "." +
              std::to_string(minor) + ": " + vaQueryVendorString(d->display));
    return true;
}

bool VaapiEncoder::chooseProfile(Codec codec, std::string& error)
{
    // Best profile the driver offers for the codec, with a real encode
    // entrypoint. Asked of the driver rather than assumed: the same libva sits
    // over silicon that spans a decade.
    const int count = vaMaxNumProfiles(d->display);
    std::vector<VAProfile> profiles(static_cast<size_t>(count));
    int have = 0;
    if (vaQueryConfigProfiles(d->display, profiles.data(), &have) != VA_STATUS_SUCCESS) {
        error = "VA-API cannot list its profiles";
        return false;
    }
    profiles.resize(static_cast<size_t>(have));

    const auto supportsEncode = [&](VAProfile p) {
        const int n = vaMaxNumEntrypoints(d->display);
        std::vector<VAEntrypoint> entrypoints(static_cast<size_t>(n));
        int got = 0;
        if (vaQueryConfigEntrypoints(d->display, p, entrypoints.data(), &got) != VA_STATUS_SUCCESS)
            return false;
        for (int i = 0; i < got; ++i)
            if (entrypoints[static_cast<size_t>(i)] == VAEntrypointEncSlice) return true;
        return false;
    };
    const auto pick = [&](std::initializer_list<VAProfile> wanted) {
        for (VAProfile w : wanted)
            for (VAProfile p : profiles)
                if (p == w && supportsEncode(p)) return w;
        return VAProfileNone;
    };

    switch (codec) {
    case Codec::H264:
        d->profile = pick({VAProfileH264High, VAProfileH264Main, VAProfileH264ConstrainedBaseline});
        break;
    case Codec::Hevc: d->profile = pick({VAProfileHEVCMain}); break;
    case Codec::Av1:
        // The driver has it (vainfo lists AV1Profile0 EncSlice on a 780M) and
        // nothing here has driven it yet. Refused rather than shipped blind —
        // the capability query does not claim it either.
        error = "AV1 through VA-API is not implemented yet";
        return false;
    }
    if (d->profile == VAProfileNone) {
        error = std::string("this GPU has no VA-API encoder for ") + toString(codec);
        return false;
    }
    return true;
}

bool VaapiEncoder::createSurfaces(std::string& error)
{
    VAStatus status =
        vaCreateSurfaces(d->display, VA_RT_FORMAT_YUV420, static_cast<unsigned>(m_Width),
                         static_cast<unsigned>(m_Height), &d->input, 1, nullptr, 0);
    if (status != VA_STATUS_SUCCESS) {
        error = "could not allocate the input surface: " + vaText(status);
        return false;
    }
    status =
        vaCreateSurfaces(d->display, VA_RT_FORMAT_YUV420, static_cast<unsigned>(m_Width),
                         static_cast<unsigned>(m_Height), d->recon, kReconSurfaces, nullptr, 0);
    if (status != VA_STATUS_SUCCESS) {
        error = "could not allocate the reconstruction surfaces: " + vaText(status);
        return false;
    }
    return true;
}

bool VaapiEncoder::exportInput(std::string& error)
{
    // Two layers, one per plane, so GlConvert can view each as its own image —
    // R8 for the luma, GR88 for the chroma — and render into them separately.
    const VAStatus status = vaExportSurfaceHandle(
        d->display, d->input, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &d->exported);
    if (status != VA_STATUS_SUCCESS) {
        error = "could not export the input surface: " + vaText(status);
        return false;
    }
    d->haveExport = true;
    if (d->exported.num_layers != 2) {
        error = "the input surface exported with " + std::to_string(d->exported.num_layers) +
                " layers, not the two NV12 planes";
        return false;
    }
    m_Input = convert::Nv12Target{};
    m_Input.width = m_Width;
    m_Input.height = m_Height;
    m_Input.modifier = d->exported.objects[0].drm_format_modifier;
    m_Input.fdY = d->exported.objects[d->exported.layers[0].object_index[0]].fd;
    m_Input.offsetY = d->exported.layers[0].offset[0];
    m_Input.pitchY = d->exported.layers[0].pitch[0];
    m_Input.fdUV = d->exported.objects[d->exported.layers[1].object_index[0]].fd;
    m_Input.offsetUV = d->exported.layers[1].offset[0];
    m_Input.pitchUV = d->exported.layers[1].pitch[0];
    return true;
}

bool VaapiEncoder::init(const std::string& renderNode, Codec codec, int width, int height, int fps,
                        int bitrateKbps, bool intraRefresh, const EncoderTuning& tuning,
                        std::string& error)
{
    stop();
    d = std::make_unique<Impl>();

    if (width <= 0 || height <= 0) {
        error = "invalid encoder parameters";
        return false;
    }
    m_Codec = codec;
    m_Width = width & ~1;
    m_Height = height & ~1;
    // H.264 carries a cropping window in its sequence parameters, so a picture
    // that is not a whole number of macroblocks is padded and cropped back.
    // HEVC's VA-API sequence buffer has no such field and the driver writes the
    // SPS, so the only honest coded size is a whole number of minimum coding
    // blocks: round DOWN to 8 and encode that. Every common resolution already
    // is one — 1920×1080 included — so this costs nothing where it costs
    // nothing, and loses at most 7 columns or rows where it would otherwise
    // hand the decoder a size the bitstream cannot express.
    if (codec == Codec::Hevc) {
        const int alignedWidth = m_Width & ~7;
        const int alignedHeight = m_Height & ~7;
        if (alignedWidth != m_Width || alignedHeight != m_Height)
            log::info("[native] VA-API HEVC: " + std::to_string(m_Width) + "x" +
                      std::to_string(m_Height) +
                      " is not a whole number of 8-pixel blocks — "
                      "encoding " +
                      std::to_string(alignedWidth) + "x" + std::to_string(alignedHeight));
        m_Width = alignedWidth;
        m_Height = alignedHeight;
    }
    m_Fps = fps > 0 ? fps : 60;
    m_BitrateKbps = bitrateKbps > 0 ? bitrateKbps : 20000;
    m_Tuning = tuning;

    if (!openDisplay(renderNode, error)) return false;
    if (!chooseProfile(codec, error)) return false;

    // What this encoder can do, asked before it is configured: the rate
    // control mode, whether it writes its own headers, and whether it refreshes
    // by rolling columns. Each answer changes what is sent per frame.
    VAConfigAttrib attribs[4] = {};
    attribs[0].type = VAConfigAttribRTFormat;
    attribs[1].type = VAConfigAttribRateControl;
    attribs[2].type = VAConfigAttribEncPackedHeaders;
    attribs[3].type = VAConfigAttribEncIntraRefresh;
    if (vaGetConfigAttributes(d->display, d->profile, VAEntrypointEncSlice, attribs, 4) !=
        VA_STATUS_SUCCESS) {
        error = "VA-API cannot describe the encoder";
        return false;
    }
    if (!(attribs[0].value & VA_RT_FORMAT_YUV420)) {
        error = "the encoder does not take NV12";
        return false;
    }
    if (!(attribs[1].value & VA_RC_CBR)) {
        error = "the encoder has no constant-bitrate mode";
        return false;
    }
    d->packedHeadersRequired = attribs[2].value != VA_ATTRIB_NOT_SUPPORTED &&
                               attribs[2].value != 0 && false; // see the header: not sent yet
    d->rollingColumnRefresh = attribs[3].value != VA_ATTRIB_NOT_SUPPORTED &&
                              (attribs[3].value & VA_ENC_INTRA_REFRESH_ROLLING_COLUMN);

    VAConfigAttrib chosen[2] = {};
    chosen[0].type = VAConfigAttribRTFormat;
    chosen[0].value = VA_RT_FORMAT_YUV420;
    chosen[1].type = VAConfigAttribRateControl;
    chosen[1].value = VA_RC_CBR;
    VAStatus status =
        vaCreateConfig(d->display, d->profile, VAEntrypointEncSlice, chosen, 2, &d->config);
    if (status != VA_STATUS_SUCCESS) {
        error = "could not create the encoder configuration: " + vaText(status);
        return false;
    }

    if (!createSurfaces(error)) return false;
    if (!exportInput(error)) return false;

    // The context is what binds the reconstruction surfaces to the encoder.
    status = vaCreateContext(d->display, d->config, m_Width, m_Height, VA_PROGRESSIVE, d->recon,
                             kReconSurfaces, &d->context);
    if (status != VA_STATUS_SUCCESS) {
        error = "could not create the encoder context: " + vaText(status);
        return false;
    }

    status = vaCreateBuffer(d->display, d->context, VAEncCodedBufferType,
                            static_cast<unsigned>(codedBufferSize(m_Width, m_Height)), 1, nullptr,
                            &d->coded);
    if (status != VA_STATUS_SUCCESS) {
        error = "could not create the coded buffer: " + vaText(status);
        return false;
    }

    // Intra-refresh only where the driver rolls columns; otherwise keyframes,
    // reported honestly so the receiver keeps its own recovery.
    m_IntraRefresh = intraRefresh && d->rollingColumnRefresh;
    m_IntraRefreshPeriod = m_IntraRefresh ? intraRefreshPeriodFrames(m_Fps) : 0;
    if (intraRefresh && !m_IntraRefresh)
        log::info(
            "[native] VA-API: this driver has no rolling intra-refresh — keyframes on demand");

    m_FrameNum = 0;
    m_IdrPicId = 0;
    m_HaveReference = false;
    m_RateDirty = true;
    m_RefreshPosition = 0;

    log::info("[native] VA-API ready: " + std::to_string(m_Width) + "x" + std::to_string(m_Height) +
              "@" + std::to_string(m_Fps) + " " + toString(codec) + " CBR " +
              std::to_string(m_BitrateKbps) + " kbps, VBV " +
              std::to_string(
                  vbvBits(static_cast<uint32_t>(m_BitrateKbps) * 1000u, m_Fps, m_Tuning.vbvFrames) /
                  8 / 1024) +
              " KB" +
              (m_IntraRefresh
                   ? ", intra-refresh over " + std::to_string(m_IntraRefreshPeriod) + " frames"
                   : ", keyframes on demand") +
              ", headers by the driver" +
              (m_Tuning.isDefault() ? "" : " [bench: " + m_Tuning.describe() + "]"));
    return true;
}

bool VaapiEncoder::renderRateControl(std::string& error)
{
    // Sent with the first frame and whenever the bitrate moves — VA-API takes
    // rate-control changes as ordinary per-picture parameters, which is what
    // makes setBitrate() a no-restart operation here as everywhere else.
    const uint32_t bitsPerSecond = static_cast<uint32_t>(m_BitrateKbps) * 1000u;
    const uint32_t vbv = vbvBits(bitsPerSecond, m_Fps, m_Tuning.vbvFrames);

    VAEncMiscParameterRateControl rc = {};
    rc.bits_per_second = bitsPerSecond;
    rc.target_percentage = 100;
    rc.window_size = 1000;
    rc.initial_qp = 0;
    rc.min_qp = 0;
    rc.max_qp = 51;
    // No filler: on a still desktop CBR padding would be bytes on the wire that
    // carry nothing, and the still-screen floor already keeps the link alive.
    rc.rc_flags.bits.disable_bit_stuffing = 1;

    VAEncMiscParameterHRD hrd = {};
    hrd.buffer_size = vbv;
    hrd.initial_buffer_fullness = vbv;

    VAEncMiscParameterFrameRate frameRate = {};
    frameRate.framerate = static_cast<uint32_t>(m_Fps);

    VABufferID buffers[3] = {VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID};
    if (!createMisc(d->display, d->context, VAEncMiscParameterTypeRateControl, rc, buffers[0]) ||
        !createMisc(d->display, d->context, VAEncMiscParameterTypeHRD, hrd, buffers[1]) ||
        !createMisc(d->display, d->context, VAEncMiscParameterTypeFrameRate, frameRate,
                    buffers[2])) {
        error = "could not create the rate-control buffers";
        for (VABufferID b : buffers)
            if (b != VA_INVALID_ID) vaDestroyBuffer(d->display, b);
        return false;
    }
    const VAStatus status = vaRenderPicture(d->display, d->context, buffers, 3);
    for (VABufferID b : buffers)
        if (b != VA_INVALID_ID) vaDestroyBuffer(d->display, b);
    if (status != VA_STATUS_SUCCESS) {
        error = "rate control refused: " + vaText(status);
        return false;
    }
    m_RateDirty = false;
    return true;
}

bool VaapiEncoder::renderH264(bool idr, std::string& error)
{
    const uint32_t widthMbs = static_cast<uint32_t>((m_Width + 15) / 16);
    const uint32_t heightMbs = static_cast<uint32_t>((m_Height + 15) / 16);
    const VASurfaceID current = d->recon[d->reconCurrent];
    const bool predicts = !idr && d->referenceSlot >= 0;
    const VASurfaceID reference = predicts ? d->recon[d->referenceSlot] : VA_INVALID_SURFACE;
    const uint32_t referencePicNum = predicts ? d->reconState[d->referenceSlot].picNum : 0;

    std::vector<VABufferID> buffers;
    const auto add = [&](VABufferType type, const void* data, unsigned size) {
        VABufferID id = VA_INVALID_ID;
        if (vaCreateBuffer(d->display, d->context, type, size, 1, const_cast<void*>(data), &id) !=
            VA_STATUS_SUCCESS)
            return false;
        buffers.push_back(id);
        return true;
    };

    if (idr) {
        VAEncSequenceParameterBufferH264 seq = {};
        seq.seq_parameter_set_id = 0;
        seq.level_idc = 51;
        // No periodic keyframe: the largest period the field holds. The IDR the
        // client asks for is forced per picture, below.
        seq.intra_period = 0;
        seq.intra_idr_period = 0;
        seq.ip_period = 1;
        seq.bits_per_second = static_cast<uint32_t>(m_BitrateKbps) * 1000u;
        // The DPB the decoder must keep. One would do for a stream that always
        // predicts from the previous picture; four is what lets a picture reach
        // back past a frame the receiver lost and heal with a delta.
        seq.max_num_ref_frames = kMaxReferences;
        seq.picture_width_in_mbs = static_cast<uint16_t>(widthMbs);
        seq.picture_height_in_mbs = static_cast<uint16_t>(heightMbs);
        seq.seq_fields.bits.chroma_format_idc = 1;
        seq.seq_fields.bits.frame_mbs_only_flag = 1;
        seq.seq_fields.bits.direct_8x8_inference_flag = 1;
        seq.seq_fields.bits.log2_max_frame_num_minus4 = 12;
        // POC type 2: the order IS the decode order, which is the whole truth
        // of a stream with no B-frames, and the cheapest to signal.
        seq.seq_fields.bits.pic_order_cnt_type = 2;
        seq.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 0;
        // The picture is padded to whole macroblocks; the crop says what is
        // real. 1080 is not a multiple of 16, so this is not theoretical.
        if (widthMbs * 16 != static_cast<uint32_t>(m_Width) ||
            heightMbs * 16 != static_cast<uint32_t>(m_Height)) {
            seq.frame_cropping_flag = 1;
            seq.frame_crop_right_offset = (widthMbs * 16 - static_cast<uint32_t>(m_Width)) / 2;
            seq.frame_crop_bottom_offset = (heightMbs * 16 - static_cast<uint32_t>(m_Height)) / 2;
        }
        // The VUI, and in it bitstream_restriction: without it a browser's
        // hardware decoder assumes a DPB's worth of reordering on a stream that
        // has none — 200 ms of decode latency, measured on NVENC (B8). The
        // timing info tells the same decoder the frame rate.
        seq.vui_parameters_present_flag = 1;
        seq.vui_fields.bits.timing_info_present_flag = 1;
        seq.vui_fields.bits.bitstream_restriction_flag = 1;
        seq.vui_fields.bits.log2_max_mv_length_horizontal = 15;
        seq.vui_fields.bits.log2_max_mv_length_vertical = 15;
        seq.num_units_in_tick = 1;
        seq.time_scale = static_cast<uint32_t>(m_Fps) * 2;
        // num_reorder_frames is not a field here: the driver derives it from
        // ip_period = 1 (no B-frames), and writes 0.
        if (!add(VAEncSequenceParameterBufferType, &seq, sizeof(seq))) {
            error = "could not create the sequence parameters";
            return false;
        }
    }

    VAEncPictureParameterBufferH264 pic = {};
    pic.CurrPic.picture_id = current;
    pic.CurrPic.frame_idx = m_FrameNum;
    pic.CurrPic.flags = 0;
    pic.CurrPic.TopFieldOrderCnt = static_cast<int32_t>(2 * m_FrameNum);
    pic.CurrPic.BottomFieldOrderCnt = pic.CurrPic.TopFieldOrderCnt;
    for (auto& ref : pic.ReferenceFrames) {
        ref.picture_id = VA_INVALID_SURFACE;
        ref.flags = VA_PICTURE_H264_INVALID;
    }
    if (predicts) {
        pic.ReferenceFrames[0].picture_id = reference;
        pic.ReferenceFrames[0].frame_idx = referencePicNum;
        pic.ReferenceFrames[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        pic.ReferenceFrames[0].TopFieldOrderCnt = static_cast<int32_t>(2 * referencePicNum);
        pic.ReferenceFrames[0].BottomFieldOrderCnt = pic.ReferenceFrames[0].TopFieldOrderCnt;
    }
    pic.coded_buf = d->coded;
    pic.pic_parameter_set_id = 0;
    pic.seq_parameter_set_id = 0;
    pic.frame_num = static_cast<uint16_t>(m_FrameNum);
    pic.pic_init_qp = 26;
    pic.num_ref_idx_l0_active_minus1 = 0;
    pic.pic_fields.bits.idr_pic_flag = idr ? 1 : 0;
    pic.pic_fields.bits.reference_pic_flag = 1;
    pic.pic_fields.bits.entropy_coding_mode_flag = 1; // CABAC
    pic.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    pic.pic_fields.bits.transform_8x8_mode_flag = d->profile == VAProfileH264High ? 1 : 0;
    if (!add(VAEncPictureParameterBufferType, &pic, sizeof(pic))) {
        error = "could not create the picture parameters";
        return false;
    }

    if (m_IntraRefresh) {
        // The rolling column: which band of macroblocks is intra this frame.
        // After one period the whole picture has been refreshed — the wave that
        // replaces the periodic keyframe, as on the three Windows encoders.
        const int bandWidth = static_cast<int>((widthMbs + m_IntraRefreshPeriod - 1) /
                                               static_cast<uint32_t>(m_IntraRefreshPeriod));
        const VABufferID refresh =
            refreshBand(d->display, d->context, m_RefreshPosition, bandWidth);
        if (refresh == VA_INVALID_ID) {
            error = "could not create the intra-refresh parameters";
            return false;
        }
        buffers.push_back(refresh);
        m_RefreshPosition += bandWidth;
        if (m_RefreshPosition >= static_cast<int>(widthMbs)) m_RefreshPosition = 0;
    }

    VAEncSliceParameterBufferH264 slice = {};
    slice.macroblock_address = 0;
    slice.num_macroblocks = widthMbs * heightMbs;
    slice.slice_type = idr ? 2 : 0; // I : P
    slice.pic_parameter_set_id = 0;
    slice.idr_pic_id = static_cast<uint16_t>(m_IdrPicId);
    slice.pic_order_cnt_lsb = 0;
    slice.direct_spatial_mv_pred_flag = 1;
    slice.num_ref_idx_active_override_flag = 0;
    slice.cabac_init_idc = 0;
    slice.slice_qp_delta = 0;
    slice.disable_deblocking_filter_idc = 0;
    for (auto& ref : slice.RefPicList0) {
        ref.picture_id = VA_INVALID_SURFACE;
        ref.flags = VA_PICTURE_H264_INVALID;
    }
    for (auto& ref : slice.RefPicList1) {
        ref.picture_id = VA_INVALID_SURFACE;
        ref.flags = VA_PICTURE_H264_INVALID;
    }
    if (predicts) slice.RefPicList0[0] = pic.ReferenceFrames[0];
    if (!add(VAEncSliceParameterBufferType, &slice, sizeof(slice))) {
        error = "could not create the slice parameters";
        return false;
    }

    const VAStatus status =
        vaRenderPicture(d->display, d->context, buffers.data(), static_cast<int>(buffers.size()));
    for (VABufferID b : buffers)
        vaDestroyBuffer(d->display, b);
    if (status != VA_STATUS_SUCCESS) {
        error = "the H.264 picture was refused: " + vaText(status);
        return false;
    }
    return true;
}

bool VaapiEncoder::renderHevc(bool idr, std::string& error)
{
    // HEVC counts in coding tree blocks, not macroblocks. 64 is the CTB every
    // encoder in this class uses: log2 min CB 8 (minus3 = 0) plus a difference
    // of 3. The picture is a whole number of min-CBs by construction — init()
    // aligns it to 8 for this codec, because unlike H.264 the sequence
    // parameters carry no cropping window and the driver writes the SPS.
    const uint32_t ctbWidth = static_cast<uint32_t>((m_Width + 63) / 64);
    const uint32_t ctbHeight = static_cast<uint32_t>((m_Height + 63) / 64);
    const VASurfaceID current = d->recon[d->reconCurrent];
    const bool predicts = !idr && d->referenceSlot >= 0;
    const VASurfaceID reference = predicts ? d->recon[d->referenceSlot] : VA_INVALID_SURFACE;
    const uint32_t referencePicNum = predicts ? d->reconState[d->referenceSlot].picNum : 0;

    std::vector<VABufferID> buffers;
    const auto add = [&](VABufferType type, const void* data, unsigned size) {
        VABufferID id = VA_INVALID_ID;
        if (vaCreateBuffer(d->display, d->context, type, size, 1, const_cast<void*>(data), &id) !=
            VA_STATUS_SUCCESS)
            return false;
        buffers.push_back(id);
        return true;
    };

    if (idr) {
        VAEncSequenceParameterBufferHEVC seq = {};
        seq.general_profile_idc = 1; // Main
        // Levels are 30× the number, so 5.1 — the same ceiling H.264 is given
        // here, and enough for 4K30 or 1440p60.
        seq.general_level_idc = 153;
        seq.general_tier_flag = 0;
        // No periodic keyframe, exactly as on H.264: the IDR the client asks
        // for is forced per picture, below.
        seq.intra_period = 0;
        seq.intra_idr_period = 0;
        seq.ip_period = 1;
        seq.bits_per_second = static_cast<uint32_t>(m_BitrateKbps) * 1000u;
        seq.pic_width_in_luma_samples = static_cast<uint16_t>(m_Width);
        seq.pic_height_in_luma_samples = static_cast<uint16_t>(m_Height);
        seq.seq_fields.bits.chroma_format_idc = 1; // 4:2:0
        seq.seq_fields.bits.bit_depth_luma_minus8 = 0;
        seq.seq_fields.bits.bit_depth_chroma_minus8 = 0;
        seq.seq_fields.bits.strong_intra_smoothing_enabled_flag = 1;
        seq.seq_fields.bits.amp_enabled_flag = 1;
        seq.seq_fields.bits.sample_adaptive_offset_enabled_flag = 1;
        // Temporal MV prediction off: it predicts motion from the collocated
        // picture, which is one more thing that breaks when a reference is lost
        // — and this stream heals by naming lost frames, not by keyframes.
        seq.seq_fields.bits.sps_temporal_mvp_enabled_flag = 0;
        // I and P only, no random-access B: says out loud what ip_period = 1
        // already means, and lets the rate control treat the GOP as flat.
        seq.seq_fields.bits.low_delay_seq = 1;
        seq.log2_min_luma_coding_block_size_minus3 = 0;
        seq.log2_diff_max_min_luma_coding_block_size = 3; // CTB 64
        seq.log2_min_transform_block_size_minus2 = 0;
        seq.log2_diff_max_min_transform_block_size = 3;
        seq.max_transform_hierarchy_depth_inter = 2;
        seq.max_transform_hierarchy_depth_intra = 2;
        // The VUI, and in it bitstream_restriction — the B8 lesson, which cost
        // 200 ms of decode latency on NVENC H.264 and applies to every codec a
        // browser hands to a hardware decoder: without it the decoder assumes a
        // DPB's worth of reordering on a stream that has none.
        seq.vui_parameters_present_flag = 1;
        seq.vui_fields.bits.vui_timing_info_present_flag = 1;
        seq.vui_fields.bits.bitstream_restriction_flag = 1;
        seq.vui_fields.bits.motion_vectors_over_pic_boundaries_flag = 1;
        seq.vui_fields.bits.restricted_ref_pic_lists_flag = 1;
        seq.vui_fields.bits.log2_max_mv_length_horizontal = 15;
        seq.vui_fields.bits.log2_max_mv_length_vertical = 15;
        // Unlike H.264, where the tick counts fields, an HEVC clock tick is a
        // frame: time_scale over num_units_in_tick IS the frame rate.
        seq.vui_num_units_in_tick = 1;
        seq.vui_time_scale = static_cast<uint32_t>(m_Fps);
        if (!add(VAEncSequenceParameterBufferType, &seq, sizeof(seq))) {
            error = "could not create the sequence parameters";
            return false;
        }
    }

    VAEncPictureParameterBufferHEVC pic = {};
    pic.decoded_curr_pic.picture_id = current;
    pic.decoded_curr_pic.pic_order_cnt = static_cast<int32_t>(m_FrameNum);
    pic.decoded_curr_pic.flags = 0;
    for (auto& ref : pic.reference_frames) {
        ref.picture_id = VA_INVALID_SURFACE;
        ref.flags = VA_PICTURE_HEVC_INVALID;
    }
    if (predicts) {
        pic.reference_frames[0].picture_id = reference;
        pic.reference_frames[0].pic_order_cnt = static_cast<int32_t>(referencePicNum);
        pic.reference_frames[0].flags = 0;
    }
    pic.coded_buf = d->coded;
    // 0xFF is the "there is no collocated picture" value the header asks for
    // when slice_temporal_mvp_enabled_flag is 0.
    pic.collocated_ref_pic_index = 0xFF;
    pic.last_picture = 0;
    pic.pic_init_qp = 26;
    pic.diff_cu_qp_delta_depth = 0;
    pic.num_ref_idx_l0_default_active_minus1 = 0;
    pic.num_ref_idx_l1_default_active_minus1 = 0;
    pic.slice_pic_parameter_set_id = 0;
    // IDR_W_RADL for a keyframe, TRAIL_R for a referenced delta.
    pic.nal_unit_type = idr ? 19 : 1;
    pic.pic_fields.bits.idr_pic_flag = idr ? 1 : 0;
    pic.pic_fields.bits.coding_type = idr ? 1 : 2; // I : P
    pic.pic_fields.bits.reference_pic_flag = 1;
    // Per-CU QP deltas are what lets a bitrate-driven encoder spend unevenly
    // inside a picture; without them CBR can only move the whole frame.
    pic.pic_fields.bits.cu_qp_delta_enabled_flag = 1;
    pic.log2_parallel_merge_level_minus2 = 0;
    pic.ctu_max_bitsize_allowed = 0;
    if (!add(VAEncPictureParameterBufferType, &pic, sizeof(pic))) {
        error = "could not create the picture parameters";
        return false;
    }

    if (m_IntraRefresh) {
        const int bandWidth = static_cast<int>((ctbWidth + m_IntraRefreshPeriod - 1) /
                                               static_cast<uint32_t>(m_IntraRefreshPeriod));
        const VABufferID refresh =
            refreshBand(d->display, d->context, m_RefreshPosition, bandWidth);
        if (refresh == VA_INVALID_ID) {
            error = "could not create the intra-refresh parameters";
            return false;
        }
        buffers.push_back(refresh);
        m_RefreshPosition += bandWidth;
        if (m_RefreshPosition >= static_cast<int>(ctbWidth)) m_RefreshPosition = 0;
    }

    VAEncSliceParameterBufferHEVC slice = {};
    slice.slice_segment_address = 0;
    slice.num_ctu_in_slice = ctbWidth * ctbHeight;
    // HEVC numbers its slice types the other way round from H.264: B 0, P 1, I 2.
    slice.slice_type = idr ? 2 : 1;
    slice.slice_pic_parameter_set_id = 0;
    slice.num_ref_idx_l0_active_minus1 = 0;
    slice.num_ref_idx_l1_active_minus1 = 0;
    for (auto& ref : slice.ref_pic_list0) {
        ref.picture_id = VA_INVALID_SURFACE;
        ref.flags = VA_PICTURE_HEVC_INVALID;
    }
    for (auto& ref : slice.ref_pic_list1) {
        ref.picture_id = VA_INVALID_SURFACE;
        ref.flags = VA_PICTURE_HEVC_INVALID;
    }
    if (predicts) slice.ref_pic_list0[0] = pic.reference_frames[0];
    slice.max_num_merge_cand = 5;
    slice.slice_qp_delta = 0;
    slice.slice_fields.bits.last_slice_of_pic_flag = 1;
    // SAO is on in the sequence, so it has to be allowed in the slice too, or
    // the filter the SPS promised never runs.
    slice.slice_fields.bits.slice_sao_luma_flag = 1;
    slice.slice_fields.bits.slice_sao_chroma_flag = 1;
    slice.slice_fields.bits.slice_temporal_mvp_enabled_flag = 0;
    slice.slice_fields.bits.num_ref_idx_active_override_flag = 0;
    slice.slice_fields.bits.collocated_from_l0_flag = 1;
    if (!add(VAEncSliceParameterBufferType, &slice, sizeof(slice))) {
        error = "could not create the slice parameters";
        return false;
    }

    const VAStatus status =
        vaRenderPicture(d->display, d->context, buffers.data(), static_cast<int>(buffers.size()));
    for (VABufferID b : buffers)
        vaDestroyBuffer(d->display, b);
    if (status != VA_STATUS_SUCCESS) {
        error = "the HEVC picture was refused: " + vaText(status);
        return false;
    }
    return true;
}

bool VaapiEncoder::encode(bool forceKeyframe, uint32_t frameNumber, EncoderOutput& out,
                          std::string& error)
{
    if (!d->display || d->context == VA_INVALID_ID) {
        error = "the encoder is not initialized";
        return false;
    }
    if (m_OutputHeld) {
        error = "the previous frame was not released";
        return false;
    }

    // The newest picture the receiver is still known to have. Normally that is
    // the one just encoded; after an invalidation it is older, and the delta
    // that reaches back over the hole is what saves a keyframe.
    d->referenceSlot = -1;
    uint32_t newest = 0;
    for (int i = 0; i < kReconSurfaces; ++i) {
        if (i == d->reconCurrent || !d->reconState[i].valid) continue;
        if (d->referenceSlot < 0 || d->reconState[i].picNum >= newest) {
            newest = d->reconState[i].picNum;
            d->referenceSlot = i;
        }
    }

    const bool idr = forceKeyframe || d->referenceSlot < 0;
    if (idr) {
        m_FrameNum = 0;
        ++m_IdrPicId;
        m_RateDirty = true; // the sequence goes out with the IDR, the rate with it
        d->referenceSlot = -1;
        // An IDR resets the decoder's whole picture buffer: nothing older
        // survives it, so nothing older may be referenced afterwards.
        for (int i = 0; i < kReconSurfaces; ++i)
            d->reconState[i].valid = false;
    }

    VAStatus status = vaBeginPicture(d->display, d->context, d->input);
    if (status != VA_STATUS_SUCCESS) {
        error = "vaBeginPicture: " + vaText(status);
        return false;
    }
    if (m_RateDirty && !renderRateControl(error)) return false;
    const bool rendered = m_Codec == Codec::H264 ? renderH264(idr, error) : renderHevc(idr, error);
    if (!rendered) return false;
    status = vaEndPicture(d->display, d->context);
    if (status != VA_STATUS_SUCCESS) {
        error = "vaEndPicture: " + vaText(status);
        return false;
    }
    // Synchronous, like the other three: one frame in flight by design.
    status = vaSyncSurface(d->display, d->input);
    if (status != VA_STATUS_SUCCESS) {
        error = "waiting for the encoded frame failed: " + vaText(status);
        return false;
    }

    // The coded data comes back as a chain of segments; they are gathered into
    // one Annex-B run the sender can fragment. One copy, GPU→CPU, as everywhere.
    VACodedBufferSegment* segment = nullptr;
    status = vaMapBuffer(d->display, d->coded, reinterpret_cast<void**>(&segment));
    if (status != VA_STATUS_SUCCESS) {
        error = "could not read the coded buffer: " + vaText(status);
        return false;
    }
    m_Bitstream.clear();
    for (VACodedBufferSegment* s = segment; s; s = static_cast<VACodedBufferSegment*>(s->next)) {
        const auto* bytes = static_cast<const uint8_t*>(s->buf);
        m_Bitstream.insert(m_Bitstream.end(), bytes, bytes + s->size);
    }
    vaUnmapBuffer(d->display, d->coded);

    m_OutputHeld = true;
    out.data = m_Bitstream.data();
    out.size = m_Bitstream.size();
    out.keyframe = idr;
    out.avgQp = -1;

    // This reconstruction is now a picture the receiver has — remembered under
    // BOTH names, the receiver's and the bitstream's.
    d->reconState[d->reconCurrent].engineFrame = frameNumber;
    d->reconState[d->reconCurrent].picNum = m_FrameNum;
    d->reconState[d->reconCurrent].valid = true;
    d->reconCurrent = (d->reconCurrent + 1) % kReconSurfaces;
    ++m_FrameNum;
    m_HaveReference = true;
    return true;
}

bool VaapiEncoder::invalidateReference(uint32_t frameNumber, std::string& error)
{
    if (!d->display) {
        error = "the encoder is not initialized";
        return false;
    }
    // The named frame never arrived, and every picture encoded after it may
    // have predicted from it — so the whole tail goes, not just the one. What
    // is left is a picture the receiver demonstrably has, and the next delta
    // predicts from that.
    int survivors = 0;
    for (int i = 0; i < kReconSurfaces; ++i) {
        if (!d->reconState[i].valid) continue;
        if (d->reconState[i].engineFrame >= frameNumber)
            d->reconState[i].valid = false;
        else
            ++survivors;
    }
    if (survivors == 0) {
        error = "no reconstruction older than frame " + std::to_string(frameNumber) +
                " is still held — a keyframe is the only repair";
        return false;
    }
    return true;
}

void VaapiEncoder::releaseOutput()
{
    m_OutputHeld = false;
}

bool VaapiEncoder::setBitrate(int bitrateKbps, std::string& error)
{
    if (!d->display || bitrateKbps <= 0) {
        error = "the encoder is not initialized";
        return false;
    }
    // Taken by the next frame, with the VBV re-derived through the same floor
    // as init() — B4 on AMF and the oneVPL reset were both this step done wrong.
    m_BitrateKbps = bitrateKbps;
    m_RateDirty = true;
    return true;
}

void VaapiEncoder::stop()
{
    if (!d) return;
    if (d->display) {
        if (d->coded != VA_INVALID_ID) vaDestroyBuffer(d->display, d->coded);
        if (d->context != VA_INVALID_ID) vaDestroyContext(d->display, d->context);
        if (d->config != VA_INVALID_ID) vaDestroyConfig(d->display, d->config);
        if (d->haveExport)
            for (unsigned i = 0; i < d->exported.num_objects; ++i)
                ::close(d->exported.objects[i].fd);
        if (d->input != VA_INVALID_SURFACE) vaDestroySurfaces(d->display, &d->input, 1);
        if (d->recon[0] != VA_INVALID_SURFACE)
            vaDestroySurfaces(d->display, d->recon, kReconSurfaces);
        vaTerminate(d->display);
    }
    if (d->renderFd >= 0) ::close(d->renderFd);
    d = std::make_unique<Impl>();
    m_Input = convert::Nv12Target{};
    m_OutputHeld = false;
    m_HaveReference = false;
}

} // namespace mw::native::encode
