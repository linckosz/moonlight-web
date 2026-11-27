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

#include "PortalCapture.h"

#include "../../audio/linux/PipeWireLibrary.h"
#include "../../core/Log.h"
#include "../../core/PortalCursorMode.h"
#include "MutterScreenCast.h"
#include "XFixesCursor.h"

#include <drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <signal.h>
#include <spa/utils/result.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>

// PipeWire 0.3.40's name, for a header older than that — the value is the
// protocol's.
#ifndef SPA_POD_PROP_FLAG_DONT_FIXATE
#define SPA_POD_PROP_FLAG_DONT_FIXATE (1u << 4)
#endif

namespace mw::native::capture {
namespace {

/// pw_init is process-wide; the audio tap may already have called it, and
/// calling it twice is harmless but calling it from two threads at once is not.
void ensurePipeWire()
{
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

/// SPA's video formats, as DRM fourccs. Only the four a compositor actually
/// offers for a screen: 32-bit packed, with or without a meaningful alpha.
///
/// The byte orders look reversed because they are: SPA names a format by the
/// order of its COMPONENTS, DRM by the order of its BYTES in memory, and on a
/// little-endian machine those two run opposite ways.
uint32_t drmFourcc(uint32_t spaFormat)
{
    switch (spaFormat) {
    case SPA_VIDEO_FORMAT_BGRx: return DRM_FORMAT_XRGB8888;
    case SPA_VIDEO_FORMAT_BGRA: return DRM_FORMAT_ARGB8888;
    case SPA_VIDEO_FORMAT_RGBx: return DRM_FORMAT_XBGR8888;
    case SPA_VIDEO_FORMAT_RGBA: return DRM_FORMAT_ABGR8888;
    default: return 0;
    }
}

/// Room for a cursor of @p w × @p h in a buffer's metadata: the position
/// header, the bitmap header, and the pixels. SPA has no macro for this — the
/// size is the application's to ask for, and asking for too little is how the
/// shape silently never arrives.
constexpr int cursorMetaSize(int w, int h)
{
    return static_cast<int>(sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap)) + w * h * 4;
}

/// SPA_META_VideoTransform and its struct, spa_meta_videotransform, by their
/// ABI: the PipeWire of Ubuntu 22.04 (0.3.48), which builds the packages, may
/// predate the names, and a compositor that sends none simply leaves the
/// buffer without one. One value, numbered as wl_output's transform:
/// how the compositor holds the picture in the buffer (Orientation.h).
constexpr uint32_t kMetaVideoTransform = 8;
struct VideoTransformMeta
{
    uint32_t transform;
};

const char* formatName(uint32_t spaFormat)
{
    switch (spaFormat) {
    case SPA_VIDEO_FORMAT_BGRx: return "BGRx";
    case SPA_VIDEO_FORMAT_BGRA: return "BGRA";
    case SPA_VIDEO_FORMAT_RGBx: return "RGBx";
    case SPA_VIDEO_FORMAT_RGBA: return "RGBA";
    default: return "?";
    }
}

} // namespace

struct PortalCapture::Impl
{
    PortalScreenCast portal;
    PortalStream granted;

    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    pw_stream_events events{};

    /// Guards everything the PipeWire thread writes and acquire() reads.
    std::mutex mutex;
    std::condition_variable ready;

    spa_video_info_raw format{};
    bool haveFormat = false;
    bool failed = false;
    std::string failure;

    /// The buffer the stream last handed us, still queued in PipeWire until
    /// release(). One frame held, exactly like the KMS path.
    pw_buffer* held = nullptr;
    KmsFrame frame{};
    bool frameFresh = false;
    /// Buffers handed over since the consumer last took one (click trace).
    int folded = 0;
    bool isDmabuf = false;
    /// A buffer has arrived since start(): what dmabuf() says is the
    /// buffers' own word from then on, not the format's.
    bool sawBuffer = false;
    /// Formats settled on before the first buffer (renegotiatingWithoutPicture).
    int formatsBeforePicture = 0;
    /// The GPU's modifiers, offered to the compositor before shared memory.
    PortalCapture::DmabufOffer offer;
    /// What the formats offered right now include: DMA-BUF before shared
    /// memory, or shared memory alone (setDmabufWhilePointerHidden moves it).
    bool offering = false;
    /// The size and the rates every format carries, kept for a renegotiation.
    bool virtualMonitorFormats = false;
    spa_rectangle sizeDefault = SPA_RECTANGLE(1920, 1080);
    spa_rectangle sizeMin = SPA_RECTANGLE(1, 1);
    spa_rectangle sizeMax = SPA_RECTANGLE(8192, 8192);
    spa_fraction rateDefault = SPA_FRACTION(60, 1);
    spa_fraction rateMin = SPA_FRACTION(0, 1);
    spa_fraction rateMax = SPA_FRACTION(1000, 1);
    /// The GPU the session converts and encodes on.
    std::string renderNode;

    CursorState cursor;
    bool cursorFresh = false;
    /// How the compositor holds the picture in its buffers, by their video
    /// transform: upright but where it hands over a display turned against its
    /// panel as the panel scans it out (xdg-desktop-portal-wlr, Hyprland's).
    /// The last buffer's, which the size follows (width, height).
    Orientation orientation;
    /// The orientation last said in the log.
    Orientation saidOrientation;
    /// An earlier grant to replay, so start() raises no dialog.
    std::string restore;

    /// A virtual monitor's mode; zero width for a real monitor.
    int virtualWidth = 0;
    int virtualHeight = 0;
    int virtualFps = 60;
    /// KWin makes the virtual monitor, under this name (setKwinVirtualOutput).
    std::string kwinName;
    /// Mutter makes it, or records the monitor named here (setMutter).
    bool mutter = false;
    std::string mutterConnector;
    /// The connector is a real monitor of the desktop, not a virtual one.
    bool mutterRealMonitor = false;
    /// One of GNOME's virtual monitors before GNOME 48: its pointer is asked
    /// painted into every picture (MutterScreenCast::embedsPointer).
    bool embedCursor = false;
    /// A portal that hands over no pointer beside the pictures painted it into
    /// them instead (PortalStream::cursorMode): the client must not draw one.
    bool cursorPainted = false;
    /// When acquire() next asks whether Mutter ended the screen cast.
    int64_t nextEndCheckUs = 0;
    /// gamescope's node, read on the session's PipeWire with no handshake at
    /// all (setGamescope); its pointer comes from its Xwayland.
    uint32_t gamescopeNode = 0;
    std::string gamescopeDisplay;
    int gamescopePid = 0;
    XFixesCursor gamescopeCursor;

    int64_t nowUs() const
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static void onStateChanged(void* data, pw_stream_state, pw_stream_state state,
                               const char* error)
    {
        auto* self = static_cast<Impl*>(data);
        if (state == PW_STREAM_STATE_ERROR) {
            log::warning(std::string("[native] portal stream: ") +
                         (error ? error : "failed, no reason given"));
            std::lock_guard<std::mutex> lock(self->mutex);
            self->failed = true;
            self->failure = error ? error : "the stream failed";
            self->ready.notify_all();
        }
    }

    static void onParamChanged(void* data, uint32_t id, const spa_pod* param)
    {
        auto* self = static_cast<Impl*>(data);
        if (!param || id != SPA_PARAM_Format) return;

        uint32_t mediaType = 0, mediaSubtype = 0;
        if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0) return;
        if (mediaType != SPA_MEDIA_TYPE_video || mediaSubtype != SPA_MEDIA_SUBTYPE_raw) return;

        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0) return;
        // A modifier in the format: the compositor took one of the offer's,
        // and hands DMA-BUF (offerDmabuf). None: shared memory. Read from the
        // format itself: spa_video_info_raw::flags and SPA_VIDEO_FLAG_MODIFIER
        // are not in the PipeWire of Ubuntu 22.04 (0.3.48), which builds the
        // packages.
        const bool modifier =
            spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier) != nullptr;

        {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->format = info;
            self->haveFormat = true;
            if (!self->sawBuffer) {
                self->isDmabuf = modifier;
                ++self->formatsBeforePicture;
            }
        }
        self->ready.notify_all();
        char modifierText[40] = "shared memory";
        if (modifier)
            std::snprintf(modifierText, sizeof(modifierText), "DMA-BUF, modifier 0x%llx",
                          static_cast<unsigned long long>(info.modifier));
        log::info("[native] portal stream: " + std::to_string(info.size.width) + "x" +
                  std::to_string(info.size.height) + " " + formatName(info.format) + " at " +
                  std::to_string(info.max_framerate.denom > 0
                                     ? info.max_framerate.num / info.max_framerate.denom
                                     : 0) +
                  " fps max, " + modifierText);

        // Ask for the metadata we want alongside the pixels. The cursor one is
        // what keeps the pointer OUT of the picture — the handshake asked for
        // METADATA mode, and this is where the buffer gets somewhere to put it.
        // Mutter 46 and later want room for a 384×384 cursor: a range that
        // stops below it agrees on no cursor metadata at all, and the pointer
        // never travels (bench §8s).
        uint8_t storage[1024];
        spa_pod_builder builder{};
        spa_pod_builder_init(&builder, storage, sizeof(storage));
        const spa_pod* params[4];
        uint32_t count = 0;
        params[count++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
            SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size,
            SPA_POD_Int(static_cast<int>(sizeof(spa_meta_header)))));
        params[count++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
            SPA_POD_Id(SPA_META_Cursor), SPA_PARAM_META_size,
            SPA_POD_CHOICE_RANGE_Int(cursorMetaSize(384, 384), cursorMetaSize(1, 1),
                                     cursorMetaSize(512, 512))));
        // And how the buffer holds the picture. A portal that hands over a
        // display turned against its panel as the panel scans it out —
        // xdg-desktop-portal-wlr, Hyprland's — says so here, and only to a
        // consumer that asks (issue #54); OBS and Chrome ask the same way.
        // GNOME and KDE turn the picture themselves and leave it out.
        params[count++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
            SPA_POD_Id(kMetaVideoTransform), SPA_PARAM_META_size,
            SPA_POD_Int(static_cast<int>(sizeof(VideoTransformMeta)))));
        // DMA-BUF asked for by name once a modifier is agreed. Shared memory
        // keeps what it always had — no buffer parameter, the compositor's
        // own — except on a stream that moves between the two
        // (setDmabufWhilePointerHidden): PipeWire replaces only the kinds of
        // parameter it is handed again, and the DMA-BUF one left from before
        // fails the shared memory's allocation ("error alloc buffers: Invalid
        // argument", UM790Pro, GNOME 46, 09/10/2026). It is named there too.
        const bool switches = self->embedCursor && !self->offer.renderNode.empty();
        if (modifier || switches)
            params[count++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
                &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_dataType,
                SPA_POD_Int(modifier ? 1 << SPA_DATA_DmaBuf
                                     : (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
        pw_stream_update_params(self->stream, params, count);
    }

    /// Read the cursor metadata a buffer carries, if any. True when the pointer
    /// moved, appeared, went or changed shape.
    bool readCursor(pw_buffer* b)
    {
        auto* meta = static_cast<spa_meta_cursor*>(
            spa_buffer_find_meta_data(b->buffer, SPA_META_Cursor, sizeof(spa_meta_cursor)));
        if (!meta) return false;
        const bool visible = spa_meta_cursor_is_valid(meta);
        if (!visible) {
            if (!cursor.visible) return false;
            cursor.visible = false;
            cursorFresh = true;
            return true;
        }
        const bool moved =
            !cursor.visible || cursor.x != meta->position.x || cursor.y != meta->position.y;
        cursor.visible = true;
        cursor.x = meta->position.x;
        cursor.y = meta->position.y;
        if (moved) cursorFresh = true;

        // The shape only travels when it changes; a zero id means "same as
        // before", which is the common case for thousands of frames.
        if (meta->bitmap_offset == 0) return moved;
        auto* bitmap = SPA_PTROFF(meta, meta->bitmap_offset, spa_meta_bitmap);
        if (!bitmap || bitmap->size.width == 0 || bitmap->size.height == 0) return moved;
        const auto* pixels = SPA_PTROFF(bitmap, bitmap->offset, uint8_t);
        const int w = static_cast<int>(bitmap->size.width);
        const int h = static_cast<int>(bitmap->size.height);
        cursor.width = w;
        cursor.height = h;
        cursor.pixels.assign(static_cast<size_t>(w) * h * 4, 0);
        int inkW = 0, inkH = 0;
        for (int row = 0; row < h; ++row) {
            const uint8_t* src = pixels + static_cast<size_t>(row) * bitmap->stride;
            uint8_t* dst = &cursor.pixels[static_cast<size_t>(row) * w * 4];
            std::memcpy(dst, src, static_cast<size_t>(w) * 4);
            for (int col = 0; col < w; ++col)
                if (dst[col * 4 + 3] != 0) {
                    if (col + 1 > inkW) inkW = col + 1;
                    if (row + 1 > inkH) inkH = row + 1;
                }
        }
        cursor.invert.assign(static_cast<size_t>(w) * h, 0);
        cursor.inkWidth = inkW;
        cursor.inkHeight = inkH;
        ++cursor.shapeVersion;
        cursorFresh = true;
        return true;
    }

    /// PipeWire takes a buffer back — a renegotiation, which GNOME 42 does at
    /// a display mode change even when the size stays. A frame still held
    /// here would point at freed memory, and handing it back later (release,
    /// stop) crashed the session inside pw_stream_queue_buffer (UM790Pro,
    /// 15/09/2026). Forgotten instead: it is no longer ours to return.
    static void onRemoveBuffer(void* data, pw_buffer* b)
    {
        auto* self = static_cast<Impl*>(data);
        std::lock_guard<std::mutex> lock(self->mutex);
        if (self->held != b) return;
        self->held = nullptr;
        self->frameFresh = false;
    }

    static void onProcess(void* data)
    {
        auto* self = static_cast<Impl*>(data);
        pw_buffer* b = pw_stream_dequeue_buffer(self->stream);
        if (!b) return;

        std::unique_lock<std::mutex> lock(self->mutex);
        spa_buffer* buf = b->buffer;
        if (buf->n_datas < 1 || buf->datas[0].chunk == nullptr || buf->datas[0].chunk->size == 0) {
            // An empty buffer is how PipeWire says "nothing new" — and how
            // Mutter sends a pointer that moved over a still screen: its
            // metadata on a buffer with no pixels (bench §8s.4). Not a frame:
            // the one held for the consumer stays held, it may be reading it
            // right now, and this one goes straight back, which is what keeps
            // the stream fed. The pointer it carries is read first.
            const bool pointer = self->readCursor(b);
            pw_stream_queue_buffer(self->stream, b);
            if (pointer) {
                lock.unlock();
                self->ready.notify_all();
            }
            return;
        }

        // One frame held at a time: if the consumer has not taken the last one,
        // this newer one replaces it. Never a queue — the whole engine's rule.
        if (self->held) {
            pw_stream_queue_buffer(self->stream, self->held);
            self->held = nullptr;
        }
        self->folded = self->frameFresh ? self->folded + 1 : 1;

        self->readCursor(b);

        KmsFrame& f = self->frame;
        f = KmsFrame{};
        f.width = static_cast<int>(self->format.size.width);
        f.height = static_cast<int>(self->format.size.height);
        f.fourcc = drmFourcc(self->format.format);
        f.modifier = self->format.modifier;
        f.planeCount = static_cast<int>(buf->n_datas);
        for (uint32_t i = 0; i < buf->n_datas && i < 4; ++i) {
            f.fds[i] = buf->datas[i].type == SPA_DATA_DmaBuf || buf->datas[i].type == SPA_DATA_MemFd
                           ? static_cast<int>(buf->datas[i].fd)
                           : -1;
            f.offsets[i] = buf->datas[i].chunk->offset;
            f.pitches[i] = static_cast<uint32_t>(buf->datas[i].chunk->stride);
        }
        self->isDmabuf = buf->datas[0].type == SPA_DATA_DmaBuf;
        self->sawBuffer = true;
        if (!self->isDmabuf && buf->datas[0].data) {
            f.mapped =
                static_cast<const uint8_t*>(buf->datas[0].data) + buf->datas[0].chunk->offset;
            f.mappedSize = buf->datas[0].chunk->size;
        }

        // When the compositor stamps a time, use it: it is when the frame was
        // produced, not when we noticed — the same distinction KMS makes with
        // its vblank.
        auto* header = static_cast<spa_meta_header*>(
            spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(spa_meta_header)));
        f.presentUs = header && header->pts > 0 ? header->pts / 1000 : self->nowUs();
        f.capturedUs = self->nowUs();
        f.presentRawUs = header && header->pts > 0 ? header->pts / 1000 : 0;
        f.sequence = header ? static_cast<int64_t>(header->seq) : -1;

        // How the buffer holds the picture, where the compositor says (asked
        // for in onParamChanged): a display turned against its panel, handed
        // over as the panel scans it out. The frame is then the picture's
        // size, upright, and the converters stand it up as they read it. The
        // pointer's metadata is left as it comes: no portal known to send a
        // turned picture sends its pointer beside it.
        const auto* transform = static_cast<const VideoTransformMeta*>(
            spa_buffer_find_meta_data(buf, kMetaVideoTransform, sizeof(VideoTransformMeta)));
        self->orientation =
            transform ? orientationFromWayland(transform->transform) : Orientation{};
        if (!self->orientation.upright()) {
            f.orientation = self->orientation;
            f.bufferWidth = f.width;
            f.bufferHeight = f.height;
            orientedSize(f.bufferWidth, f.bufferHeight, f.orientation, f.width, f.height);
        }
        const bool sayOrientation = self->orientation != self->saidOrientation;
        self->saidOrientation = self->orientation;
        const Orientation said = self->orientation;

        self->held = b;
        self->frameFresh = true;
        lock.unlock();
        self->ready.notify_all();
        if (sayOrientation)
            log::info(said.upright()
                          ? std::string("[native] portal stream: the compositor's buffers hold the "
                                        "picture upright again")
                          : "[native] portal stream: the compositor's buffers hold the picture "
                            "turned " +
                                std::to_string(said.degrees()) + "°" +
                                (said.mirrored ? " and mirrored" : "") +
                                " (its video transform) — turned upright in the conversion");
    }

    /// What we accept, into @p params. DMA-BUF first when @p dmabuf, one
    /// format per entry of the offer with its modifiers marked "do not
    /// fixate": the compositor picks the one it can allocate with (PipeWire's
    /// DMA-BUF negotiation). Then shared memory, no modifier named — what a
    /// compositor that takes none of them settles on, and all this route asked
    /// for until 29/09/2026: it exists for the machines that may have no GPU
    /// path at all. The compositor picks; param_changed says what it picked.
    void buildFormats(spa_pod_builder& builder, std::vector<const spa_pod*>& params, bool dmabuf)
    {
        // The size and the rate every entry carries. A virtual monitor's
        // refresh is the maxFramerate the format settles on (GNOME 42, 46 and
        // 48 alike): left free, Mutter's own default wins and makes it 60 Hz,
        // and a 60 Hz monitor streamed at 60 fps at most loses about 40 % of
        // its frames to Mutter's rate limit (bench §8s). So the virtual
        // monitor's entries come first with maxFramerate pinned to its rate —
        // 240 Hz, the Windows model — then again without, for a compositor
        // that cannot go that high. Pinned to the stream's own rate, the same
        // limit costs a 60 fps stream 12 % of its frames, a 120 fps one 8 %
        // (design click-waits §10.1).
        const spa_fraction pinnedMax = rateMax;
        auto addSizeAndRate = [&](bool pinMax) {
            spa_pod_builder_add(&builder, SPA_FORMAT_VIDEO_size,
                                SPA_POD_CHOICE_RANGE_Rectangle(&sizeDefault, &sizeMin, &sizeMax),
                                SPA_FORMAT_VIDEO_framerate,
                                SPA_POD_CHOICE_RANGE_Fraction(&rateDefault, &rateMin, &rateMax), 0);
            if (pinMax)
                spa_pod_builder_add(&builder, SPA_FORMAT_VIDEO_maxFramerate,
                                    SPA_POD_Fraction(&pinnedMax), 0);
        };
        auto addFormats = [&](bool pinMax) {
            if (dmabuf) {
                static const uint32_t kFormats[] = {SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx,
                                                    SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA};
                for (uint32_t spaFormat : kFormats) {
                    const std::vector<uint64_t>* modifiers = nullptr;
                    for (const auto& entry : offer.modifiers)
                        if (entry.first == drmFourcc(spaFormat) && !entry.second.empty())
                            modifiers = &entry.second;
                    if (!modifiers) continue;
                    spa_pod_frame object{};
                    spa_pod_frame choice{};
                    spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Format,
                                                SPA_PARAM_EnumFormat);
                    spa_pod_builder_add(&builder, SPA_FORMAT_mediaType,
                                        SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
                                        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
                                        SPA_POD_Id(spaFormat), 0);
                    spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_modifier,
                                         SPA_POD_PROP_FLAG_MANDATORY |
                                             SPA_POD_PROP_FLAG_DONT_FIXATE);
                    spa_pod_builder_push_choice(&builder, &choice, SPA_CHOICE_Enum, 0);
                    // An enum choice: its default first, then every value it allows.
                    spa_pod_builder_long(&builder, static_cast<int64_t>(modifiers->front()));
                    for (uint64_t modifier : *modifiers)
                        spa_pod_builder_long(&builder, static_cast<int64_t>(modifier));
                    spa_pod_builder_pop(&builder, &choice);
                    addSizeAndRate(pinMax);
                    params.push_back(
                        static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &object)));
                }
            }
            spa_pod_frame object{};
            spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Format,
                                        SPA_PARAM_EnumFormat);
            spa_pod_builder_add(
                &builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
                SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
                                       SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_BGRA,
                                       SPA_VIDEO_FORMAT_RGBA),
                0);
            addSizeAndRate(pinMax);
            params.push_back(static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &object)));
        };
        if (virtualMonitorFormats) addFormats(true);
        addFormats(false);
    }
};

PortalCapture::PortalCapture()
    : d(std::make_unique<Impl>())
{}

PortalCapture::~PortalCapture()
{
    stop();
}

void PortalCapture::setRestoreToken(std::string token)
{
    d->restore = std::move(token);
}

void PortalCapture::setVirtualMonitor(int width, int height, int fps)
{
    d->virtualWidth = width;
    d->virtualHeight = height;
    d->virtualFps = fps > 0 ? fps : 60;
}

void PortalCapture::setKwinVirtualOutput(std::string name)
{
    d->kwinName = std::move(name);
}

void PortalCapture::setMutter(std::string connector, bool realMonitor)
{
    d->mutter = true;
    d->mutterConnector = std::move(connector);
    d->mutterRealMonitor = realMonitor;
}

void PortalCapture::setGamescope(uint32_t node, std::string xDisplay, int pid)
{
    d->gamescopeNode = node;
    d->gamescopeDisplay = std::move(xDisplay);
    d->gamescopePid = pid;
}

void PortalCapture::offerDmabuf(DmabufOffer offer)
{
    d->offer = std::move(offer);
}

void PortalCapture::setRenderNode(std::string renderNode)
{
    d->renderNode = std::move(renderNode);
}

bool PortalCapture::start(std::string& error)
{
    // Before the handshake: without the library the stream the portal grants
    // could not be read, and the user would have answered its dialog for nothing.
    if (!audio::pipeWireAvailable()) {
        error = audio::kPipeWireMissing;
        return false;
    }
    ensurePipeWire();

    // A monitor recorded as it is has a size of its own: nothing is pinned.
    // Nor is gamescope's, made at the stream's size already.
    const bool virtualMonitor = d->virtualWidth > 0 && d->virtualHeight > 0 &&
                                d->mutterConnector.empty() && d->gamescopeNode == 0;
    if (d->gamescopeNode != 0) {
        // Nobody to ask: the node is gamescope's, on the session's PipeWire.
        d->granted = PortalStream{};
        d->granted.nodeId = d->gamescopeNode;
        d->granted.sessionPipeWire = true;
        std::string why;
        const bool cursor = d->gamescopeCursor.start(
            d->gamescopeDisplay,
            [this](const CursorState& state) {
                {
                    std::lock_guard<std::mutex> lock(d->mutex);
                    d->cursor = state;
                    d->cursorFresh = true;
                }
                d->ready.notify_all();
            },
            why);
        if (!cursor) log::warning("[native] gamescope: no pointer to draw for the viewer — " + why);
    } else {
        d->portal.setVirtual(virtualMonitor);
        if (d->mutter)
            d->portal.setMutter(d->mutterConnector, d->virtualWidth, d->virtualHeight,
                                d->virtualFps);
        else if (virtualMonitor && !d->kwinName.empty())
            d->portal.setKwinVirtualOutput(d->kwinName, d->virtualWidth, d->virtualHeight);
        // GNOME's virtual monitors — made by Mutter or its portal, or another
        // stream's recorded as it is — paint the pointer into their DMA-BUF
        // frames up to GNOME 47: it is asked painted into every one, or it
        // blinks. The version is the shell's to say.
        const bool gnomeVirtual = (virtualMonitor && d->kwinName.empty()) ||
                                  (!d->mutterConnector.empty() && !d->mutterRealMonitor);
        d->embedCursor =
            gnomeVirtual && MutterScreenCast::embedsPointer(MutterScreenCast::shellMajor());
        d->portal.setEmbedCursor(d->embedCursor);
        if (!d->portal.start(d->restore, 0, d->granted, error)) return false;
        d->cursorPainted = d->granted.cursorMode == kPortalCursorEmbedded;
    }
    if (!d->granted.valid()) {
        error = "the portal granted nothing usable";
        return false;
    }

    d->loop = pw_thread_loop_new("mw-portal-capture", nullptr);
    if (!d->loop) {
        error = "cannot create the PipeWire loop";
        return false;
    }
    d->context = pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0);
    if (!d->context) {
        error = "cannot create the PipeWire context";
        return false;
    }
    if (pw_thread_loop_start(d->loop) < 0) {
        error = "cannot start the PipeWire loop";
        return false;
    }

    pw_thread_loop_lock(d->loop);
    // The fd the portal handed us: PipeWire takes ownership of it here, which
    // is why start() must not close it afterwards. KWin's output and Mutter's
    // own screen casts stream on the session's own PipeWire, reached the
    // ordinary way.
    d->core = d->granted.pipewireFd >= 0
                  ? pw_context_connect_fd(d->context, d->granted.pipewireFd, nullptr, 0)
                  : pw_context_connect(d->context, nullptr, 0);
    d->granted.pipewireFd = -1;
    if (!d->core) {
        pw_thread_loop_unlock(d->loop);
        error = "cannot connect to the portal's PipeWire remote";
        return false;
    }

    d->events = pw_stream_events{};
    d->events.version = PW_VERSION_STREAM_EVENTS;
    d->events.state_changed = &Impl::onStateChanged;
    d->events.param_changed = &Impl::onParamChanged;
    d->events.process = &Impl::onProcess;
    d->events.remove_buffer = &Impl::onRemoveBuffer;

    d->stream = pw_stream_new(d->core, "MoonlightWeb screen",
                              pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                                                "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));
    if (!d->stream) {
        pw_thread_loop_unlock(d->loop);
        error = "cannot create the PipeWire stream";
        return false;
    }
    pw_stream_add_listener(d->stream, &d->listener, &d->events, d.get());

    if (virtualMonitor) {
        // A virtual monitor has no size of its own: the compositor makes it
        // the size this format settles on. One size, then — the client's —
        // and its cadence as the most it will be asked for.
        const auto w = static_cast<uint32_t>(d->virtualWidth);
        const auto h = static_cast<uint32_t>(d->virtualHeight);
        const auto fps = static_cast<uint32_t>(d->virtualFps);
        d->sizeDefault = d->sizeMin = d->sizeMax = SPA_RECTANGLE(w, h);
        d->rateDefault = d->rateMax = SPA_FRACTION(fps, 1);
    }
    d->virtualMonitorFormats = virtualMonitor;
    // Except a GNOME virtual monitor before GNOME 48 (embedCursor): into its
    // DMA-BUF frames Mutter blits the pointer over a recycled buffer, without
    // repainting what it held, so over a still desktop every place the
    // pointer went stays painted in — trails, and a stale second pointer
    // beside the live one (UM790Pro, GNOME 46, Bruno's test). Its shared-
    // memory frames are painted whole each time, pointer included, and show
    // neither (probe: 0 trails over 90 moves; a pointer in every picture).
    // The price is a copy through system memory, on that route, while the
    // pointer may show (setDmabufWhilePointerHidden).
    d->offering = !d->offer.renderNode.empty() && (!d->embedCursor || d->offer.evenWithTrails);
    if (!d->offer.renderNode.empty() && !d->offering)
        log::info("[native] GNOME's virtual monitor before GNOME 48: shared memory while the "
                  "pointer shows — its DMA-BUF frames keep trails of it");
    if (d->embedCursor && d->offer.evenWithTrails && d->offering)
        log::info("[native] GNOME's virtual monitor before GNOME 48: DMA-BUF all the same, "
                  "trails of the pointer and all (bench: portaldmabuf=1)");
    std::vector<uint8_t> storage(16384);
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, storage.data(), static_cast<uint32_t>(storage.size()));
    std::vector<const spa_pod*> params;
    d->buildFormats(builder, params, d->offering);

    const int res = pw_stream_connect(
        d->stream, PW_DIRECTION_INPUT, d->granted.nodeId,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
        params.data(), static_cast<uint32_t>(params.size()));
    pw_thread_loop_unlock(d->loop);
    if (res < 0) {
        error = std::string("cannot connect to the portal's node: ") + spa_strerror(res);
        return false;
    }

    // The compositor decides the format; until it has, there is no size to
    // report and nothing honest to say about the stream.
    std::unique_lock<std::mutex> lock(d->mutex);
    d->ready.wait_for(lock, std::chrono::seconds(10),
                      [this] { return d->haveFormat || d->failed; });
    if (d->failed) {
        error = d->failure;
        return false;
    }
    if (!d->haveFormat) {
        error = "the portal's stream never negotiated a format";
        return false;
    }
    // The first buffer says what really comes: the session picks its pair on
    // dmabuf(), and a pair built for DMA-BUF cannot read shared memory. A
    // still screen sends one too — the compositor paints the stream's first
    // picture when it starts. Without one in time, the format's word stands.
    d->ready.wait_for(lock, std::chrono::seconds(1), [this] { return d->sawBuffer || d->failed; });
    return true;
}

std::string PortalCapture::restoreToken() const
{
    return d->granted.restoreToken;
}

bool PortalCapture::mutterRefused() const
{
    return d->portal.routeRefused();
}

AcquireStatus PortalCapture::acquire(int timeoutMs, KmsFrame& frame)
{
    // Mutter ends a screen cast of its own accord — the monitor it records
    // went with the stream that made it, the desktop was locked — and the
    // PipeWire stream need not say so: asked a few times a second.
    if (d->mutter && d->nowUs() >= d->nextEndCheckUs) {
        d->nextEndCheckUs = d->nowUs() + 250 * 1000;
        if (d->portal.ended()) {
            std::lock_guard<std::mutex> lock(d->mutex);
            if (!d->failed) {
                d->failed = true;
                d->failure = "GNOME ended the screen cast";
                log::info("[native] GNOME ended the screen cast — the monitor it showed went, or "
                          "the desktop was locked");
            }
        }
    }
    // gamescope's stream only pauses when gamescope goes — its app quit — so
    // gamescope itself is asked, as often.
    if (d->gamescopePid > 0 && d->nowUs() >= d->nextEndCheckUs) {
        d->nextEndCheckUs = d->nowUs() + 250 * 1000;
        if (::kill(d->gamescopePid, 0) != 0 && errno == ESRCH) {
            std::lock_guard<std::mutex> lock(d->mutex);
            if (!d->failed) {
                d->failed = true;
                d->failure = "gamescope went";
                log::info("[native] gamescope went — its app quit, or it was stopped");
            }
        }
    }
    std::unique_lock<std::mutex> lock(d->mutex);
    if (d->failed) {
        log::info("[native] portal stream lost: " + d->failure);
        return AcquireStatus::Lost;
    }
    if (!d->frameFresh && !d->cursorFresh) {
        d->ready.wait_for(lock, std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 1),
                          [this] { return d->frameFresh || d->cursorFresh || d->failed; });
    }
    if (d->failed) {
        log::info("[native] portal stream lost: " + d->failure);
        return AcquireStatus::Lost;
    }
    if (!d->frameFresh) {
        // Nothing new. A pointer that moved is still a visible change, on the
        // same reasoning as the KMS path.
        if (d->cursorFresh) {
            d->cursorFresh = false;
            return AcquireStatus::PointerOnly;
        }
        return AcquireStatus::Timeout;
    }
    d->frameFresh = false;
    d->cursorFresh = false;
    frame = d->frame;
    frame.accumulated = d->folded;
    d->folded = 0;
    return AcquireStatus::Ok;
}

void PortalCapture::release()
{
    std::lock_guard<std::mutex> lock(d->mutex);
    if (!d->held) return;
    pw_stream_queue_buffer(d->stream, d->held);
    d->held = nullptr;
}

void PortalCapture::stop()
{
    // Its callback takes the mutex the stream's does: stopped first.
    d->gamescopeCursor.stop();
    if (d->loop) {
        pw_thread_loop_lock(d->loop);
        if (d->held && d->stream) {
            pw_stream_queue_buffer(d->stream, d->held);
            d->held = nullptr;
        }
        if (d->stream) {
            pw_stream_destroy(d->stream);
            d->stream = nullptr;
        }
        if (d->core) {
            pw_core_disconnect(d->core);
            d->core = nullptr;
        }
        pw_thread_loop_unlock(d->loop);
        pw_thread_loop_stop(d->loop);
    }
    if (d->context) {
        pw_context_destroy(d->context);
        d->context = nullptr;
    }
    if (d->loop) {
        pw_thread_loop_destroy(d->loop);
        d->loop = nullptr;
    }
    if (d->granted.pipewireFd >= 0) {
        ::close(d->granted.pipewireFd);
        d->granted.pipewireFd = -1;
    }
    d->portal.stop();
    d->haveFormat = false;
    d->sawBuffer = false;
    d->formatsBeforePicture = 0;
}

// The picture's size: the buffers', stood upright where the compositor holds
// the picture turned (its video transform, onProcess).
int PortalCapture::width() const
{
    return static_cast<int>(d->orientation.swapsAxes() ? d->format.size.height
                                                       : d->format.size.width);
}

int PortalCapture::height() const
{
    return static_cast<int>(d->orientation.swapsAxes() ? d->format.size.width
                                                       : d->format.size.height);
}

int PortalCapture::refreshMilliHz() const
{
    const spa_fraction& r = d->format.max_framerate;
    if (r.denom == 0) return 60000;
    return static_cast<int>(static_cast<int64_t>(r.num) * 1000 / r.denom);
}

uint32_t PortalCapture::fourcc() const
{
    return drmFourcc(d->format.format);
}

bool PortalCapture::dmabuf() const
{
    return d->isDmabuf;
}

bool PortalCapture::switchesDmabufWithPointer() const
{
    return d->embedCursor && !d->offer.renderNode.empty() && !d->offer.evenWithTrails &&
           d->stream != nullptr;
}

bool PortalCapture::setDmabufWhilePointerHidden(bool hidden)
{
    if (!switchesDmabufWithPointer() || hidden == d->offering) return false;
    // The formats offered again on the live stream: the compositor
    // renegotiates in place — the node, the grant and the monitor stay — and
    // the buffers of the new kind follow (dmabuf() says so from the first).
    std::vector<uint8_t> storage(16384);
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, storage.data(), static_cast<uint32_t>(storage.size()));
    std::vector<const spa_pod*> params;
    pw_thread_loop_lock(d->loop);
    d->buildFormats(builder, params, hidden);
    const int res =
        pw_stream_update_params(d->stream, params.data(), static_cast<uint32_t>(params.size()));
    pw_thread_loop_unlock(d->loop);
    if (res < 0) {
        log::warning(std::string("[native] the portal's formats could not be offered again: ") +
                     spa_strerror(res));
        return false;
    }
    d->offering = hidden;
    log::info(hidden ? "[native] the pointer is hidden — the portal is offered DMA-BUF again"
                     : "[native] the pointer may show — the portal is asked for shared memory");
    return true;
}

bool PortalCapture::renegotiatingWithoutPicture() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    return !d->sawBuffer && d->formatsBeforePicture >= 3;
}

std::string PortalCapture::renderNodePath() const
{
    if (!d->renderNode.empty()) return d->renderNode;
    return d->isDmabuf ? d->offer.renderNode : std::string();
}

DesktopRect PortalCapture::desktopRect() const
{
    // The portal hands over a stream, not a monitor — but for a monitor stream
    // it says where that monitor sits in the compositor's space and how big it
    // is there, in LOGICAL pixels. That is the rectangle an absolute pointer
    // needs, in the very space the compositor spreads it across (issue #18: on
    // a second monitor, the picture at the origin aimed the pointer at the
    // first). Without a position the picture stays at the origin: right on one
    // screen, and the caller's Wayland layout may still place it.
    DesktopRect rect;
    if (d->granted.hasPosition && d->granted.width > 0 && d->granted.height > 0) {
        rect.left = d->granted.x;
        rect.top = d->granted.y;
        rect.right = d->granted.x + d->granted.width;
        rect.bottom = d->granted.y + d->granted.height;
        return rect;
    }
    rect.left = 0;
    rect.top = 0;
    rect.right = width();
    rect.bottom = height();
    return rect;
}

const CursorState& PortalCapture::cursor() const
{
    return d->cursor;
}

bool PortalCapture::cursorInPicture() const
{
    return d->embedCursor || d->cursorPainted;
}

} // namespace mw::native::capture
