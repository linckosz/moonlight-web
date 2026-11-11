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

#include "mw/native/HidPassthrough.h"

#include "core/Log.h"
#include "input/HidDescriptor.h"
#include "input/HidFfbJournal.h"
#include "input/HidPid.h"
#include "input/VirtualHid.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace mw::native {

namespace {

int64_t steadyMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string idsOf(uint16_t vendorId, uint16_t productId)
{
    char ids[16];
    std::snprintf(ids, sizeof ids, "%04x:%04x", vendorId, productId);
    return ids;
}

/// A refused device, named, with the descriptor built for it: the device's
/// report layout and nothing else (no serial number), so a refusal reported
/// from a log can be replayed without the device.
void logRefusal(int slot, const HidDeviceInfo& device, const std::vector<uint8_t>& descriptor,
                const std::string& why)
{
    std::string hex;
    hex.reserve(descriptor.size() * 2);
    for (const uint8_t b : descriptor) {
        char two[3];
        std::snprintf(two, sizeof two, "%02x", b);
        hex += two;
    }
    log::warning("[hid] slot " + std::to_string(slot) + ": " + device.name + " " +
                 idsOf(device.vendorId, device.productId) + " refused (" + why + "), descriptor " +
                 std::to_string(descriptor.size()) + " bytes: " + hex);
}

} // namespace

/// Where a device's requests and force feedback go, and what of them it must
/// remember. Shared by the device's backend threads and whoever holds the
/// device: a stream's HidPassthrough (owner), or nobody while it is parked,
/// when the journal alone hears what the game does. Everything here is read
/// and written under `mutex`, the sinks called under it too: an owner that
/// lets go waits for a call in flight, and never hears one after.
struct HidPassthrough::Route
{
    std::mutex mutex;
    std::unique_ptr<input::hid::PidEngine> engine;
    input::hid::FfbJournal journal;
    HidPassthrough* owner = nullptr;
    int slot = 0;
    Clock clock = steadyMs;
    // What the game asked since the watchdog last looked, for the log: output
    // reports, set and get feature, and the gets answered with nothing.
    uint32_t outputs = 0, sets = 0, gets = 0, emptyGets = 0;
    // Whether the game asked anything in the last 5 s the log saw.
    bool asking = false;
    // The next requests logged one by one (a stream's first, the game's
    // force feedback setting up), and the last output's report id.
    int traceLeft = 0;
    int lastOutputId = -1;
};

namespace {

std::string hexHead(const std::vector<uint8_t>& data)
{
    std::string out;
    char b[4];
    for (size_t i = 0; i < data.size() && i < 12; ++i) {
        std::snprintf(b, sizeof b, "%02x", data[i]);
        out += b;
    }
    if (data.size() > 12) out += "…";
    return out;
}

} // namespace

struct HidPassthrough::Slot
{
    std::unique_ptr<input::IVirtualHid> device;
    std::shared_ptr<Route> route;
    input::VirtualHidIdentity identity;
    uint8_t pidFirstId = 0;
    input::hid::Parsed parsed;
    bool numbered = false;
    std::map<uint8_t, size_t> inputBytes; // per report id, without the id byte
    std::map<uint8_t, std::vector<uint8_t>> last;
    // The first report of each id: where its pedals and throttles rest (restReport).
    std::map<uint8_t, std::vector<uint8_t>> first;
    std::map<uint8_t, uint16_t> lastSeq;
    int64_t lastAtMs = 0;
    bool resting = true;
    // Taken back from the parking: attached() replays its force feedback.
    bool fromParking = false;

    void describe(const std::vector<uint8_t>& descriptor)
    {
        parsed = input::hid::parse(descriptor);
        numbered = !(parsed.reportIds.size() == 1 && parsed.reportIds.front() == 0);
        for (uint8_t id : parsed.reportIds) {
            const size_t n = input::hid::reportBytes(parsed, input::hid::Kind::Input, id);
            if (n) inputBytes[id] = n - (numbered ? 1 : 0);
        }
    }

    /// Every input report back to rest, once (Bruno's rule of 04/10).
    void rest()
    {
        resting = true;
        for (auto& [id, report] : last) {
            std::vector<uint8_t> r = input::hid::restReport(parsed, id, report, first[id]);
            if (r == report) continue;
            report = r;
            if (numbered) r.insert(r.begin(), id);
            device->input(r.data(), r.size());
        }
    }

    bool sameAs(const std::string& name, uint16_t vendorId, uint16_t productId, uint16_t version,
                const std::vector<uint8_t>& descriptor) const
    {
        return identity.vendorId == vendorId && identity.productId == productId &&
               identity.version == version && identity.name == name &&
               identity.descriptor == descriptor;
    }
};

// ── HidParking ──────────────────────────────────────────────────────────────

struct HidParking::Entry
{
    std::unique_ptr<HidPassthrough::Slot> slot;
    int64_t keepUntilMs = 0;
    int64_t unclaimedUntilMs = 0; // 0: no stream waits on it
    uint64_t batch = 0;           // handed out in this batch; 0: here
};

HidParking::HidParking(Clock clock, bool ownThread)
    : m_clock(clock ? std::move(clock) : Clock(steadyMs))
{
    if (ownThread) m_thread = std::thread([this] { run(); });
}

HidParking::~HidParking()
{
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    std::vector<std::unique_ptr<Entry>> gone;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        gone.swap(m_entries);
    }
}

HidParking& HidParking::process()
{
    static HidParking parking(nullptr, true);
    return parking;
}

size_t HidParking::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}

void HidParking::run()
{
    int rounds = 0;
    while (!m_stop) {
        for (int i = 0; i < 10 && !m_stop; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        expire(m_clock());
        if (++rounds % 5 == 0) logRequests();
    }
}

void HidParking::logRequests()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const std::unique_ptr<Entry>& e : m_entries) {
        HidPassthrough::Slot& s = *e->slot;
        // A handed out device is paused on purpose.
        const bool servicing = e->batch || s.device->servicing();
        const std::string line = HidPassthrough::requestSummary(*s.route, servicing);
        if (!line.empty()) log::info("[hid] " + s.identity.name + " (kept): " + line);
    }
}

void HidParking::park(std::unique_ptr<Entry> entry)
{
    entry->keepUntilMs = m_clock() + kKeepMs;
    entry->unclaimedUntilMs = 0;
    entry->batch = 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.push_back(std::move(entry));
}

std::unique_ptr<HidParking::Entry> HidParking::claim(const std::string& name, uint16_t vendorId,
                                                     uint16_t productId, uint16_t version,
                                                     const std::vector<uint8_t>& descriptor)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if ((*it)->batch || !(*it)->slot->sameAs(name, vendorId, productId, version, descriptor))
            continue;
        std::unique_ptr<Entry> e = std::move(*it);
        m_entries.erase(it);
        return e;
    }
    return nullptr;
}

void HidParking::expire(int64_t nowMs)
{
    std::vector<std::unique_ptr<Entry>> gone;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            Entry& e = **it;
            const bool expired = !e.batch && (nowMs >= e.keepUntilMs ||
                                              (e.unclaimedUntilMs && nowMs >= e.unclaimedUntilMs));
            if (!expired) {
                ++it;
                continue;
            }
            gone.push_back(std::move(*it));
            it = m_entries.erase(it);
        }
    }
    // Outside the lock: removing a device waits for its backend's thread.
    for (std::unique_ptr<Entry>& e : gone) {
        const input::VirtualHidIdentity& id = e->slot->identity;
        log::info("[hid] " + id.name + " " + idsOf(id.vendorId, id.productId) +
                  (e->unclaimedUntilMs && nowMs >= e->unclaimedUntilMs
                       ? " not taken by this stream: removed"
                       : " kept with no stream for too long: removed"));
        e->slot->device->release();
    }
}

uint64_t HidParking::handOut(std::vector<HidHandedDevice>& out)
{
    std::vector<Entry*> handed;
    uint64_t batch = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (std::unique_ptr<Entry>& e : m_entries) {
            if (e->batch || !e->slot->device->nativeHandle()) continue;
            if (!batch) batch = m_nextBatch++;
            e->batch = batch;
            handed.push_back(e.get());
        }
    }
    // Entries of a batch move only through settle(), so the pointers hold.
    const int64_t now = m_clock();
    for (Entry* e : handed) {
        HidPassthrough::Slot& s = *e->slot;
        // From here the game's requests wait in the driver for the next
        // process; pausing waits for the one being answered.
        s.device->pause();
        HidHandedDevice d;
        d.handle = s.device->nativeHandle();
        HidParkedState& st = d.state;
        st.name = s.identity.name;
        st.vendorId = s.identity.vendorId;
        st.productId = s.identity.productId;
        st.version = s.identity.version;
        st.descriptor = s.identity.descriptor;
        st.first.assign(s.first.begin(), s.first.end());
        st.last.assign(s.last.begin(), s.last.end());
        st.pidFirstId = s.pidFirstId;
        {
            std::lock_guard<std::mutex> lock(s.route->mutex);
            if (s.route->engine) st.pidState = s.route->engine->save();
            st.effects = s.route->journal.replay(now);
        }
        st.keepMs = std::max<int64_t>(0, e->keepUntilMs - now);
        out.push_back(std::move(d));
    }
    return batch;
}

void HidParking::settle(uint64_t batch, bool taken)
{
    if (!batch) return;
    std::vector<std::unique_ptr<Entry>> gone;
    std::vector<Entry*> back;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if ((*it)->batch != batch) {
                ++it;
                continue;
            }
            if (taken) {
                gone.push_back(std::move(*it));
                it = m_entries.erase(it);
            } else {
                back.push_back(it->get());
                ++it;
            }
        }
    }
    // The other process holds its own copy: closing ours leaves the device.
    for (std::unique_ptr<Entry>& e : gone)
        e->slot->device->release();
    for (Entry* e : back) {
        e->slot->device->resume();
        std::lock_guard<std::mutex> lock(m_mutex);
        e->batch = 0;
    }
}

std::string HidParking::takeIn(uint64_t handle, const HidParkedState& state, int64_t unclaimedMs)
{
    std::string error;
    std::unique_ptr<input::IVirtualHid> device = input::adoptVirtualHid(handle, error);
    if (!device) return error;
    return takeIn(std::move(device), state, unclaimedMs);
}

std::string HidParking::takeIn(std::unique_ptr<input::IVirtualHid> device,
                               const HidParkedState& state, int64_t unclaimedMs)
{
    if (std::string why = input::hid::validate(state.descriptor); !why.empty()) {
        device->release();
        return why;
    }
    auto s = std::make_unique<HidPassthrough::Slot>();
    s->identity.name = state.name;
    s->identity.vendorId = state.vendorId;
    s->identity.productId = state.productId;
    s->identity.version = state.version;
    s->identity.descriptor = state.descriptor;
    s->pidFirstId = state.pidFirstId;
    s->describe(state.descriptor);
    s->first.insert(state.first.begin(), state.first.end());
    s->last.insert(state.last.begin(), state.last.end());

    const int64_t now = m_clock();
    s->route = std::make_shared<HidPassthrough::Route>();
    s->route->clock = m_clock;
    s->route->traceLeft = HidPassthrough::kTraceRequests;
    if (state.pidFirstId) {
        s->route->engine = std::make_unique<input::hid::PidEngine>(s->parsed, state.pidFirstId);
        if (!s->route->engine->restore(state.pidState))
            log::warning("[hid] " + state.name + ": its effect table did not come along");
    }
    for (const HidFfb& op : state.effects)
        s->route->journal.record(op, now);
    HidPassthrough::wire(*device, s->route);
    device->resume();
    s->device = std::move(device);

    auto e = std::make_unique<Entry>();
    log::info("[hid] " + state.name + " " + idsOf(state.vendorId, state.productId) +
              " kept for the next stream (" + std::to_string(s->route->journal.effects()) +
              " effects)");
    e->slot = std::move(s);
    e->keepUntilMs = now + std::max<int64_t>(0, state.keepMs);
    e->unclaimedUntilMs = unclaimedMs > 0 ? now + unclaimedMs : 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.push_back(std::move(e));
    return {};
}

// ── HidPassthrough ──────────────────────────────────────────────────────────

HidPassthrough::HidPassthrough(RequestSink onRequest, Factory factory, Clock clock)
    : m_onRequest(std::move(onRequest))
    , m_factory(std::move(factory))
    , m_clock(clock ? std::move(clock) : Clock(steadyMs))
{}

HidPassthrough::~HidPassthrough()
{
    m_stop = true;
    if (m_watchdog.joinable()) m_watchdog.join();
    if (!m_parking) {
        detachAll();
        return;
    }
    // The stream ends, the game may not: each device waits at rest for the
    // next stream, still answering the game.
    std::map<int, std::unique_ptr<Slot>> leaving;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        leaving.swap(m_slots);
    }
    for (auto& [slot, s] : leaving) {
        s->rest();
        {
            std::lock_guard<std::mutex> lock(s->route->mutex);
            s->route->owner = nullptr;
        }
        log::info("[hid] slot " + std::to_string(slot) + ": " + s->identity.name +
                  " kept at rest for the next stream");
        auto e = std::make_unique<HidParking::Entry>();
        e->slot = std::move(s);
        m_parking->park(std::move(e));
    }
}

std::string HidPassthrough::unavailableReason()
{
    return input::virtualHidUnavailableReason();
}

const char* HidPassthrough::supportToken()
{
    switch (input::virtualHidSupport()) {
    case input::VirtualHidSupport::Ready: return "ok";
    case input::VirtualHidSupport::DriverMissing: return "driver_missing";
    case input::VirtualHidSupport::NotSetUp: return "setup";
    case input::VirtualHidSupport::Unsupported: break;
    }
    return "unsupported";
}

bool HidPassthrough::forceFeedbackAvailable()
{
    return input::virtualHidForceFeedback();
}

void HidPassthrough::wire(input::IVirtualHid& device, const std::shared_ptr<Route>& route)
{
    // pid.dll's synchronous questions: answered here, at once.
    device.setFeatureHandler([route](uint8_t reportId) {
        std::lock_guard<std::mutex> lock(route->mutex);
        std::vector<uint8_t> answer =
            route->engine ? route->engine->getFeature(reportId) : std::vector<uint8_t>{};
        ++route->gets;
        if (answer.empty()) ++route->emptyGets;
        if (route->traceLeft > 0) {
            --route->traceLeft;
            log::info("[hid] slot " + std::to_string(route->slot) +
                      (route->owner ? "" : " (kept)") + ": get feature " +
                      std::to_string(reportId) + " -> " +
                      (answer.empty() ? std::string("nothing") : hexHead(answer)));
        }
        return answer;
    });
    // Effects the backend services itself (uinput).
    device.setFfbHandler([route](const HidFfb& op) {
        std::lock_guard<std::mutex> lock(route->mutex);
        route->journal.record(op, route->clock());
        HidPassthrough* owner = route->owner;
        if (!owner || !owner->m_onFfb) return;
        HidFfb o = op;
        o.slot = route->slot;
        owner->m_onFfb(o);
    });
    device.setRequestHandler([route](input::IVirtualHid::Request kind, uint8_t reportId,
                                     const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(route->mutex);
        const bool output = kind == input::IVirtualHid::Request::Output;
        if (output) ++route->outputs;
        if (kind == input::IVirtualHid::Request::SetFeature) ++route->sets;
        const bool repeat = output && route->lastOutputId == reportId;
        if (output) route->lastOutputId = reportId;
        if (route->traceLeft > 0 && !repeat && kind != input::IVirtualHid::Request::GetFeature) {
            --route->traceLeft;
            log::info(
                "[hid] slot " + std::to_string(route->slot) + (route->owner ? "" : " (kept)") +
                (kind == input::IVirtualHid::Request::Output ? ": output " : ": set feature ") +
                std::to_string(reportId) + " " + hexHead(data));
        }
        // The PID block's reports stay here: decoded into operations, or
        // already answered by the feature handler.
        std::vector<HidFfb> ops;
        bool handled = false;
        if (route->engine && route->engine->owns(reportId)) {
            if (kind != input::IVirtualHid::Request::GetFeature) route->engine->write(data, ops);
            handled = true;
        }
        const int64_t now = route->clock();
        for (const HidFfb& op : ops)
            route->journal.record(op, now);
        HidPassthrough* owner = route->owner;
        if (!owner) return; // parked: the journal is all that hears it
        for (HidFfb& op : ops) {
            op.slot = route->slot;
            if (owner->m_onFfb) owner->m_onFfb(op);
        }
        if (handled || !owner->m_onRequest) return;
        HidRequest r;
        r.slot = route->slot;
        r.kind = kind == input::IVirtualHid::Request::Output       ? HidRequest::Kind::Output
                 : kind == input::IVirtualHid::Request::GetFeature ? HidRequest::Kind::GetFeature
                                                                   : HidRequest::Kind::SetFeature;
        r.reportId = reportId;
        r.data = data;
        owner->m_onRequest(r);
    });
}

std::string HidPassthrough::attach(int slot, const HidDeviceInfo& device)
{
    if (slot < 0 || slot >= kMaxSlots) return "slot out of range";

    std::vector<input::hid::Collection> collections = device.collections;
    input::hid::repairBounds(collections);
    input::hid::repairUsages(collections);
    std::unique_ptr<input::IVirtualHid> dev =
        m_factory ? m_factory() : input::makeVirtualHid(device.forceFeedback);
    if (!dev) {
        // Never empty: on a host where the OS backend is usable, its reason is
        // "" and a missing backend must still read as a refusal.
        const std::string why = unavailableReason();
        return why.empty() ? "no virtual HID backend could be made" : why;
    }
    // Force feedback: a PID block after the device's own reports, when the
    // page can play it, the backend's OS drives PID and the device numbers its
    // reports. A wheel's own PID block never reaches the host's OS: the host
    // answers for it, or nobody.
    const uint8_t pidId =
        device.forceFeedback && dev->forceFeedbackByPid() ? input::hid::pidFirstId(collections) : 0;
    input::hid::stripPid(collections);
    const std::vector<uint8_t> descriptor =
        pidId ? input::hid::encodeWithPid(collections, pidId) : input::hid::encode(collections);
    if (std::string why = input::hid::validate(descriptor); !why.empty()) {
        logRefusal(slot, device, descriptor, why);
        return why;
    }

    // The same device, kept from the last stream: the game still has it.
    std::unique_ptr<Slot> s;
    if (m_parking) {
        std::unique_ptr<HidParking::Entry> e = m_parking->claim(
            device.name, device.vendorId, device.productId, device.version, descriptor);
        if (e) s = std::move(e->slot);
    }
    const bool kept = s != nullptr;
    if (kept) {
        // Its route stays ownerless until attached(): what the game does in
        // between goes to the journal, and reaches the page in the replay.
        s->lastSeq.clear(); // the page counts from 0 again
        s->fromParking = true;
    } else {
        s = std::make_unique<Slot>();
        s->identity.name = device.name;
        s->identity.vendorId = device.vendorId;
        s->identity.productId = device.productId;
        s->identity.version = device.version;
        s->identity.descriptor = descriptor;
        s->pidFirstId = pidId;
        s->describe(descriptor);
        s->route = std::make_shared<Route>();
        s->route->clock = m_clock;
        if (pidId) s->route->engine = std::make_unique<input::hid::PidEngine>(s->parsed, pidId);
        s->route->owner = this;
        s->route->slot = slot;
        s->route->traceLeft = kTraceRequests;
        wire(*dev, s->route);
        std::string error;
        if (!dev->create(s->identity, error)) {
            logRefusal(slot, device, descriptor, error);
            return error;
        }
        s->device = std::move(dev);
    }

    std::unique_ptr<Slot> previous;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        previous = std::move(m_slots[slot]);
        m_slots[slot] = std::move(s);
        if (!m_watchdog.joinable()) m_watchdog = std::thread([this] { watchdog(); });
    }
    previous.reset(); // its device goes away outside the lock
    log::info("[hid] slot " + std::to_string(slot) + ": " + device.name + " " +
              idsOf(device.vendorId, device.productId) +
              (kept ? " taken back from the last stream"
                    : " created, descriptor " + std::to_string(descriptor.size()) + " bytes") +
              (pidId ? ", force feedback (PID reports from " + std::to_string(pidId) + ")"
                     : std::string()));
    return {};
}

void HidPassthrough::attached(int slot)
{
    std::shared_ptr<Route> route;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_slots.find(slot);
        if (it == m_slots.end() || !it->second->fromParking) return;
        it->second->fromParking = false;
        route = it->second->route;
    }
    // Under the route's lock: the game's next operation waits for the replay,
    // and the page's player meets every effect before its updates.
    std::lock_guard<std::mutex> lock(route->mutex);
    route->owner = this;
    route->slot = slot;
    route->clock = m_clock;
    route->traceLeft = kTraceRequests;
    const std::vector<HidFfb> ops = route->journal.replay(m_clock());
    if (m_onFfb)
        for (HidFfb op : ops) {
            op.slot = slot;
            m_onFfb(op);
        }
    log::info("[hid] slot " + std::to_string(slot) + ": force feedback of the last stream sent " +
              "again (" + std::to_string(route->journal.effects()) + " effects)");
}

void HidPassthrough::input(int slot, uint8_t reportId, uint16_t seq, const uint8_t* bytes,
                           size_t size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_slots.find(slot);
    if (it == m_slots.end()) return;
    Slot& s = *it->second;
    auto want = s.inputBytes.find(reportId);
    if (want == s.inputBytes.end() || want->second != size) return; // not a report this device has
    auto seen = s.lastSeq.find(reportId);
    if (seen != s.lastSeq.end() && static_cast<int16_t>(seq - seen->second) <= 0) return; // late
    s.lastSeq[reportId] = seq;

    std::vector<uint8_t> report;
    report.reserve(size + 1);
    if (s.numbered) report.push_back(reportId);
    report.insert(report.end(), bytes, bytes + size);
    s.device->input(report.data(), report.size());
    s.last[reportId].assign(bytes, bytes + size);
    if (!s.first.count(reportId)) s.first[reportId].assign(bytes, bytes + size);
    s.lastAtMs = m_clock();
    s.resting = false;
}

void HidPassthrough::reply(int slot, uint8_t reportId, const uint8_t* bytes, size_t size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_slots.find(slot);
    if (it == m_slots.end()) return;
    Slot& s = *it->second;
    auto want = s.inputBytes.find(reportId);
    if (want == s.inputBytes.end() || want->second != size) return;
    std::vector<uint8_t> report;
    report.reserve(size + 1);
    if (s.numbered) report.push_back(reportId);
    report.insert(report.end(), bytes, bytes + size);
    s.device->input(report.data(), report.size());
}

void HidPassthrough::detach(int slot)
{
    std::unique_ptr<Slot> gone;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_slots.find(slot);
        if (it == m_slots.end()) return;
        gone = std::move(it->second);
        m_slots.erase(it);
    }
    log::info("[hid] slot " + std::to_string(slot) + " removed");
}

void HidPassthrough::detachAll()
{
    std::map<int, std::unique_ptr<Slot>> gone;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        gone.swap(m_slots);
    }
}

void HidPassthrough::checkSilence(int64_t nowMs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [slot, sp] : m_slots) {
        Slot& s = *sp;
        if (s.resting || nowMs - s.lastAtMs < kSilenceMs) continue;
        s.rest();
        log::info("[hid] slot " + std::to_string(slot) + " silent for " +
                  std::to_string(kSilenceMs) + " ms: put to rest");
    }
}

void HidPassthrough::watchdog()
{
    int ticks = 0;
    while (!m_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        checkSilence(m_clock());
        if (++ticks % 20 == 0) logRequests();
    }
}

void HidPassthrough::logRequests()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [slot, sp] : m_slots) {
        Slot& s = *sp;
        const std::string line = requestSummary(*s.route, s.device->servicing());
        if (!line.empty()) log::info("[hid] slot " + std::to_string(slot) + ": " + line);
    }
}

std::string HidPassthrough::requestSummary(Route& route, bool servicing)
{
    uint32_t outputs, sets, gets, emptyGets;
    bool was;
    {
        std::lock_guard<std::mutex> lock(route.mutex);
        outputs = std::exchange(route.outputs, 0);
        sets = std::exchange(route.sets, 0);
        gets = std::exchange(route.gets, 0);
        emptyGets = std::exchange(route.emptyGets, 0);
        was = std::exchange(route.asking, outputs || sets || gets);
    }
    const bool asking = outputs || sets || gets;
    if (servicing && !emptyGets && asking == was) return {};
    if (!asking)
        return servicing ? "the game asks nothing any more"
                         : "requests NOT SERVICED: the event thread has stopped";
    return "in 5 s the game sent " + std::to_string(outputs) + " output, " + std::to_string(sets) +
           " set feature, asked " + std::to_string(gets) + " get feature (" +
           std::to_string(emptyGets) + " unanswered); requests " +
           (servicing ? "serviced" : "NOT SERVICED: the event thread has stopped");
}

} // namespace mw::native
