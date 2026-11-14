/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#include "IceMuxTcpCore.h"

#include <algorithm>

namespace mw::icemuxtcp {

using mw::icemux::Kind;
using mw::icemux::StunView;

// Every agent within its bound has room among the connections handed over,
// as long as no worker sits on named ones: where none is left, a connection
// is closed (route()).
static_assert(kHandedMax >= kWindowsMax * kUnansweredMax,
              "an agent within its bound could find no room for a hand-over");
// The length travels in two bytes, and the frame with it in the place.
static_assert(kFrameMin >= 20 && kFrameMax <= 0xFFFF - 2, "the frame's bounds do not fit");
// A try's fragment is no agent's, here or in the mux: it is longer than any.
static_assert(kTokenSize > mw::icemux::kUfragMax, "a try's fragment could be an agent's");
static_assert(kHereUfragMin <= mw::icemux::kUfragMin, "an agent of this process's is held to more");

Source sourceOf(uint8_t family, const uint8_t* address) noexcept
{
    Source s;
    s.family = family;
    if (!address) return s;
    // An IPv4 address whole; of an IPv6 one, the network it is in: a host
    // there has as many addresses as it likes.
    std::copy(address, address + (family == 4 ? 4 : 8), s.net.begin());
    return s;
}

Table::Window* Table::windowOf(uint32_t feed) noexcept
{
    if (feed == 0) return nullptr;
    for (Window& w : m_Windows)
        if (w.feed == feed) return &w;
    return nullptr;
}

bool Table::usable(int place) const noexcept
{
    return place >= 0 && static_cast<size_t>(place) < kPlaces &&
           m_Waiting[static_cast<size_t>(place)].socket != kNoSocket;
}

void Table::release(Waiting& waiting, Closed& closed)
{
    if (waiting.socket == kNoSocket) return;
    closed.push_back({waiting.socket, false});
    waiting.socket = kNoSocket;
    waiting.have = 0;
    waiting.length = 0;
}

void Table::release(Entry& entry, Closed& closed)
{
    if (entry.number == 0) return;
    closed.push_back({entry.socket, true});
    forget(entry);
}

void Table::forget(Entry& entry) noexcept
{
    if (entry.number == 0) return;
    if (Window* w = windowOf(entry.feed))
        if (w->unanswered > 0) --w->unanswered;
    entry.number = 0;
    entry.feed = 0;
    entry.owner = 0;
    entry.named = false;
    entry.socket = kNoSocket;
    entry.length = 0;
}

void Table::settle(Closed& closed)
{
    if (expected()) return;
    // Nobody expects a connection: whatever waits has nobody to go to
    for (Waiting& waiting : m_Waiting) {
        if (waiting.socket == kNoSocket) continue;
        ++m_Counters.unexpected;
        release(waiting, closed);
    }
}

void Table::noteEnd(uint16_t port, End why) noexcept
{
    if (port == 0) return;
    PortEnd* slot = nullptr;
    for (PortEnd& end : m_Ends) {
        if (end.port == port) {
            slot = &end;
            break;
        }
        if (end.port == 0 && !slot) slot = &end;
    }
    // No room: the one written longest ago gives its place
    if (!slot) slot = &m_Ends[m_NextEnd++ % kEndsMax];
    slot->port = port;
    slot->why = why;
}

End Table::endedOn(uint16_t port) const noexcept
{
    for (const PortEnd& end : m_Ends)
        if (end.port == port && port != 0) return end.why;
    return End::None;
}

bool Table::open(uint32_t feed, std::string_view ufrag, const uint16_t* ports, size_t count,
                 uint64_t nowMs, uint32_t owner)
{
    if (feed == 0 || owner == 0 || !ports || count == 0 || count > kWindowPortsMax) return false;
    // An agent of this process's own is numbered apart, and nobody else's is:
    // what is handed for the one can never be asked for as the other's
    if ((owner == kHere) != hereFeed(feed)) return false;
    const size_t shortest = owner == kHere ? kHereUfragMin : mw::icemux::kUfragMin;
    if (ufrag.size() < shortest || ufrag.size() > mw::icemux::kUfragMax) return false;
    Window* room = nullptr;
    for (Window& w : m_Windows) {
        if (w.feed == 0) {
            if (!room) room = &w;
        } else if (w.feed == feed || w.ufrag == ufrag) {
            return false;
        }
    }
    if (!room) return false;
    Window fresh;
    for (size_t i = 0; i < count; ++i) {
        if (ports[i] == 0) return false;
        const auto end = fresh.ports.begin() + fresh.portCount;
        if (std::find(fresh.ports.begin(), end, ports[i]) == end)
            fresh.ports[fresh.portCount++] = ports[i];
    }
    fresh.feed = feed;
    fresh.owner = owner;
    fresh.ufrag.assign(ufrag);
    fresh.untilMs = nowMs + kWindowMs;
    *room = std::move(fresh);
    return true;
}

void Table::close(uint32_t feed, Closed& closed, End why)
{
    Window* w = windowOf(feed);
    if (!w) return;
    for (Entry& entry : m_Handed) {
        // One that was named stays until its worker answers for it, or ends
        if (entry.number == 0 || entry.feed != feed || entry.named) continue;
        ++m_Counters.unanswered;
        release(entry, closed);
    }
    for (uint8_t i = 0; i < w->portCount; ++i)
        noteEnd(w->ports[i], why);
    *w = Window{};
    settle(closed);
}

void Table::releaseOwner(uint32_t owner, Closed& closed)
{
    if (owner == 0) return;
    for (const Window& w : m_Windows)
        if (w.feed != 0 && w.owner == owner) close(w.feed, closed);
    for (Entry& entry : m_Handed) {
        if (entry.number == 0 || entry.owner != owner) continue;
        ++m_Counters.unanswered;
        release(entry, closed);
    }
}

void Table::reset(Closed& closed)
{
    for (const Window& w : m_Windows)
        if (w.feed != 0) close(w.feed, closed);
    for (Entry& entry : m_Handed) {
        if (entry.number == 0) continue;
        ++m_Counters.unanswered;
        release(entry, closed);
    }
    for (Try& t : m_Tries)
        t = Try{};
    settle(closed);
}

bool Table::expects(uint32_t feed) const noexcept
{
    if (feed == 0) return false;
    for (const Window& w : m_Windows)
        if (w.feed == feed) return true;
    return false;
}

bool Table::anyWindow() const noexcept
{
    for (const Window& w : m_Windows)
        if (w.feed != 0) return true;
    return false;
}

bool Table::expected() const noexcept
{
    if (anyWindow()) return true;
    for (const Try& t : m_Tries)
        if (t.id != 0 && !t.arrived) return true;
    return false;
}

size_t Table::listenPorts(uint16_t* out, size_t max) const noexcept
{
    size_t count = 0;
    for (const Window& w : m_Windows) {
        if (w.feed == 0) continue;
        for (uint8_t i = 0; i < w.portCount; ++i) {
            if (std::find(out, out + count, w.ports[i]) != out + count) continue;
            if (count >= max) return count;
            out[count++] = w.ports[i];
        }
    }
    for (const Try& t : m_Tries) {
        if (t.id == 0 || t.arrived) continue;
        if (std::find(out, out + count, t.port) != out + count) continue;
        if (count >= max) return count;
        out[count++] = t.port;
    }
    return count;
}

bool Table::idle() const noexcept
{
    if (anyWindow()) return false;
    for (const Waiting& waiting : m_Waiting)
        if (waiting.socket != kNoSocket) return false;
    for (const Entry& entry : m_Handed)
        if (entry.number != 0) return false;
    // One that came stays until it is asked about, or its time is over
    for (const Try& t : m_Tries)
        if (t.id != 0) return false;
    return true;
}

uint32_t Table::expectToken(std::string_view fragment, uint16_t port, uint64_t nowMs)
{
    if (fragment.size() != kTokenSize || port == 0) return 0;
    Try* room = nullptr;
    for (Try& t : m_Tries) {
        if (t.id == 0) {
            if (!room) room = &t;
        } else if (std::equal(t.fragment.begin(), t.fragment.end(), fragment.begin())) {
            return 0;
        }
    }
    if (!room) return 0;
    if (m_NextTry == 0) m_NextTry = 1;
    room->id = m_NextTry++;
    std::copy(fragment.begin(), fragment.end(), room->fragment.begin());
    room->port = port;
    room->untilMs = nowMs + kTokenMs;
    room->arrived = false;
    return room->id;
}

Table::Token Table::token(uint32_t id) const noexcept
{
    if (id == 0) return Token::None;
    for (const Try& t : m_Tries)
        if (t.id == id) return t.arrived ? Token::Arrived : Token::Waiting;
    return Token::None;
}

void Table::endToken(uint32_t id, Closed& closed)
{
    if (id == 0) return;
    for (Try& t : m_Tries) {
        if (t.id != id) continue;
        if (!t.arrived) noteEnd(t.port, End::Tried);
        t = Try{};
        settle(closed);
        return;
    }
}

int Table::accept(uintptr_t socket, const Source& from, uint16_t port, uint64_t nowMs,
                  Closed& closed)
{
    if (socket == kNoSocket) return -1;
    if (!expected()) {
        ++m_Counters.unexpected;
        closed.push_back({socket, false});
        return -1;
    }
    ++m_Counters.accepted;
    int room = -1, oldest = -1, sourceOldest = -1;
    size_t ofSource = 0;
    for (size_t k = 0; k < kPlaces; ++k) {
        const Waiting& w = m_Waiting[k];
        const int at = static_cast<int>(k);
        if (w.socket == kNoSocket) {
            if (room < 0) room = at;
            continue;
        }
        if (oldest < 0 || w.order < m_Waiting[static_cast<size_t>(oldest)].order) oldest = at;
        if (!(w.from == from)) continue;
        ++ofSource;
        if (sourceOldest < 0 || w.order < m_Waiting[static_cast<size_t>(sourceOldest)].order)
            sourceOldest = at;
    }
    // One source never holds more than its share, and so cannot fill the
    // table; a full table gives the oldest place, whatever its connection has
    // sent: a peer's own connection is in after one round trip.
    const int place = ofSource >= kPerSource ? sourceOldest : room >= 0 ? room : oldest;
    Waiting& w = m_Waiting[static_cast<size_t>(place)];
    if (w.socket != kNoSocket) {
        ++m_Counters.evicted;
        release(w, closed);
    }
    w.socket = socket;
    w.from = from;
    w.port = port;
    w.untilMs = nowMs + kFirstFrameMs;
    w.order = ++m_Order;
    w.have = 0;
    w.length = 0;
    return place;
}

uintptr_t Table::socketAt(int place) const noexcept
{
    return usable(place) ? m_Waiting[static_cast<size_t>(place)].socket : kNoSocket;
}

Table::Need Table::need(int place) noexcept
{
    if (!usable(place)) return {};
    Waiting& w = m_Waiting[static_cast<size_t>(place)];
    const size_t wanted = w.have < 2 ? size_t{2} : size_t{2} + w.length;
    if (w.have >= wanted) return {};
    return {w.bytes.data() + w.have, wanted - w.have};
}

Table::Step Table::got(int place, size_t count) noexcept
{
    if (!usable(place)) return Step::Close;
    Waiting& w = m_Waiting[static_cast<size_t>(place)];
    const size_t wanted = w.have < 2 ? size_t{2} : size_t{2} + w.length;
    if (count == 0 || count > wanted - w.have) return Step::Close;
    const bool hadLength = w.have >= 2;
    w.have = static_cast<uint16_t>(w.have + count);
    if (w.have < 2) return Step::More;
    if (!hadLength) {
        w.length = static_cast<uint16_t>((w.bytes[0] << 8) | w.bytes[1]);
        return w.length < kFrameMin || w.length > kFrameMax ? Step::Close : Step::More;
    }
    return w.have == 2 + w.length ? Step::Whole : Step::More;
}

void Table::refuse(int place, Closed& closed)
{
    if (!usable(place)) return;
    ++m_Counters.malformed;
    release(m_Waiting[static_cast<size_t>(place)], closed);
}

void Table::drop(int place, Closed& closed)
{
    if (!usable(place)) return;
    ++m_Counters.ended;
    release(m_Waiting[static_cast<size_t>(place)], closed);
}

Table::Handed Table::route(int place, uint64_t nowMs, Closed& closed)
{
    if (!usable(place)) return {};
    Waiting& w = m_Waiting[static_cast<size_t>(place)];
    if (w.have < 2 || w.have != 2 + w.length) {
        // Not whole: nothing is routed on part of a frame
        ++m_Counters.malformed;
        release(w, closed);
        return {};
    }
    const uint8_t* frame = w.bytes.data() + 2;
    const StunView view = mw::icemux::readStun(frame, w.length);
    // A try of the way here, before any agent: its fragment has a length no
    // agent's has, so it is looked for among the tries and nowhere else. It
    // counts once, on the port it was made for, and its connection is
    // nobody's: closed here.
    if (view.kind == Kind::BindingRequest && view.localUfrag.size() == kTokenSize) {
        for (Try& t : m_Tries) {
            if (t.id == 0 || t.arrived || nowMs >= t.untilMs || t.port != w.port) continue;
            if (!std::equal(t.fragment.begin(), t.fragment.end(), view.localUfrag.begin()))
                continue;
            t.arrived = true;
            // As long again for whoever made it to ask about it
            t.untilMs = nowMs + kTokenMs;
            ++m_Counters.tried;
            const uint32_t id = t.id;
            noteEnd(t.port, End::Tried);
            release(w, closed);
            settle(closed);
            return {0, 0, id};
        }
    }
    // A request, and for an agent that expects a connection on this very
    // port: a worker is handed nothing for a fragment that is not its own
    // agent's, and nothing on a port it does not hold.
    Window* target = nullptr;
    if (view.kind == Kind::BindingRequest && !view.localUfrag.empty()) {
        for (Window& candidate : m_Windows) {
            if (candidate.feed == 0 || nowMs >= candidate.untilMs) continue;
            if (candidate.ufrag != view.localUfrag) continue;
            const auto end = candidate.ports.begin() + candidate.portCount;
            if (std::find(candidate.ports.begin(), end, w.port) != end) target = &candidate;
            break;
        }
    }
    if (!target) {
        ++m_Counters.unrouted;
        release(w, closed);
        return {};
    }
    Entry* room = nullptr;
    size_t named = 0;
    for (Entry& entry : m_Handed) {
        if (entry.number == 0) {
            if (!room) room = &entry;
        } else if (entry.named && entry.owner == target->owner) {
            ++named;
        }
    }
    if (!room || target->unanswered >= kUnansweredMax || target->handed >= kHandedPerWindow ||
        named >= kNamedPerOwnerMax) {
        ++m_Counters.bounded;
        release(w, closed);
        return {};
    }
    if (m_NextNumber == 0) m_NextNumber = 1;
    room->number = m_NextNumber++;
    room->feed = target->feed;
    room->owner = target->owner;
    room->named = false;
    room->socket = w.socket;
    room->untilMs = nowMs + kAnswerMs;
    room->order = ++m_Order;
    room->length = w.length;
    std::copy(frame, frame + w.length, room->frame.begin());
    ++target->handed;
    ++target->unanswered;
    ++m_Counters.handed;
    // The place is free; the socket is the entry's now, and not let go
    w.socket = kNoSocket;
    w.have = 0;
    w.length = 0;
    return {room->feed, room->number, 0};
}

bool Table::held(uint32_t number, Held* out) const noexcept
{
    if (number == 0 || !out) return false;
    for (const Entry& entry : m_Handed) {
        if (entry.number != number) continue;
        *out = Held{entry.socket, entry.feed, entry.frame.data(), entry.length};
        return true;
    }
    return false;
}

bool Table::name(uint32_t number) noexcept
{
    if (number == 0) return false;
    for (Entry& entry : m_Handed) {
        if (entry.number != number) continue;
        entry.named = true;
        return true;
    }
    return false;
}

bool Table::answered(uint32_t owner, uint32_t number, Closed& closed)
{
    if (owner == 0) return false;
    Entry* found = nullptr;
    for (Entry& entry : m_Handed) {
        if (entry.number == 0 || entry.owner != owner) continue;
        if (number != 0) {
            if (entry.number == number) found = &entry;
        } else if (!entry.named && (!found || entry.order < found->order)) {
            found = &entry;
        }
    }
    if (!found) return false;
    ++m_Counters.answered;
    release(*found, closed);
    return true;
}

bool Table::take(uint32_t owner, uint32_t number, Held* out)
{
    if (owner == 0 || number == 0 || !out) return false;
    for (Entry& entry : m_Handed) {
        if (entry.number != number || entry.owner != owner || entry.named) continue;
        // The frame stays where it is until the entry is given to another
        *out = Held{entry.socket, entry.feed, entry.frame.data(), entry.length};
        ++m_Counters.answered;
        forget(entry);
        return true;
    }
    return false;
}

bool Table::letGo(uint32_t number, Closed& closed)
{
    if (number == 0) return false;
    for (Entry& entry : m_Handed) {
        if (entry.number != number || entry.named) continue;
        ++m_Counters.unanswered;
        release(entry, closed);
        return true;
    }
    return false;
}

void Table::expire(uint64_t nowMs, Closed& closed)
{
    for (Waiting& waiting : m_Waiting) {
        if (waiting.socket == kNoSocket || nowMs < waiting.untilMs) continue;
        ++m_Counters.timedOut;
        release(waiting, closed);
    }
    for (Entry& entry : m_Handed) {
        // Not one that was named: its worker may copy it yet
        if (entry.number == 0 || entry.named || nowMs < entry.untilMs) continue;
        ++m_Counters.unanswered;
        release(entry, closed);
    }
    for (const Window& w : m_Windows) {
        if (w.feed == 0 || nowMs < w.untilMs) continue;
        close(w.feed, closed, End::TimedOut);
    }
    bool ended = false;
    for (Try& t : m_Tries) {
        if (t.id == 0 || nowMs < t.untilMs) continue;
        if (!t.arrived) noteEnd(t.port, End::Tried);
        t = Try{};
        ended = true;
    }
    if (ended) settle(closed);
}

} // namespace mw::icemuxtcp
