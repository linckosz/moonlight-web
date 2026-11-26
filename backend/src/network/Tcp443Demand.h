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

#include <QList>
#include <QString>

#include <cstdint>
#include <deque>

/// Whether TCP 443 is wanted on the router right now (settings.json
/// ice_tcp_443, Tcp443Guard): only once a browser has shown it needs it, and
/// for as long as one is connected through it, then ten minutes more.
///
/// 443 is allowed by default, and asked for by nobody until then. A browser
/// shows it needs it in one of two ways, and only these:
///   - the entry link's tunnel: the browser, before its tunnel is up, has no
///     session to show. What it says comes through the introduction server,
///     on the host's own authenticated line to it, as one of the tunnel's
///     signals, and nowhere else (Asker::Rendezvous). Such asks open 443 a
///     few times an hour at most (kRendezvousPerHour);
///   - a stream: its launch comes from a signed-in session or an invited
///     player's (Asker::Session), which says its STUN probe got no answer.
/// Anything else asks for nothing (Asker::Other). What a browser says is one
/// yes, never a port, a protocol or an address.
///
/// Wanted, the guard asks the router for 443 (one run at a time: it is the
/// guard's). What came of it is told back (settled()): held, or not, in
/// which case nothing is asked again for kRetryMs. Held, it is kept while a
/// browser is connected through it (arrived(), left(): a connection the
/// shared TCP port's ICE gate let in, never an ask), and for kGraceMs after
/// the last one left, or after it was held with none. Then it is no longer
/// wanted, and the guard takes it off the router, the removal read back.
/// One asked for meanwhile is wanted again: the guard, which does one thing
/// at a time, keeps it or asks for it anew.
///
/// Free of Qt's event loop and of the clock: the caller passes the time, and
/// asks when the next tick is due. On one thread, the main one.
namespace mw::tcp443 {

enum class Asker
{
    /// Something that has no say: nothing is asked for.
    Other,
    /// A tunnel's browser, through the introduction server, before any
    /// session: bounded by kRendezvousPerHour.
    Rendezvous,
    /// A signed-in session or an invited player's.
    Session,
};

/// Every switch TCP 443 takes, as the allocator reads them.
struct Switches
{
    /// The guard of 443 was set up for this run (RouterPortAllocator::setTcp443()).
    bool guard = false;
    /// UPnP and Internet Access are on.
    bool router = false;
    /// The run walks a DEV identity's ports.
    bool devPorts = false;
    /// The streams share ports in this run.
    bool sharedPorts = false;
    /// settings.json ice_tcp_passive, ice_tcp_443 (absent: the system's default).
    bool tcpPassive = false;
    bool tcp443 = false;
    /// settings.json router_ports_allowed is there, valid or not: 443 is
    /// never one of its ports.
    bool portList = false;
};

/// Whether 443 may be asked for at all: every switch it takes.
constexpr bool allowedBy(const Switches& s)
{
    return s.guard && s.router && !s.devPorts && s.sharedPorts && s.tcpPassive && s.tcp443 &&
           !s.portList;
}

class Demand
{
public:
    static constexpr uint64_t kGraceMs = 10 * 60 * 1000;
    static constexpr uint64_t kRetryMs = 60 * 1000;
    /// Asked for, and the guard has said nothing of it this long (a sweep
    /// that waits, a router that is gone, a look that never came): taken as
    /// a run that did not hold it. The guard's own run takes seconds.
    static constexpr uint64_t kOpeningMaxMs = 2 * 60 * 1000;
    static constexpr uint64_t kHourMs = 60 * 60 * 1000;
    static constexpr int kRendezvousPerHour = 6;

    enum class State
    {
        /// Not wanted.
        Idle,
        /// Wanted, and the guard has not said yet what came of it.
        Opening,
        /// Wanted, and the router forwards 443 here.
        Held,
    };

    struct Line
    {
        bool warning = false;
        QString text;
    };

    /// A browser needs 443, at `nowMs`. `allowed`: every switch 443 takes is
    /// on (ice_tcp_443, UPnP, Internet Access, the shared ports, no
    /// router_ports_allowed list) — the caller's to say. True when 443 is
    /// wanted from now on and was not: the caller has the guard look again.
    /// False otherwise, and nothing changes: already wanted (an ask keeps
    /// nothing open longer), not allowed, not from one that has a say, within
    /// kRetryMs of a failure, or over the hour's bound. What there is to
    /// say goes to `out`: the hour's bound, once an hour.
    bool ask(Asker who, bool allowed, uint64_t nowMs, QList<Line>* out = nullptr);

    /// A browser connected through 443 (its connection came in on the shared
    /// TCP port and its agent took it), or one such went.
    void arrived(uint64_t nowMs);
    void left(uint64_t nowMs);

    /// What the guard did with what it was last asked: `held`, the router
    /// forwards 443 here. True when that changes whether 443 is wanted (a
    /// run that did not hold it, an entry lost): the caller has the guard
    /// look again, which takes it off. Nothing is asked again for kRetryMs.
    bool settled(bool held, uint64_t nowMs);

    /// True when, at `nowMs`, 443 is no longer wanted: kGraceMs went by with
    /// no browser on it, or the guard said nothing of an ask for
    /// kOpeningMaxMs (then as settled(false): nothing asked for kRetryMs).
    /// The caller has the guard look again.
    bool tick(uint64_t nowMs);
    /// When the next tick is due, 0 for none.
    uint64_t dueAtMs() const;

    /// Every switch went off (allowed is false now): not wanted any more,
    /// and nothing is asked until a browser asks again. True when it was.
    bool off();

    bool wanted() const { return m_State != State::Idle; }
    State state() const { return m_State; }
    int peers() const { return m_Peers; }

private:
    State m_State = State::Idle;
    int m_Peers = 0;
    /// The end of the grace, 0 while a browser is on 443 or nothing is held.
    uint64_t m_GraceUntilMs = 0;
    /// No ask is taken before then: a run that did not hold 443.
    uint64_t m_RetryNotBeforeMs = 0;
    /// When the ask under way was taken (Opening).
    uint64_t m_OpeningSinceMs = 0;
    /// When each of the last hour's openings asked through the introduction
    /// server was taken.
    std::deque<uint64_t> m_RendezvousOpenings;
    /// The bound of the hour was said: not again before then.
    uint64_t m_BoundSaidUntilMs = 0;
};

} // namespace mw::tcp443
