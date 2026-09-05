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

#include "PipeWireCapture.h"

#include "../../core/Log.h"
#include "../AudioInterleave.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

namespace mw::native::audio {
namespace {

constexpr int kRetrySeconds = 2;

/// pw_init is process-wide and idempotent in spirit but not in fact (it
/// re-registers plugins): once per process, whatever the number of sessions.
void initLibrary()
{
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

const char* formatName(uint32_t format)
{
    switch (format) {
    case SPA_AUDIO_FORMAT_F32: return "F32 interleaved";
    case SPA_AUDIO_FORMAT_F32P: return "F32 planar";
    case SPA_AUDIO_FORMAT_S16: return "S16";
    case SPA_AUDIO_FORMAT_S32: return "S32";
    default: return "other";
    }
}

} // namespace

struct PipeWireCapture::Impl
{
    SampleCallback onSamples;

    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    spa_source* retryTimer = nullptr;
    /// Referenced by the stream for its whole life: a member, not a local.
    pw_stream_events events{};

    /// What the graph negotiated — the buffers are read against THIS, never
    /// against what was asked for.
    spa_audio_info_raw format{};
    bool formatKnown = false;
    bool formatUsable = false;

    std::vector<float> scratch;
    std::atomic<int64_t> frames{0};
    int64_t buffers = 0;
    bool streaming = false;
    bool loggedStreaming = false;
    bool loggedError = false;
    bool stopping = false;

    // ── Connection, on the loop thread with the loop locked ──────────────────

    bool connect(std::string& error)
    {
        pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
            // The monitor of an output, never a microphone.
            PW_KEY_STREAM_CAPTURE_SINK, "true", PW_KEY_NODE_NAME, "MoonlightWeb", PW_KEY_APP_NAME,
            "MoonlightWeb",
            // The graph is asked for 5 ms quanta at the stream's rate. It may
            // say no (another client holding a larger one wins); the pacer's
            // queue is sized for a 21 ms default quantum, so that costs nothing.
            PW_KEY_NODE_LATENCY, "240/48000", PW_KEY_NODE_RATE, "1/48000", nullptr);
        if (!props) {
            error = "PipeWire: out of memory building the stream's properties";
            return false;
        }

        events = pw_stream_events{};
        events.version = PW_VERSION_STREAM_EVENTS;
        events.state_changed = &Impl::onStateChanged;
        events.param_changed = &Impl::onParamChanged;
        events.process = &Impl::onProcess;

        // Owns a context and a core connection of its own; no daemon is the
        // one failure that shows here, synchronously.
        stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "MoonlightWeb audio", props,
                                      &events, this);
        if (!stream) {
            const int err = errno;
            error = std::string("PipeWire: ") + std::strerror(err) +
                    " — is the PipeWire daemon running for this user?";
            return false;
        }

        // Interleaved float, 48 kHz, stereo — the pipeline's own format. The
        // adapter converts from the sink's; what it settles on arrives in
        // onParamChanged and is what the buffers are read against.
        spa_audio_info_raw want{};
        want.format = SPA_AUDIO_FORMAT_F32;
        want.rate = 48000;
        want.channels = 2;
        want.position[0] = SPA_AUDIO_CHANNEL_FL;
        want.position[1] = SPA_AUDIO_CHANNEL_FR;

        uint8_t storage[1024];
        spa_pod_builder builder{};
        spa_pod_builder_init(&builder, storage, sizeof(storage));
        const spa_pod* params[1] = {
            spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &want)};

        const int res = pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                                       PW_STREAM_FLAG_MAP_BUFFERS |
                                                                       PW_STREAM_FLAG_RT_PROCESS),
                                          params, 1);
        if (res < 0) {
            error =
                std::string("PipeWire: connecting the capture stream failed: ") + spa_strerror(res);
            pw_stream_destroy(stream);
            stream = nullptr;
            return false;
        }
        return true;
    }

    void armRetry()
    {
        if (!retryTimer || stopping) return;
        timespec when{};
        when.tv_sec = kRetrySeconds;
        pw_loop_update_timer(pw_thread_loop_get_loop(loop), retryTimer, &when, nullptr, false);
    }

    // ── Callbacks (loop thread) ───────────────────────────────────────────────

    static void onStateChanged(void* data, pw_stream_state /*old*/, pw_stream_state state,
                               const char* message)
    {
        auto* d = static_cast<Impl*>(data);
        switch (state) {
        case PW_STREAM_STATE_STREAMING:
            d->streaming = true;
            if (!d->loggedStreaming) {
                d->loggedStreaming = true;
                log::info("[native] audio tap: PipeWire is streaming the default output's monitor");
            }
            break;
        case PW_STREAM_STATE_ERROR: {
            // The one error a Linux desktop actually produces here is "no node
            // available": a graph with no output in it, which is what a
            // PulseAudio machine's PipeWire looks like. Said once, retried
            // quietly — a sink can appear later.
            d->streaming = false;
            const std::string why = message ? message : "unknown error";
            if (!d->loggedError) {
                d->loggedError = true;
                log::warning("[native] audio tap: PipeWire refused the capture stream (" + why +
                             ") — no output to record in the PipeWire graph? On a machine where "
                             "PulseAudio is still the audio server, sound needs pipewire-pulse. "
                             "Retrying every " +
                             std::to_string(kRetrySeconds) + " s, streaming silent meanwhile");
            }
            d->armRetry();
            break;
        }
        case PW_STREAM_STATE_UNCONNECTED:
            // Reached on our own teardown too; only a stream that was up and
            // dropped (daemon restart) is worth a new attempt.
            if (d->streaming && !d->stopping) {
                d->streaming = false;
                log::warning("[native] audio tap: PipeWire stream dropped — reconnecting");
                d->armRetry();
            }
            break;
        default: break;
        }
    }

    static void onParamChanged(void* data, uint32_t id, const spa_pod* param)
    {
        auto* d = static_cast<Impl*>(data);
        if (!param || id != SPA_PARAM_Format) return;
        uint32_t mediaType = 0, mediaSubtype = 0;
        if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0) return;
        if (mediaType != SPA_MEDIA_TYPE_audio || mediaSubtype != SPA_MEDIA_SUBTYPE_raw) return;
        spa_audio_info_raw info{};
        if (spa_format_audio_raw_parse(param, &info) < 0) return;

        d->format = info;
        d->formatKnown = true;
        d->formatUsable = info.format == SPA_AUDIO_FORMAT_F32 && info.rate == 48000 &&
                          info.channels >= 1 && info.channels <= SPA_AUDIO_MAX_CHANNELS;
        log::info("[native] audio tap: PipeWire negotiated " +
                  std::string(formatName(info.format)) + ", " + std::to_string(info.channels) +
                  " channel(s), " + std::to_string(info.rate) + " Hz" +
                  (d->formatUsable ? "" : " — not the pipeline's format, samples are ignored"));
    }

    static void onProcess(void* data)
    {
        auto* d = static_cast<Impl*>(data);
        pw_buffer* b = pw_stream_dequeue_buffer(d->stream);
        if (!b) return;
        const spa_data& sd = b->buffer->datas[0];
        if (sd.data && sd.chunk && d->formatUsable) {
            const uint32_t channels = d->format.channels;
            const size_t frames = sd.chunk->size / (sizeof(float) * channels);
            const auto* pcm = reinterpret_cast<const float*>(static_cast<const uint8_t*>(sd.data) +
                                                             sd.chunk->offset);
            if (frames > 0) {
                if (d->buffers++ == 0)
                    log::info("[native] audio tap: PipeWire delivers " + std::to_string(frames) +
                              " samples per buffer, " + std::to_string(channels) +
                              " channel(s) interleaved");
                if (channels == 2) {
                    d->onSamples(pcm, frames);
                } else {
                    d->scratch.resize(frames * 2);
                    interleavedToStereo(pcm, static_cast<int>(channels), frames, d->scratch.data());
                    d->onSamples(d->scratch.data(), frames);
                }
                d->frames.fetch_add(static_cast<int64_t>(frames), std::memory_order_relaxed);
            }
        }
        pw_stream_queue_buffer(d->stream, b);
    }

    static void onRetry(void* data, uint64_t /*expirations*/)
    {
        auto* d = static_cast<Impl*>(data);
        if (d->stopping) return;
        // Not from inside a stream callback (PipeWire forbids re-entering the
        // stream there); the timer is the loop's own tick, so it is safe to
        // rebuild the stream whole.
        if (d->stream) {
            pw_stream_destroy(d->stream);
            d->stream = nullptr;
        }
        std::string error;
        if (!d->connect(error)) {
            log::warning("[native] audio tap: " + error);
            d->armRetry();
        }
    }
};

PipeWireCapture::PipeWireCapture(SampleCallback onSamples)
    : d(std::make_unique<Impl>())
{
    d->onSamples = std::move(onSamples);
}

PipeWireCapture::~PipeWireCapture()
{
    stop();
}

bool PipeWireCapture::start(std::string& error)
{
    if (d->loop) return true;
    if (!d->onSamples) {
        error = "no sample callback";
        return false;
    }
    initLibrary();

    d->stopping = false;
    d->loop = pw_thread_loop_new("mw-audio-tap", nullptr);
    if (!d->loop) {
        error = "PipeWire: could not create the capture loop";
        return false;
    }
    if (pw_thread_loop_start(d->loop) < 0) {
        error = "PipeWire: could not start the capture loop";
        pw_thread_loop_destroy(d->loop);
        d->loop = nullptr;
        return false;
    }

    pw_thread_loop_lock(d->loop);
    const bool connected = d->connect(error);
    if (connected) {
        d->retryTimer =
            pw_loop_add_timer(pw_thread_loop_get_loop(d->loop), &Impl::onRetry, d.get());
        log::info("[native] audio tap: PipeWire " + libraryVersion() +
                  ", capture of the default output's monitor requested (48 kHz stereo)");
    }
    pw_thread_loop_unlock(d->loop);

    if (!connected) {
        stop();
        return false;
    }
    return true;
}

void PipeWireCapture::stop()
{
    if (!d->loop) return;
    pw_thread_loop_lock(d->loop);
    d->stopping = true;
    if (d->retryTimer) {
        pw_loop_destroy_source(pw_thread_loop_get_loop(d->loop), d->retryTimer);
        d->retryTimer = nullptr;
    }
    if (d->stream) {
        // Destroys the core and context pw_stream_new_simple created with it.
        pw_stream_destroy(d->stream);
        d->stream = nullptr;
    }
    pw_thread_loop_unlock(d->loop);
    pw_thread_loop_stop(d->loop);
    pw_thread_loop_destroy(d->loop);
    d->loop = nullptr;

    if (d->buffers > 0)
        log::info("[native] audio tap: " + std::to_string(d->frames.load()) + " samples in " +
                  std::to_string(d->buffers) + " buffers from PipeWire");
    d->streaming = false;
    d->formatKnown = false;
    d->formatUsable = false;
}

int64_t PipeWireCapture::framesCaptured() const
{
    return d->frames.load(std::memory_order_relaxed);
}

std::string PipeWireCapture::libraryVersion()
{
    const char* v = pw_get_library_version();
    return v ? v : "?";
}

} // namespace mw::native::audio
