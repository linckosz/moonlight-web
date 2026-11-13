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

#pragma once

#include "IceMuxCore.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// What the server decides of a TCP connection that came in on a shared port,
/// apart from every socket and every thread: where it waits for its first
/// frame, whose place it takes when there is none left, which stream worker
/// it is for, and how many such a worker can be sent. IceMuxTcp (the
/// listeners, the thread) calls it; the tests and the fuzz target call it
/// alone. docs/design/ice-mux.md
///
/// The server admits nothing. It reads one frame, finds from it which agent
/// the connection names, and hands the connection to that agent's worker,
/// whose own gate decides (the frame's integrity, under a password this
/// process never has). What is here bounds what that costs.
namespace mw::icemuxtcp {

/// Connections waiting for their first frame, in all, and for one source.
inline constexpr size_t kPlaces = 8;
inline constexpr size_t kPerSource = 2;
/// How long a connection has to send that frame, from its accept.
inline constexpr uint64_t kFirstFrameMs = 2000;
/// The frame's announced length (RFC 4571): a STUN header at least.
inline constexpr size_t kFrameMin = 20;
inline constexpr size_t kFrameMax = 1024;

/// Agents that can expect a connection at once: every agent the mux knows.
inline constexpr size_t kWindowsMax = mw::icemux::Router::kAgentsMax;
/// The shared ports one agent has (IceMux::kAgentSocketsMax).
inline constexpr size_t kWindowPortsMax = 4;
/// How long an agent can expect one, whatever its worker says or does not
/// say: above the time an agent gives its checks before it fails.
inline constexpr uint64_t kWindowMs = 45'000;

/// Connections handed to a worker for one agent that the worker has not
/// answered for, and connections handed for one agent in the life of its
/// window. An agent's user fragment is no secret (it travels in the clear
/// with every check), so anyone can name it: beyond these, a connection that
/// names it is closed here.
inline constexpr size_t kUnansweredMax = 2;
inline constexpr uint32_t kHandedPerWindow = 32;
/// How long this process keeps its end of a connection it handed over, when
/// the worker says nothing of it.
inline constexpr uint64_t kAnswerMs = 2000;
inline constexpr size_t kHandedMax = kWindowsMax * kUnansweredMax;
/// Connections one worker holds named (Table::name()) at once. Such a one is
/// kept for as long as the worker takes to answer for it, whatever becomes of
/// its agent: beyond these, a connection for an agent of that worker is
/// closed here.
inline constexpr size_t kNamedPerOwnerMax = 4;

/// The value no socket has.
inline constexpr uintptr_t kNoSocket = ~uintptr_t(0);

/// Whom a connection comes from, as the places are counted: an IPv4 address,
/// or the /64 an IPv6 address is in.
struct Source
{
    uint8_t family = 0; ///< 4 or 6
    std::array<uint8_t, 8> net{};

    bool operator==(const Source& o) const noexcept { return family == o.family && net == o.net; }
};

/// `address` holds 4 bytes for IPv4 and 16 for IPv6.
Source sourceOf(uint8_t family, const uint8_t* address) noexcept;

/// A socket the table lets go of: its caller's to close.
struct Gone
{
    uintptr_t socket = kNoSocket;
    /// It was handed to a worker, which may hold an end of its own: it is
    /// closed as it is, nothing of it changed. One that never was is this
    /// process's alone, and is reset, so that nothing of it lingers.
    bool handed = false;
};
using Closed = std::vector<Gone>;

/// The table. Not thread-safe: IceMuxTcp holds one lock around it. It holds
/// the sockets as numbers and closes none: whatever it lets go of is added to
/// the `closed` its caller passes.
class Table
{
public:
    // ── The agents that expect a connection ─────────────────────────────────

    /// The agent `feed`, known by `ufrag`, expects a TCP connection on these
    /// ports from now on, for kWindowMs at most. `owner` says whose worker
    /// the agent is: any number but 0, the same for every agent of one
    /// worker. False when the agent has a window already, the fragment is
    /// another window's or is not one the mux takes (kUfragMin to kUfragMax),
    /// no port or more than kWindowPortsMax are given, one is 0, there is no
    /// owner, or kWindowsMax agents expect one.
    bool open(uint32_t feed, std::string_view ufrag, const uint16_t* ports, size_t count,
              uint64_t nowMs, uint32_t owner);
    /// It expects none any more. What was handed for it and not answered is
    /// let go, but for what was named (name()); and, when it was the last,
    /// whatever still waits.
    void close(uint32_t feed, Closed& closed);
    /// The worker `owner` is gone: its agents expect nothing, and everything
    /// handed for them is let go, named or not.
    void releaseOwner(uint32_t owner, Closed& closed);
    /// Everything, for every worker: the server stops.
    void reset(Closed& closed);
    bool expects(uint32_t feed) const noexcept;
    bool anyWindow() const noexcept;
    /// The ports somebody expects a connection on, each once. The count,
    /// `max` at most.
    size_t listenPorts(uint16_t* out, size_t max) const noexcept;
    /// Nothing expected, nothing waiting, nothing handed and unanswered.
    bool idle() const noexcept;

    // ── The connections that wait for their first frame ─────────────────────

    /// A connection just accepted on `port`. Its place, 0 to kPlaces - 1: a
    /// free one; or, when its source has kPerSource already, the place of
    /// that source's oldest; or, when none is free, the oldest's of all,
    /// whatever that one has sent. -1, and the socket in `closed`, when no
    /// agent expects a connection.
    int accept(uintptr_t socket, const Source& from, uint16_t port, uint64_t nowMs, Closed& closed);
    /// The socket in `place`, kNoSocket when it is free.
    uintptr_t socketAt(int place) const noexcept;

    /// What to ask the socket in `place` for next: the two bytes of the
    /// length, then the frame they announce, and never a byte more — what
    /// follows the first frame is not this process's to read.
    struct Need
    {
        uint8_t* into = nullptr;
        size_t count = 0;
    };
    Need need(int place) noexcept;

    enum class Step
    {
        More,  ///< ask again
        Whole, ///< the frame is all there: route()
        Close, ///< not a frame an agent takes: refuse()
    };
    /// `count` bytes were read into what need() gave.
    Step got(int place, size_t count) noexcept;

    /// What the connection in `place` sent is no frame (Step::Close): the
    /// place is free, and the socket in `closed`.
    void refuse(int place, Closed& closed);
    /// The connection in `place` is over (its peer closed it, a read failed):
    /// the same.
    void drop(int place, Closed& closed);

    /// A connection handed to a worker, by the number it is known under until
    /// the worker answers for it.
    struct Handed
    {
        uint32_t feed = 0; ///< 0: handed to nobody, the socket is in `closed`
        uint32_t number = 0;
    };
    /// The frame in `place` is whole. It goes to the agent it names when it is
    /// a STUN Binding request whose USERNAME starts with the fragment of an
    /// agent that expects a connection on the port this one came in on, and
    /// that agent is within its bounds. The place is free either way.
    Handed route(int place, uint64_t nowMs, Closed& closed);

    // ── The connections handed over, until their worker answers ─────────────

    struct Held
    {
        uintptr_t socket = kNoSocket;
        uint32_t feed = 0;
        const uint8_t* frame = nullptr;
        size_t size = 0;
    };
    /// The connection handed under `number`, while this process still has it.
    bool held(uint32_t number, Held* out) const noexcept;
    /// The connection `number` is named to its worker: told by the number of
    /// this process's own handle on it, for the worker to copy out later.
    /// From then on only that worker lets go of it, by its answer or by its
    /// end: not the time, not its agent's end. A handle let go of before the
    /// worker copied it could be another one's number by then. False when
    /// there is none such.
    bool name(uint32_t number) noexcept;
    /// The worker `owner` has its own end of the connection `number`, or
    /// will not have it: this process's end is let go. With number 0, the
    /// one handed first to that worker among those not named. False when
    /// there is none such: a worker answers for what it was handed, and for
    /// nothing of another's.
    bool answered(uint32_t owner, uint32_t number, Closed& closed);
    /// There is nobody to hand the connection `number` to: it is let go at
    /// once. Not one that was named. False when there is none such.
    bool letGo(uint32_t number, Closed& closed);

    /// Lets go of what ran out of time: a connection whose frame did not come
    /// within kFirstFrameMs, one handed over kAnswerMs ago and not named, a
    /// window open for kWindowMs.
    void expire(uint64_t nowMs, Closed& closed);

    /// What became of the connections, for the log: counts, and nothing that
    /// names a peer or an agent.
    struct Counters
    {
        uint64_t accepted = 0;
        uint64_t handed = 0;     ///< routed to a worker
        uint64_t answered = 0;   ///< of those, the worker said so
        uint64_t unanswered = 0; ///< of those, let go with no answer from the worker
        uint64_t evicted = 0;    ///< gave their place to a newer one
        uint64_t timedOut = 0;   ///< sent no whole frame in time
        uint64_t ended = 0;      ///< closed by their peer, or a read failed
        uint64_t malformed = 0;  ///< announced a length no frame has
        uint64_t unrouted = 0;   ///< named no agent that expects one there
        uint64_t bounded = 0;    ///< named an agent that has had its share
        uint64_t unexpected = 0; ///< came when no agent expected one
    };
    const Counters& counters() const noexcept { return m_Counters; }

private:
    struct Window
    {
        uint32_t feed = 0; ///< 0: free
        uint32_t owner = 0;
        std::string ufrag;
        std::array<uint16_t, kWindowPortsMax> ports{};
        uint8_t portCount = 0;
        uint64_t untilMs = 0;
        uint32_t handed = 0;
        uint32_t unanswered = 0;
    };
    struct Waiting
    {
        uintptr_t socket = kNoSocket; ///< kNoSocket: free
        Source from;
        uint16_t port = 0;
        uint64_t untilMs = 0;
        uint64_t order = 0;
        uint16_t have = 0;   ///< bytes of `bytes` filled
        uint16_t length = 0; ///< the frame's, once its two bytes are there
        std::array<uint8_t, 2 + kFrameMax> bytes;
    };
    struct Entry
    {
        uint32_t number = 0; ///< 0: free
        uint32_t feed = 0;
        uint32_t owner = 0;
        bool named = false;
        uintptr_t socket = kNoSocket;
        uint64_t untilMs = 0;
        uint64_t order = 0;
        uint16_t length = 0;
        std::array<uint8_t, kFrameMax> frame;
    };

    Window* windowOf(uint32_t feed) noexcept;
    bool usable(int place) const noexcept;
    void release(Waiting& waiting, Closed& closed);
    void release(Entry& entry, Closed& closed);

    std::array<Window, kWindowsMax> m_Windows;
    std::array<Waiting, kPlaces> m_Waiting;
    std::array<Entry, kHandedMax> m_Handed;
    uint32_t m_NextNumber = 1;
    uint64_t m_Order = 0;
    Counters m_Counters;
};

} // namespace mw::icemuxtcp
