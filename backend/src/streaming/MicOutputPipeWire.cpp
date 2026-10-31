/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 MoonlightWeb contributors
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

// The viewer's microphone on Linux, through PipeWire (pipewire-pulse serves the
// PulseAudio apps from the same graph).
//
// Two objects. The device the apps see is a virtual source, "MoonlightWeb
// Microphone" (support.null-audio-sink, media.class Audio/Source/Virtual), made
// the first time and left in the graph with object.linger: a stream ends with
// its worker process, and a device that came and went with it would leave an
// app recording from nothing after every relaunch or quality switch. Into it,
// per session, a playback stream this file links by hand (link-factory) rather
// than leaving to the session manager: nothing then depends on WirePlumber's
// policy for virtual sources (WirePlumber 0.5 answers "no target node
// available" to a playback stream aimed at one), and a stream without its
// target can never fall back to the speakers. The session manager still gives
// the unlinked stream its port, a moment after the stream is up: the link is
// made when that port appears in the registry.
//
// libpipewire is opened with dlopen, like the native host's (LazyLibrary.h says
// why), but through a table of our own: the native host defines extern "C"
// forwarders named pw_* in this same binary, and a second set would collide.

#include "MicJitter.h"
#include "MicOutput.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <dlfcn.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <vector>

namespace mw::mic {
namespace {

constexpr const char* kNodeName = "moonlightweb_mic";
constexpr const char* kNodeDescription = "MoonlightWeb Microphone";
constexpr int kRoundtripSeconds = 2;

/// The library's exports this file calls; the prototypes are the headers' own.
struct Api
{
    bool loaded = false;
    decltype(&::pw_init) init = nullptr;
    decltype(&::pw_thread_loop_new) loopNew = nullptr;
    decltype(&::pw_thread_loop_destroy) loopDestroy = nullptr;
    decltype(&::pw_thread_loop_start) loopStart = nullptr;
    decltype(&::pw_thread_loop_stop) loopStop = nullptr;
    decltype(&::pw_thread_loop_lock) loopLock = nullptr;
    decltype(&::pw_thread_loop_unlock) loopUnlock = nullptr;
    decltype(&::pw_thread_loop_signal) loopSignal = nullptr;
    decltype(&::pw_thread_loop_timed_wait) loopTimedWait = nullptr;
    decltype(&::pw_thread_loop_get_loop) loopGet = nullptr;
    decltype(&::pw_context_new) contextNew = nullptr;
    decltype(&::pw_context_destroy) contextDestroy = nullptr;
    decltype(&::pw_context_connect) contextConnect = nullptr;
    decltype(&::pw_core_disconnect) coreDisconnect = nullptr;
    decltype(&::pw_proxy_destroy) proxyDestroy = nullptr;
    decltype(&::pw_properties_new) propertiesNew = nullptr;
    decltype(&::pw_properties_free) propertiesFree = nullptr;
    decltype(&::pw_stream_new) streamNew = nullptr;
    decltype(&::pw_stream_destroy) streamDestroy = nullptr;
    decltype(&::pw_stream_add_listener) streamAddListener = nullptr;
    decltype(&::pw_stream_connect) streamConnect = nullptr;
    decltype(&::pw_stream_get_node_id) streamNodeId = nullptr;
    decltype(&::pw_stream_dequeue_buffer) streamDequeue = nullptr;
    decltype(&::pw_stream_queue_buffer) streamQueue = nullptr;
};

const Api& api()
{
    static Api a;
    static std::once_flag once;
    std::call_once(once, [] {
        // The same instance the native host opens: dlopen counts references.
        void* h = ::dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!h) return;
        bool all = true;
        auto bind = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(::dlsym(h, name));
            all = all && fn != nullptr;
        };
        bind(a.init, "pw_init");
        bind(a.loopNew, "pw_thread_loop_new");
        bind(a.loopDestroy, "pw_thread_loop_destroy");
        bind(a.loopStart, "pw_thread_loop_start");
        bind(a.loopStop, "pw_thread_loop_stop");
        bind(a.loopLock, "pw_thread_loop_lock");
        bind(a.loopUnlock, "pw_thread_loop_unlock");
        bind(a.loopSignal, "pw_thread_loop_signal");
        bind(a.loopTimedWait, "pw_thread_loop_timed_wait");
        bind(a.loopGet, "pw_thread_loop_get_loop");
        bind(a.contextNew, "pw_context_new");
        bind(a.contextDestroy, "pw_context_destroy");
        bind(a.contextConnect, "pw_context_connect");
        bind(a.coreDisconnect, "pw_core_disconnect");
        bind(a.proxyDestroy, "pw_proxy_destroy");
        bind(a.propertiesNew, "pw_properties_new");
        bind(a.propertiesFree, "pw_properties_free");
        bind(a.streamNew, "pw_stream_new");
        bind(a.streamDestroy, "pw_stream_destroy");
        bind(a.streamAddListener, "pw_stream_add_listener");
        bind(a.streamConnect, "pw_stream_connect");
        bind(a.streamNodeId, "pw_stream_get_node_id");
        bind(a.streamDequeue, "pw_stream_dequeue_buffer");
        bind(a.streamQueue, "pw_stream_queue_buffer");
        // Guarded inside the library (initialized / init_count, 0.3.48 and 1.x
        // alike), so the native host's own call costs nothing here.
        if (all) a.init(nullptr, nullptr);
        a.loaded = all;
    });
    return a;
}

/// Where libpipewire will look for the daemon, as it would itself work it out.
std::string socketPath()
{
    const char* remote = std::getenv("PIPEWIRE_REMOTE");
    std::string name = remote && *remote ? remote : "pipewire-0";
    if (!name.empty() && name[0] == '/') return name;
    for (const char* var : {"PIPEWIRE_RUNTIME_DIR", "XDG_RUNTIME_DIR", "USERPROFILE"}) {
        const char* dir = std::getenv(var);
        if (dir && *dir) return std::string(dir) + "/" + name;
    }
    return {};
}

class PipeWireOutput final : public IMicOutput
{
public:
    explicit PipeWireOutput(LogFn log)
        : m_Log(std::move(log))
    {}
    ~PipeWireOutput() override { stop(); }

    bool start(JitterRing& ring, std::string& why) override
    {
        if (m_Loop) return true;
        const Api& pw = api();
        if (!pw.loaded) {
            why = "libpipewire-0.3 is not installed on this machine";
            return false;
        }
        m_Ring = &ring;
        m_Loop = pw.loopNew("mw-mic", nullptr);
        if (!m_Loop || pw.loopStart(m_Loop) < 0) {
            why = "PipeWire: could not start the microphone's loop";
            if (m_Loop) pw.loopDestroy(m_Loop);
            m_Loop = nullptr;
            return false;
        }
        pw.loopLock(m_Loop);
        why = connectLocked();
        pw.loopUnlock(m_Loop);
        if (!why.empty()) {
            stop();
            return false;
        }
        return true;
    }

    void stop() override
    {
        const Api& pw = api();
        if (!m_Loop) return;
        pw.loopLock(m_Loop);
        if (m_Link) pw.proxyDestroy(m_Link);
        m_Link = nullptr;
        if (m_Registry) {
            spa_hook_remove(&m_RegistryHook);
            pw.proxyDestroy(reinterpret_cast<pw_proxy*>(m_Registry));
        }
        m_Registry = nullptr;
        if (m_Stream) pw.streamDestroy(m_Stream);
        m_Stream = nullptr;
        if (m_Core) pw.coreDisconnect(m_Core);
        m_Core = nullptr;
        if (m_Context) pw.contextDestroy(m_Context);
        m_Context = nullptr;
        pw.loopUnlock(m_Loop);
        pw.loopStop(m_Loop);
        pw.loopDestroy(m_Loop);
        m_Loop = nullptr;
    }

    std::string deviceName() const override { return kNodeDescription; }

private:
    // ── Setup, on the caller's thread with the loop locked ───────────────────

    std::string connectLocked()
    {
        const Api& pw = api();
        m_Context = pw.contextNew(pw.loopGet(m_Loop), nullptr, 0);
        if (!m_Context) return "PipeWire: could not create a context";
        m_Core = pw.contextConnect(m_Context, nullptr, 0);
        if (!m_Core)
            return "no PipeWire daemon answers for this user (" + socketPath() +
                   ") — the microphone needs PipeWire, or pipewire-pulse on a PulseAudio desktop";
        m_CoreEvents.version = PW_VERSION_CORE_EVENTS;
        m_CoreEvents.done = &PipeWireOutput::onCoreDone;
        m_CoreEvents.error = &PipeWireOutput::onCoreError;
        pw_core_add_listener(m_Core, &m_CoreHook, &m_CoreEvents, this);

        // One registry for the session: it finds the device, then the feed's
        // port once the session manager has given it one.
        m_Registry = pw_core_get_registry(m_Core, PW_VERSION_REGISTRY, 0);
        if (!m_Registry) return "PipeWire: could not read the graph";
        m_RegistryEvents.version = PW_VERSION_REGISTRY_EVENTS;
        m_RegistryEvents.global = &PipeWireOutput::onGlobal;
        pw_registry_add_listener(m_Registry, &m_RegistryHook, &m_RegistryEvents, this);
        roundtripLocked();

        if (m_NodeId == SPA_ID_INVALID) {
            if (!createNodeLocked())
                return "PipeWire refused to create \"" + std::string(kNodeDescription) + "\"";
            if (m_NodeId == SPA_ID_INVALID)
                return "PipeWire created \"" + std::string(kNodeDescription) +
                       "\" but it never appeared in the graph";
            if (m_Log)
                m_Log(std::string("created the virtual source \"") + kNodeDescription + "\"");
        }
        return connectStreamLocked();
    }

    /// One round trip: every event the daemon had queued has been delivered.
    bool roundtripLocked()
    {
        const Api& pw = api();
        m_Done = false;
        m_PendingSeq = pw_core_sync(m_Core, PW_ID_CORE, 0);
        while (!m_Done)
            if (pw.loopTimedWait(m_Loop, kRoundtripSeconds) != 0) return false;
        return true;
    }

    bool createNodeLocked()
    {
        const Api& pw = api();
        pw_properties* props = pw.propertiesNew(
            "factory.name", "support.null-audio-sink", PW_KEY_NODE_NAME, kNodeName,
            PW_KEY_NODE_DESCRIPTION, kNodeDescription, PW_KEY_MEDIA_CLASS, "Audio/Source/Virtual",
            "audio.position", "MONO", PW_KEY_OBJECT_LINGER, "true", nullptr);
        if (!props) return false;
        auto* proxy = static_cast<pw_proxy*>(pw_core_create_object(
            m_Core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0));
        pw.propertiesFree(props);
        if (!proxy) return false;
        // Its global reaches onGlobal within the round trip.
        const bool ok = roundtripLocked();
        // Lingering: the node outlives this proxy, and this process.
        pw.proxyDestroy(proxy);
        return ok;
    }

    /// The feed into the device, once both ends are known. Loop thread.
    void linkWhenReady()
    {
        if (m_Link || m_StreamNode == SPA_ID_INVALID || m_NodeId == SPA_ID_INVALID) return;
        if (std::find(m_OutPortNodes.begin(), m_OutPortNodes.end(), m_StreamNode) ==
            m_OutPortNodes.end())
            return;
        const Api& pw = api();
        const std::string out = std::to_string(m_StreamNode);
        const std::string in = std::to_string(m_NodeId);
        pw_properties* props =
            pw.propertiesNew(PW_KEY_LINK_OUTPUT_NODE, out.c_str(), PW_KEY_LINK_INPUT_NODE,
                             in.c_str(), PW_KEY_OBJECT_LINGER, "false", nullptr);
        if (!props) return;
        m_Link = static_cast<pw_proxy*>(pw_core_create_object(
            m_Core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));
        pw.propertiesFree(props);
        if (m_Log)
            m_Log(m_Link ? "feed linked into \"" + std::string(kNodeDescription) + "\""
                         : std::string("could not link the feed to the device"));
    }

    std::string connectStreamLocked()
    {
        const Api& pw = api();
        pw_properties* props = pw.propertiesNew(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE,
            "Communication", PW_KEY_NODE_NAME, "moonlightweb_mic_feed", PW_KEY_NODE_DESCRIPTION,
            "MoonlightWeb microphone feed", PW_KEY_APP_NAME, "MoonlightWeb",
            // Linked below, by hand: the session manager is told to keep off.
            PW_KEY_NODE_AUTOCONNECT, "false", "node.dont-reconnect", "true", PW_KEY_NODE_LATENCY,
            "480/48000", PW_KEY_NODE_RATE, "1/48000", nullptr);
        if (!props) return "PipeWire: out of memory";
        m_Stream = pw.streamNew(m_Core, "MoonlightWeb microphone", props);
        if (!m_Stream) return "PipeWire: could not create the microphone's stream";

        m_StreamEvents.version = PW_VERSION_STREAM_EVENTS;
        m_StreamEvents.state_changed = &PipeWireOutput::onStateChanged;
        m_StreamEvents.process = &PipeWireOutput::onProcess;
        pw.streamAddListener(m_Stream, &m_StreamHook, &m_StreamEvents, this);

        spa_audio_info_raw want{};
        want.format = SPA_AUDIO_FORMAT_F32;
        want.rate = 48000;
        want.channels = 1;
        want.position[0] = SPA_AUDIO_CHANNEL_MONO;
        uint8_t storage[1024];
        spa_pod_builder builder{};
        spa_pod_builder_init(&builder, storage, sizeof(storage));
        const spa_pod* params[1] = {
            spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &want)};
        const int res = pw.streamConnect(
            m_Stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
            params, 1);
        if (res < 0)
            return std::string("PipeWire: connecting the microphone's stream failed: ") +
                   spa_strerror(res);
        return {};
    }

    // ── Callbacks (loop thread) ──────────────────────────────────────────────

    static void onCoreDone(void* data, uint32_t id, int seq)
    {
        auto* self = static_cast<PipeWireOutput*>(data);
        if (id != PW_ID_CORE || seq != self->m_PendingSeq) return;
        self->m_Done = true;
        api().loopSignal(self->m_Loop, false);
    }

    static void onCoreError(void* data, uint32_t id, int /*seq*/, int res, const char* message)
    {
        auto* self = static_cast<PipeWireOutput*>(data);
        if (self->m_Log)
            self->m_Log("PipeWire error on object " + std::to_string(id) + ": " +
                        (message ? message : spa_strerror(res)));
        // A failed creation answers with an error and never a done: wake the
        // round trip rather than let it time out.
        if (id == PW_ID_CORE) {
            self->m_Done = true;
            api().loopSignal(self->m_Loop, false);
        }
    }

    static void onGlobal(void* data, uint32_t id, uint32_t /*permissions*/, const char* type,
                         uint32_t /*version*/, const spa_dict* props)
    {
        auto* self = static_cast<PipeWireOutput*>(data);
        if (!props) return;
        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
            if (name && std::strcmp(name, kNodeName) == 0) self->m_NodeId = id;
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
            const char* dir = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
            const char* node = spa_dict_lookup(props, PW_KEY_NODE_ID);
            if (!dir || !node || std::strcmp(dir, "out") != 0) return;
            self->m_OutPortNodes.push_back(static_cast<uint32_t>(std::strtoul(node, nullptr, 10)));
            self->linkWhenReady();
        }
    }

    static void onStateChanged(void* data, pw_stream_state /*old*/, pw_stream_state state,
                               const char* error)
    {
        auto* self = static_cast<PipeWireOutput*>(data);
        if (state == PW_STREAM_STATE_ERROR) {
            if (self->m_Log) self->m_Log(std::string("stream error: ") + (error ? error : "?"));
            return;
        }
        // The stream's node has its id from PAUSED on; its port follows.
        if (state != PW_STREAM_STATE_PAUSED || self->m_StreamNode != SPA_ID_INVALID) return;
        self->m_StreamNode = api().streamNodeId(self->m_Stream);
        self->linkWhenReady();
    }

    static void onProcess(void* data)
    {
        auto* self = static_cast<PipeWireOutput*>(data);
        const Api& pw = api();
        pw_buffer* b = pw.streamDequeue(self->m_Stream);
        if (!b) return;
        spa_data& d = b->buffer->datas[0];
        if (d.data && d.chunk) {
            size_t frames = d.maxsize / sizeof(float);
#if PW_CHECK_VERSION(0, 3, 49)
            // How much the graph wants this cycle; older libraries do not say.
            if (b->requested > 0) frames = std::min<size_t>(frames, b->requested);
#endif
            self->m_Ring->pull(static_cast<float*>(d.data), frames);
            d.chunk->offset = 0;
            d.chunk->stride = sizeof(float);
            d.chunk->size = static_cast<uint32_t>(frames * sizeof(float));
        }
        pw.streamQueue(self->m_Stream, b);
    }

    const LogFn m_Log;
    JitterRing* m_Ring = nullptr;
    pw_thread_loop* m_Loop = nullptr;
    pw_context* m_Context = nullptr;
    pw_core* m_Core = nullptr;
    pw_stream* m_Stream = nullptr;
    pw_registry* m_Registry = nullptr;
    pw_proxy* m_Link = nullptr;
    spa_hook m_CoreHook{};
    spa_hook m_StreamHook{};
    spa_hook m_RegistryHook{};
    pw_core_events m_CoreEvents{};
    pw_stream_events m_StreamEvents{};
    pw_registry_events m_RegistryEvents{};
    /// The device's node, and the feed's.
    uint32_t m_NodeId = SPA_ID_INVALID;
    uint32_t m_StreamNode = SPA_ID_INVALID;
    /// Nodes seen with an output port: the feed's arrives after it is up.
    std::vector<uint32_t> m_OutPortNodes;
    int m_PendingSeq = 0;
    bool m_Done = false;
};

} // namespace

std::unique_ptr<IMicOutput> makeOutput(const std::string& /*device*/, LogFn log)
{
    return std::make_unique<PipeWireOutput>(std::move(log));
}

std::string unavailable(const std::string& /*device*/)
{
    if (!api().loaded) return "libpipewire-0.3 is not installed on this machine";
    const std::string socket = socketPath();
    struct stat st{};
    if (socket.empty() || ::stat(socket.c_str(), &st) != 0)
        return "no PipeWire daemon to play the microphone into (" +
               (socket.empty() ? std::string("XDG_RUNTIME_DIR is not set") : socket) +
               "). In a container, mount the host's PipeWire socket — see docker/README.md";
    return {};
}

} // namespace mw::mic
