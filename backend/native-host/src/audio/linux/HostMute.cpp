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

#include "HostMute.h"

#include "../../core/Log.h"

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
// SPA_PARAM_Route and its SPA_PARAM_ROUTE_* keys live in param.h; there is no
// route.h in libpipewire 0.3.48, which is the oldest this has to build on.
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/result.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace mw::native::audio {

namespace {

/// The node name of the silent output strategy 2 creates. Stable, so a sink
/// left behind by a worker that died mid-session is recognisable — by a human
/// reading `pactl list sinks`, and by us, since creating a second one under the
/// same name would be the confusing outcome.
constexpr const char* kNullSinkName = "moonlightweb-host-muted";
constexpr const char* kDefaultSinkKey = "default.audio.sink";
constexpr const char* kJsonType = "Spa:String:JSON";

/// Long enough that a busy graph answers, short enough that a daemon which
/// never will does not hold a session's start hostage.
constexpr int kRoundTripSeconds = 3;

/// pw_init is process-wide; PipeWireCapture has the same guard for the same
/// reason, and the two are independent — either can be the first here.
void initLibrary()
{
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

std::string quoted(const std::string& name)
{
    return "\"" + name + "\"";
}

std::string defaultSinkJson(const std::string& name)
{
    std::string escaped;
    for (const char c : name) {
        if (c == '"' || c == '\\') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return "{\"name\":\"" + escaped + "\"}";
}

} // namespace

const char* toString(HostMute::Strategy s)
{
    switch (s) {
    case HostMute::Strategy::SinkMute: return "sink mute";
    case HostMute::Strategy::NullSink: return "silent output";
    case HostMute::Strategy::None: break;
    }
    return "none";
}

// ── What one look at the graph tells us ──────────────────────────────────────

namespace {

struct Survey
{
    /// A session manager publishes the "default" metadata object. Without it
    /// nothing here knows which sink the user listens to.
    bool haveMetadata = false;
    /// The raw metadata value, kept as written so release() can put back
    /// exactly what it found rather than a re-serialised equivalent.
    std::string defaultValue;
    std::string sinkName;
    uint32_t sinkId = 0;
    bool sinkFound = false;
    bool monitorCarriesVolume = false;
    /// The sink is one device of a card, and that card has a route for it —
    /// the place a desktop writes a mute. False for a virtual sink, which is
    /// muted on its node instead.
    bool hasRoute = false;
    bool muteKnown = false;
    /// The mute that counts: the route's when there is one, the node's
    /// otherwise. They are the same value once the daemon has mirrored it.
    bool muted = false;
    bool softMuted = false;
};

} // namespace

struct HostMute::Impl
{
    // ── Connection ───────────────────────────────────────────────────────────
    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_registry* registry = nullptr;
    // Referenced by the objects they listen to for the whole connection:
    // members, never locals.
    pw_core_events coreEvents{};
    pw_registry_events registryEvents{};
    pw_metadata_events metadataEvents{};
    pw_node_events nodeEvents{};
    spa_hook coreHook{};
    spa_hook registryHook{};
    spa_hook metadataHook{};
    spa_hook nodeHook{};

    int pendingSeq = 0;
    int doneSeq = -1;
    bool coreFailed = false;

    // ── What the registry told us ────────────────────────────────────────────
    struct SinkGlobal
    {
        uint32_t id;
        std::string name;
    };
    std::vector<SinkGlobal> sinks;
    pw_metadata* metadata = nullptr;
    std::string defaultValue;

    // ── The default sink, bound ──────────────────────────────────────────────
    pw_proxy* node = nullptr;
    bool nodePropsSeen = false;
    bool nodeMonitorCarriesVolume = false;
    bool nodeMuteKnown = false;
    bool nodeMuted = false;
    bool nodeSoftMuted = false;
    /// The card behind the sink, when there is one: `device.id` and the index
    /// of this sink within the card's profile. A virtual sink has neither.
    uint32_t cardId = 0;
    int cardProfileDevice = -1;

    // ── That card's route, which is where a desktop writes a mute ────────────
    pw_proxy* card = nullptr;
    pw_device_events cardEvents{};
    spa_hook cardHook{};
    bool routeFound = false;
    int routeIndex = 0;
    bool routeMuted = false;

    // ── What engage() did, and release() has to undo ─────────────────────────
    Strategy strategy = Strategy::None;
    std::string sinkName;
    /// False when there was nothing to do (already muted, already moved):
    /// release() then has nothing to put back either.
    bool wroteMute = false;
    bool useRoute = false;
    bool savedMute = false;
    bool savedSoftMute = false;
    bool wroteDefault = false;
    std::string savedDefaultValue;
    pw_proxy* nullSink = nullptr;

    ~Impl() { disconnect(); }

    // ── Callbacks, all on the loop thread ────────────────────────────────────

    static void onCoreDone(void* data, uint32_t id, int seq)
    {
        auto* d = static_cast<Impl*>(data);
        if (id != PW_ID_CORE) return;
        d->doneSeq = seq;
        pw_thread_loop_signal(d->loop, false);
    }

    static void onCoreError(void* data, uint32_t id, int /*seq*/, int res, const char* message)
    {
        auto* d = static_cast<Impl*>(data);
        // A round trip that will never complete has to end the wait, or engage()
        // blocks the start of the session for its whole timeout.
        if (id == PW_ID_CORE) {
            d->coreFailed = true;
            log::warning(std::string("[native] audio: PipeWire error while silencing the host: ") +
                         (message ? message : spa_strerror(res)));
            pw_thread_loop_signal(d->loop, false);
        }
    }

    static void onGlobal(void* data, uint32_t id, uint32_t /*permissions*/, const char* type,
                         uint32_t /*version*/, const spa_dict* props)
    {
        auto* d = static_cast<Impl*>(data);
        if (!type || !props) return;

        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
            const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
            if (mediaClass && name && std::strcmp(mediaClass, "Audio/Sink") == 0)
                d->sinks.push_back({id, name});
            return;
        }

        if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && !d->metadata) {
            const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
            // "default" is the one that names the output; "settings" and the
            // rest are somebody else's business.
            if (!name || std::strcmp(name, "default") != 0) return;
            d->metadata = static_cast<pw_metadata*>(
                pw_registry_bind(d->registry, id, type, PW_VERSION_METADATA, 0));
            if (!d->metadata) return;
            d->metadataEvents = pw_metadata_events{};
            d->metadataEvents.version = PW_VERSION_METADATA_EVENTS;
            d->metadataEvents.property = &Impl::onMetadataProperty;
            pw_metadata_add_listener(d->metadata, &d->metadataHook, &d->metadataEvents, d);
        }
    }

    static void onGlobalRemove(void* data, uint32_t id)
    {
        auto* d = static_cast<Impl*>(data);
        for (auto it = d->sinks.begin(); it != d->sinks.end(); ++it) {
            if (it->id == id) {
                d->sinks.erase(it);
                return;
            }
        }
    }

    static int onMetadataProperty(void* data, uint32_t subject, const char* key,
                                  const char* /*type*/, const char* value)
    {
        auto* d = static_cast<Impl*>(data);
        // Subject 0 is the session's own default; a per-object subject is a
        // stream's target, not ours.
        if (subject == 0 && key && std::strcmp(key, kDefaultSinkKey) == 0)
            d->defaultValue = value ? value : "";
        return 0;
    }

    static void onNodeInfo(void* data, const pw_node_info* info)
    {
        auto* d = static_cast<Impl*>(data);
        if (!info || !info->props) return;
        d->nodePropsSeen = true;
        d->nodeMonitorCarriesVolume =
            monitorCarriesVolume(spa_dict_lookup(info->props, "monitor.channel-volumes"));
        // An ALSA (or Bluetooth) sink is one device of one card, and says so.
        // A virtual sink says neither, and is muted on its node instead.
        const char* card = spa_dict_lookup(info->props, PW_KEY_DEVICE_ID);
        const char* index = spa_dict_lookup(info->props, "card.profile.device");
        if (card && index) {
            d->cardId = static_cast<uint32_t>(std::strtoul(card, nullptr, 10));
            d->cardProfileDevice = std::atoi(index);
        }
    }

    static void onCardParam(void* data, int /*seq*/, uint32_t id, uint32_t /*index*/,
                            uint32_t /*next*/, const spa_pod* param)
    {
        auto* d = static_cast<Impl*>(data);
        if (id != SPA_PARAM_Route || !param) return;
        // A card has one route per device (speakers, headphones, HDMI…); only
        // the one carrying our sink is ours to touch.
        const spa_pod_prop* device = spa_pod_find_prop(param, nullptr, SPA_PARAM_ROUTE_device);
        int32_t which = -1;
        if (!device || spa_pod_get_int(&device->value, &which) != 0) return;
        if (which != d->cardProfileDevice) return;

        const spa_pod_prop* index = spa_pod_find_prop(param, nullptr, SPA_PARAM_ROUTE_index);
        int32_t at = 0;
        if (!index || spa_pod_get_int(&index->value, &at) != 0) return;
        d->routeIndex = at;
        d->routeFound = true;

        const spa_pod_prop* props = spa_pod_find_prop(param, nullptr, SPA_PARAM_ROUTE_props);
        if (!props) return;
        const spa_pod_prop* mute = spa_pod_find_prop(&props->value, nullptr, SPA_PROP_mute);
        bool value = false;
        if (mute && spa_pod_get_bool(&mute->value, &value) == 0) d->routeMuted = value;
    }

    static void onNodeParam(void* data, int /*seq*/, uint32_t id, uint32_t /*index*/,
                            uint32_t /*next*/, const spa_pod* param)
    {
        auto* d = static_cast<Impl*>(data);
        if (id != SPA_PARAM_Props || !param) return;
        bool value = false;
        const spa_pod_prop* prop = spa_pod_find_prop(param, nullptr, SPA_PROP_mute);
        if (prop && spa_pod_get_bool(&prop->value, &value) == 0) {
            d->nodeMuteKnown = true;
            d->nodeMuted = value;
        }
        prop = spa_pod_find_prop(param, nullptr, SPA_PROP_softMute);
        if (prop && spa_pod_get_bool(&prop->value, &value) == 0) d->nodeSoftMuted = value;
    }

    // ── Connection and round trips (caller holds the loop lock) ──────────────

    /// Wait until the server has finished everything asked of it so far. False
    /// on timeout or a core error — never an unbounded wait.
    bool roundTrip()
    {
        if (!core) return false;
        doneSeq = -1;
        coreFailed = false;
        pendingSeq = pw_core_sync(core, PW_ID_CORE, 0);
        timespec deadline{};
        pw_thread_loop_get_time(loop, &deadline,
                                static_cast<int64_t>(kRoundTripSeconds) * SPA_NSEC_PER_SEC);
        while (doneSeq != pendingSeq) {
            if (coreFailed) return false;
            if (pw_thread_loop_timed_wait_full(loop, &deadline) < 0) return false;
        }
        return true;
    }

    bool connect(std::string& error)
    {
        initLibrary();
        loop = pw_thread_loop_new("mw-host-mute", nullptr);
        if (!loop) {
            error = "could not create a PipeWire loop";
            return false;
        }
        if (pw_thread_loop_start(loop) < 0) {
            error = "could not start the PipeWire loop";
            return false;
        }

        pw_thread_loop_lock(loop);
        context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
        if (context) core = pw_context_connect(context, nullptr, 0);
        if (!core) {
            pw_thread_loop_unlock(loop);
            error = "no PipeWire daemon for this user";
            return false;
        }

        coreEvents = pw_core_events{};
        coreEvents.version = PW_VERSION_CORE_EVENTS;
        coreEvents.done = &Impl::onCoreDone;
        coreEvents.error = &Impl::onCoreError;
        pw_core_add_listener(core, &coreHook, &coreEvents, this);

        registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        if (!registry) {
            pw_thread_loop_unlock(loop);
            error = "PipeWire refused the registry";
            return false;
        }
        registryEvents = pw_registry_events{};
        registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
        registryEvents.global = &Impl::onGlobal;
        registryEvents.global_remove = &Impl::onGlobalRemove;
        pw_registry_add_listener(registry, &registryHook, &registryEvents, this);

        // First trip: the registry is walked and the metadata bound. Second:
        // the properties of that metadata arrive, the default sink among them.
        const bool ok = roundTrip() && roundTrip();
        pw_thread_loop_unlock(loop);
        if (!ok) {
            error = "PipeWire did not answer in time";
            return false;
        }
        return true;
    }

    void disconnect()
    {
        if (!loop) return;
        pw_thread_loop_lock(loop);
        if (nullSink) {
            // object.linger is false, so destroying the proxy takes the sink
            // with it — the same thing our connection dropping would do.
            pw_proxy_destroy(nullSink);
            nullSink = nullptr;
        }
        if (card) {
            spa_hook_remove(&cardHook);
            pw_proxy_destroy(card);
            card = nullptr;
        }
        if (node) {
            spa_hook_remove(&nodeHook);
            pw_proxy_destroy(node);
            node = nullptr;
        }
        if (metadata) {
            spa_hook_remove(&metadataHook);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(metadata));
            metadata = nullptr;
        }
        if (registry) {
            spa_hook_remove(&registryHook);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
            registry = nullptr;
        }
        if (core) {
            spa_hook_remove(&coreHook);
            pw_core_disconnect(core);
            core = nullptr;
        }
        pw_thread_loop_unlock(loop);
        pw_thread_loop_stop(loop);
        if (context) {
            pw_context_destroy(context);
            context = nullptr;
        }
        pw_thread_loop_destroy(loop);
        loop = nullptr;
        sinks.clear();
        nodePropsSeen = false;
        nodeMuteKnown = false;
        routeFound = false;
        cardProfileDevice = -1;
    }

    /// Bind the sink the metadata names and read what decides the strategy:
    /// whether its monitor carries its volume, and whether it is muted already.
    bool bindDefaultSink(Survey& survey)
    {
        pw_thread_loop_lock(loop);
        for (const SinkGlobal& sink : sinks) {
            if (sink.name != survey.sinkName) continue;
            survey.sinkId = sink.id;
            survey.sinkFound = true;
            break;
        }
        if (!survey.sinkFound) {
            pw_thread_loop_unlock(loop);
            return false;
        }

        node = static_cast<pw_proxy*>(
            pw_registry_bind(registry, survey.sinkId, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
        if (!node) {
            pw_thread_loop_unlock(loop);
            return false;
        }
        nodeEvents = pw_node_events{};
        nodeEvents.version = PW_VERSION_NODE_EVENTS;
        nodeEvents.info = &Impl::onNodeInfo;
        nodeEvents.param = &Impl::onNodeParam;
        pw_node_add_listener(reinterpret_cast<pw_node*>(node), &nodeHook, &nodeEvents, this);
        pw_node_enum_params(reinterpret_cast<pw_node*>(node), 0, SPA_PARAM_Props, 0, 1, nullptr);
        const bool ok = roundTrip();
        pw_thread_loop_unlock(loop);
        if (!ok) return false;

        survey.monitorCarriesVolume = nodeMonitorCarriesVolume;
        survey.muteKnown = nodeMuteKnown;
        survey.muted = nodeMuted;
        survey.softMuted = nodeSoftMuted;
        // The card's route, when the sink has one, is both where the mute goes
        // and the truth about whether it is already muted.
        survey.hasRoute = bindCardRoute();
        if (survey.hasRoute) {
            survey.muted = routeMuted;
            survey.muteKnown = true;
        }
        return nodePropsSeen;
    }

    /// Bind the card behind the sink and find the route this sink plays out of.
    /// False when the sink has no card (a virtual one) or the card has no route
    /// for it — both perfectly ordinary, and both handled by muting the node.
    bool bindCardRoute()
    {
        if (cardProfileDevice < 0) return false;
        pw_thread_loop_lock(loop);
        if (!card) {
            card = static_cast<pw_proxy*>(
                pw_registry_bind(registry, cardId, PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, 0));
            if (!card) {
                pw_thread_loop_unlock(loop);
                return false;
            }
            cardEvents = pw_device_events{};
            cardEvents.version = PW_VERSION_DEVICE_EVENTS;
            cardEvents.param = &Impl::onCardParam;
            pw_device_add_listener(reinterpret_cast<pw_device*>(card), &cardHook, &cardEvents,
                                   this);
        }
        routeFound = false;
        pw_device_enum_params(reinterpret_cast<pw_device*>(card), 0, SPA_PARAM_Route, 0, UINT32_MAX,
                              nullptr);
        const bool ok = roundTrip();
        pw_thread_loop_unlock(loop);
        return ok && routeFound;
    }

    /// The mute a desktop writes: on the card's route, where the sound settings
    /// write it, where the speaker icon reads it, and where a card with a
    /// hardware mute applies it in hardware. The daemon mirrors it down onto
    /// the sink node — measured on the bench, `pactl set-sink-mute` leaves
    /// exactly this trail — so this is one write, not two.
    bool writeRouteMute(bool muted)
    {
        if (!card || !routeFound) return false;
        uint8_t storage[512];
        spa_pod_builder builder{};
        spa_pod_builder_init(&builder, storage, sizeof(storage));
        spa_pod_frame frame[2];
        spa_pod_builder_push_object(&builder, &frame[0], SPA_TYPE_OBJECT_ParamRoute,
                                    SPA_PARAM_Route);
        spa_pod_builder_prop(&builder, SPA_PARAM_ROUTE_index, 0);
        spa_pod_builder_int(&builder, routeIndex);
        spa_pod_builder_prop(&builder, SPA_PARAM_ROUTE_device, 0);
        spa_pod_builder_int(&builder, cardProfileDevice);
        spa_pod_builder_prop(&builder, SPA_PARAM_ROUTE_props, 0);
        spa_pod_builder_push_object(&builder, &frame[1], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
        spa_pod_builder_prop(&builder, SPA_PROP_mute, 0);
        spa_pod_builder_bool(&builder, muted);
        spa_pod_builder_pop(&builder, &frame[1]);
        // Not saved to the card's stored state: this is for the length of a
        // session, and a session must not rewrite what the user will find after
        // a reboot.
        spa_pod_builder_prop(&builder, SPA_PARAM_ROUTE_save, 0);
        spa_pod_builder_bool(&builder, false);
        const auto* pod = static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &frame[0]));

        pw_thread_loop_lock(loop);
        pw_device_set_param(reinterpret_cast<pw_device*>(card), SPA_PARAM_Route, 0, pod);
        const bool ok = roundTrip();
        pw_thread_loop_unlock(loop);
        return ok;
    }

    /// Re-read the route's mute, for the same "is it still ours" test the node
    /// path makes.
    bool readRouteMute(bool& muted)
    {
        if (!bindCardRoute()) return false;
        muted = routeMuted;
        return true;
    }

    /// Re-read the mute of the sink we wrote to. Used at release, where "still
    /// what we left" is the difference between putting a setting back and
    /// overwriting one the user changed.
    bool readMute(bool& muted)
    {
        if (!node) return false;
        pw_thread_loop_lock(loop);
        nodeMuteKnown = false;
        pw_node_enum_params(reinterpret_cast<pw_node*>(node), 0, SPA_PARAM_Props, 0, 1, nullptr);
        const bool ok = roundTrip();
        pw_thread_loop_unlock(loop);
        if (!ok || !nodeMuteKnown) return false;
        muted = nodeMuted;
        return true;
    }

    /// Both halves of a mute, because they are two different things and only
    /// writing both does what the user means (measured on the bench, 08/09):
    ///
    ///   * `softMute` is the stage audioconvert applies to what LEAVES the sink
    ///     for the device. It is downstream of the monitor ports, so it is what
    ///     actually silences the speakers without touching the capture. Written
    ///     alone it would work and stay invisible.
    ///   * `mute` is the one the desktop reads back — `pactl get-sink-mute`,
    ///     the speaker icon, the slider. Written alone it reported "muted" and
    ///     silenced nothing.
    ///
    /// pactl writes both; so do we, and for the same two reasons.
    bool writeMute(bool muted, bool softMuted)
    {
        if (!node) return false;
        uint8_t storage[256];
        spa_pod_builder builder{};
        spa_pod_builder_init(&builder, storage, sizeof(storage));
        const auto* pod = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute, SPA_POD_Bool(muted),
            SPA_PROP_softMute, SPA_POD_Bool(softMuted)));
        pw_thread_loop_lock(loop);
        pw_node_set_param(reinterpret_cast<pw_node*>(node), SPA_PARAM_Props, 0, pod);
        const bool ok = roundTrip();
        pw_thread_loop_unlock(loop);
        return ok;
    }

    /// Create the silent output of strategy 2 and wait for it to be in the
    /// graph under its own name — a proxy that exists proves nothing yet.
    bool createNullSink()
    {
        pw_thread_loop_lock(loop);
        pw_properties* props =
            pw_properties_new(PW_KEY_FACTORY_NAME, "support.null-audio-sink", PW_KEY_NODE_NAME,
                              kNullSinkName, PW_KEY_NODE_DESCRIPTION,
                              "MoonlightWeb (host audio muted)", PW_KEY_MEDIA_CLASS, "Audio/Sink",
                              // Dies with this connection: a worker that is
                              // killed leaves no sink behind.
                              "object.linger", "false", "audio.position", "[FL,FR]",
                              // Its monitor is the tap's source for the rest of
                              // the session; it must not carry a volume of its
                              // own. False is the default — said out loud
                              // because it is load-bearing here.
                              "monitor.channel-volumes", "false", nullptr);
        if (!props) {
            pw_thread_loop_unlock(loop);
            return false;
        }
        nullSink = static_cast<pw_proxy*>(pw_core_create_object(
            core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0));
        pw_properties_free(props);
        if (!nullSink) {
            pw_thread_loop_unlock(loop);
            return false;
        }
        const bool ok = roundTrip();
        bool present = false;
        for (const SinkGlobal& sink : sinks)
            if (sink.name == kNullSinkName) present = true;
        pw_thread_loop_unlock(loop);
        return ok && present;
    }

    bool writeDefaultSink(const std::string& value)
    {
        if (!metadata) return false;
        pw_thread_loop_lock(loop);
        pw_metadata_set_property(metadata, 0, kDefaultSinkKey, value.empty() ? nullptr : kJsonType,
                                 value.empty() ? nullptr : value.c_str());
        const bool ok = roundTrip();
        pw_thread_loop_unlock(loop);
        return ok;
    }

    std::string readDefaultSink()
    {
        pw_thread_loop_lock(loop);
        const bool ok = roundTrip();
        const std::string value = defaultValue;
        pw_thread_loop_unlock(loop);
        return ok ? value : std::string();
    }

    /// Everything the strategy is chosen from, without writing a thing.
    bool survey(Survey& out, std::string& why)
    {
        out.haveMetadata = metadata != nullptr;
        if (!out.haveMetadata) {
            why = "no session manager naming the default output — WirePlumber "
                  "publishes it, the "
                  "retired pipewire-media-session does not";
            return false;
        }
        pw_thread_loop_lock(loop);
        out.defaultValue = defaultValue;
        pw_thread_loop_unlock(loop);
        out.sinkName = sinkNameFromDefaultJson(out.defaultValue);
        if (out.sinkName.empty()) {
            why = "no default output is set";
            return false;
        }
        if (!bindDefaultSink(out)) {
            why = quoted(out.sinkName) + " is named as the default output but is not in the graph";
            return false;
        }
        return true;
    }
};

// ── The class ────────────────────────────────────────────────────────────────

HostMute::HostMute()
    : d(std::make_unique<Impl>())
{}

HostMute::~HostMute()
{
    release();
}

HostMute::Strategy HostMute::strategy() const
{
    return d->strategy;
}

HostMute::Strategy HostMute::available(std::string& why)
{
    Impl impl;
    std::string error;
    if (!impl.connect(error)) {
        why = error;
        return Strategy::None;
    }
    Survey survey;
    if (!impl.survey(survey, why)) return Strategy::None;

    if (!survey.monitorCarriesVolume) {
        why = quoted(survey.sinkName) + " mutes without muting the capture (its monitor is taken "
                                        "before the volume)";
        return Strategy::SinkMute;
    }
    why = quoted(survey.sinkName) + " applies its volume to its own monitor — "
                                    "playback would be moved to a silent output "
                                    "instead";
    return Strategy::NullSink;
}

HostMute::Strategy HostMute::engage(std::string& how)
{
    release();

    std::string error;
    if (!d->connect(error)) {
        how = "the host keeps hearing its audio: " + error;
        d->disconnect();
        return d->strategy;
    }

    Survey survey;
    std::string why;
    if (!d->survey(survey, why)) {
        how = "the host keeps hearing its audio: " + why;
        d->disconnect();
        return d->strategy;
    }
    d->sinkName = survey.sinkName;

    // ── 1. The monitor is upstream of the volume: mute and be done ───────────
    if (!survey.monitorCarriesVolume) {
        if (survey.muteKnown && survey.muted) {
            // Already muted — by the user, or by a session that ended badly.
            // Claim the strategy so the caller reports a silent host, but save
            // nothing: release() must not unmute what it did not mute.
            d->strategy = Strategy::SinkMute;
            how = quoted(survey.sinkName) + " is already muted — left as it is";
            return d->strategy;
        }
        d->savedMute = survey.muted;
        d->savedSoftMute = survey.softMuted;
        d->useRoute = survey.hasRoute;
        if (d->useRoute ? d->writeRouteMute(true) : d->writeMute(true, true)) {
            d->strategy = Strategy::SinkMute;
            d->wroteMute = true;
            how = "speakers muted on " + quoted(survey.sinkName) +
                  (d->useRoute ? "" : " (on its node: it belongs to no card)") +
                  " — the capture keeps hearing the mix, the monitor is taken before "
                  "the volume";
            return d->strategy;
        }
        // A sink that advertises a mute and refuses it is treated like one whose
        // monitor carries the volume: fall through to moving the playback.
        log::warning("[native] audio: " + quoted(survey.sinkName) +
                     " refused a mute — moving playback to a silent output instead");
    }

    // ── 2. Muting would silence the capture too: move the playback ───────────
    if (!d->createNullSink()) {
        how = "the host keeps hearing its audio: PipeWire would not create a "
              "silent output to move "
              "playback to";
        d->disconnect();
        return d->strategy;
    }
    d->savedDefaultValue = survey.defaultValue;
    if (!d->writeDefaultSink(defaultSinkJson(kNullSinkName))) {
        how = "the host keeps hearing its audio: the default output could not be "
              "changed for the "
              "session";
        d->disconnect();
        return d->strategy;
    }
    d->strategy = Strategy::NullSink;
    d->wroteDefault = true;
    how = "playback moved to a silent output for the session (" + quoted(survey.sinkName) +
          " applies its volume to its own monitor, so muting it would mute the "
          "capture too)";
    return d->strategy;
}

void HostMute::release()
{
    if (d->strategy == Strategy::SinkMute && d->wroteMute) {
        bool muted = false;
        // Still muted = still ours to lift. Unmuted meanwhile = the user's
        // doing, and already what they want.
        const bool read = d->useRoute ? d->readRouteMute(muted) : d->readMute(muted);
        if (read && muted) {
            const bool put = d->useRoute ? d->writeRouteMute(d->savedMute)
                                         : d->writeMute(d->savedMute, d->savedSoftMute);
            if (!put)
                log::warning("[native] audio: could not unmute " + quoted(d->sinkName) +
                             " — unmute it from the sound settings");
        }
    } else if (d->strategy == Strategy::NullSink && d->wroteDefault) {
        // Same rule one level up: put the output back only if ours is still the
        // one selected. A user who picked another output mid-session picked it.
        const std::string now = sinkNameFromDefaultJson(d->readDefaultSink());
        if (now == kNullSinkName && !d->writeDefaultSink(d->savedDefaultValue))
            log::warning("[native] audio: could not restore " + quoted(d->sinkName) +
                         " as the default output — pick it again in the sound settings");
    }

    // Takes the silent output down with it, whichever way this went.
    d->disconnect();
    d->strategy = Strategy::None;
    d->wroteMute = false;
    d->useRoute = false;
    d->wroteDefault = false;
    d->savedDefaultValue.clear();
    d->sinkName.clear();
}

} // namespace mw::native::audio
