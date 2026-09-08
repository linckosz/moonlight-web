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

#include "../../core/Log.h"

#include <drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

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
    bool isDmabuf = false;

    CursorState cursor;
    bool cursorFresh = false;
    /// An earlier grant to replay, so start() raises no dialog.
    std::string restore;

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

        {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->format = info;
            self->haveFormat = true;
        }
        self->ready.notify_all();
        log::info("[native] portal stream: " + std::to_string(info.size.width) + "x" +
                  std::to_string(info.size.height) + " " + formatName(info.format) + " at " +
                  std::to_string(info.max_framerate.denom > 0
                                     ? info.max_framerate.num / info.max_framerate.denom
                                     : 0) +
                  " fps max");

        // Ask for the metadata we want alongside the pixels. The cursor one is
        // what keeps the pointer OUT of the picture — the handshake asked for
        // METADATA mode, and this is where the buffer gets somewhere to put it.
        uint8_t storage[1024];
        spa_pod_builder builder{};
        spa_pod_builder_init(&builder, storage, sizeof(storage));
        const spa_pod* params[2];
        params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
            SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size,
            SPA_POD_Int(static_cast<int>(sizeof(spa_meta_header)))));
        params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
            SPA_POD_Id(SPA_META_Cursor), SPA_PARAM_META_size,
            SPA_POD_CHOICE_RANGE_Int(cursorMetaSize(64, 64), cursorMetaSize(1, 1),
                                     cursorMetaSize(256, 256))));
        pw_stream_update_params(self->stream, params, 2);
    }

    /// Read the cursor metadata a buffer carries, if any.
    void readCursor(pw_buffer* b)
    {
        auto* meta = static_cast<spa_meta_cursor*>(
            spa_buffer_find_meta_data(b->buffer, SPA_META_Cursor, sizeof(spa_meta_cursor)));
        if (!meta) return;
        const bool visible = spa_meta_cursor_is_valid(meta);
        if (!visible) {
            if (cursor.visible) {
                cursor.visible = false;
                cursorFresh = true;
            }
            return;
        }
        cursor.visible = true;
        cursor.x = meta->position.x;
        cursor.y = meta->position.y;

        // The shape only travels when it changes; a zero id means "same as
        // before", which is the common case for thousands of frames.
        if (meta->bitmap_offset == 0) return;
        auto* bitmap = SPA_PTROFF(meta, meta->bitmap_offset, spa_meta_bitmap);
        if (!bitmap || bitmap->size.width == 0 || bitmap->size.height == 0) return;
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
    }

    static void onProcess(void* data)
    {
        auto* self = static_cast<Impl*>(data);
        pw_buffer* b = pw_stream_dequeue_buffer(self->stream);
        if (!b) return;

        std::unique_lock<std::mutex> lock(self->mutex);
        // One frame held at a time: if the consumer has not taken the last one,
        // this newer one replaces it. Never a queue — the whole engine's rule.
        if (self->held) {
            pw_stream_queue_buffer(self->stream, self->held);
            self->held = nullptr;
        }

        spa_buffer* buf = b->buffer;
        if (buf->n_datas < 1 || buf->datas[0].chunk == nullptr || buf->datas[0].chunk->size == 0) {
            // An empty buffer is how PipeWire says "nothing new" — it is not a
            // frame, and queuing it straight back is what keeps the stream fed.
            pw_stream_queue_buffer(self->stream, b);
            return;
        }

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

        self->held = b;
        self->frameFresh = true;
        lock.unlock();
        self->ready.notify_all();
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

bool PortalCapture::start(std::string& error)
{
    ensurePipeWire();

    if (!d->portal.start(d->restore, 0, d->granted, error)) return false;
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
    // is why start() must not close it afterwards.
    d->core = pw_context_connect_fd(d->context, d->granted.pipewireFd, nullptr, 0);
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

    d->stream = pw_stream_new(d->core, "MoonlightWeb screen",
                              pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                                                "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));
    if (!d->stream) {
        pw_thread_loop_unlock(d->loop);
        error = "cannot create the PipeWire stream";
        return false;
    }
    pw_stream_add_listener(d->stream, &d->listener, &d->events, d.get());

    // What we accept. No modifiers are named: asking for a specific tiling is
    // how a negotiation fails on a compositor that would happily have given
    // shared memory, and this route exists for the machines that have no GPU
    // path anyway. The compositor picks; param_changed says what it picked.
    uint8_t storage[2048];
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, storage, sizeof(storage));
    spa_rectangle sizeDefault = SPA_RECTANGLE(1920, 1080);
    spa_rectangle sizeMin = SPA_RECTANGLE(1, 1);
    spa_rectangle sizeMax = SPA_RECTANGLE(8192, 8192);
    spa_fraction rateDefault = SPA_FRACTION(60, 1);
    spa_fraction rateMin = SPA_FRACTION(0, 1);
    spa_fraction rateMax = SPA_FRACTION(1000, 1);
    const spa_pod* params[1];
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
        SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
                               SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&sizeDefault, &sizeMin, &sizeMax),
        SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(&rateDefault, &rateMin, &rateMax)));

    const int res = pw_stream_connect(
        d->stream, PW_DIRECTION_INPUT, d->granted.nodeId,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
        params, 1);
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
    return true;
}

std::string PortalCapture::restoreToken() const
{
    return d->granted.restoreToken;
}

AcquireStatus PortalCapture::acquire(int timeoutMs, KmsFrame& frame)
{
    std::unique_lock<std::mutex> lock(d->mutex);
    if (d->failed) return AcquireStatus::Lost;
    if (!d->frameFresh) {
        d->ready.wait_for(lock, std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 1),
                          [this] { return d->frameFresh || d->failed; });
    }
    if (d->failed) return AcquireStatus::Lost;
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
}

int PortalCapture::width() const
{
    return static_cast<int>(d->format.size.width);
}

int PortalCapture::height() const
{
    return static_cast<int>(d->format.size.height);
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

std::string PortalCapture::renderNodePath() const
{
    return {};
}

DesktopRect PortalCapture::desktopRect() const
{
    // The portal names no position on the desktop — it hands over a stream, not
    // a monitor — so the rectangle is the picture itself, at the origin. That
    // is the truth for input mapping too: there is one surface and nothing to
    // its left or above it.
    DesktopRect rect;
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

} // namespace mw::native::capture
