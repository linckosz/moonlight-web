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

#include "IceMuxTcpCore.h"
#include "Tcp443Guard.h"

#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class IceMuxTcp;

namespace mw::tcp443 {

/// What the guard of TCP 443 learns by dialling (Tcp443Guard's IProbe), for
/// real. It dials an IPv4 address in figures, from this host, and waits, for
/// as long as a try has: so it is called on a thread that does nothing else
/// (Dialler), never on the one the router is asked from.
///
/// To know that a connection it dials arrives at this very host, it makes a
/// fragment for that one try (sixteen characters, which no agent's fragment
/// has), tells the listener of the shared ports to expect one connection
/// that names it (IceMuxTcp::expectToken), dials, and sends one frame: a STUN
/// Binding request that holds that name and nothing else, 46 bytes with its
/// length. The listener reads it as it reads any first frame, counts the
/// try, and closes the connection: nothing of it is handed to anyone, and
/// nothing is ever written back. Whoever else that frame reaches gets those
/// bytes, once, and can do nothing with them: the fragment is over by then.
///
/// A dial is told whole (Dial): the address, the port, and the shared port a
/// connection that leads here comes in on. The probe keeps nothing from one
/// dial to the next; all of it that two threads see is the mark that cuts a
/// dial short (cancel()).
class Probe final : public IProbe
{
public:
    explicit Probe(IceMuxTcp& listener);

    /// For the tests, before any dial: how long one try has, in all. Never
    /// above the time the listener gives a try (mw::icemuxtcp::kTokenMs).
    void setTryMs(uint64_t ms)
    {
        m_TryMs = ms < mw::icemuxtcp::kTokenMs ? ms : mw::icemuxtcp::kTokenMs;
    }

    /// That dial, made and waited for on the calling thread. ArrivesHere
    /// with no shared port named is no try at all.
    bool make(const Dial& dial) override;

    /// The dial under way ends within about a tenth of a second, and says
    /// no; so does every dial made from here on, until rearm(). The try of a
    /// dial cut short is over as that of any other: nothing is left expected
    /// at the listener. From any thread.
    void cancel();
    /// Dials are made again. Called by whoever has them made, before a dial
    /// nobody cancelled.
    void rearm() { m_Cancelled.store(false); }

    /// A fragment for one try: mw::icemuxtcp::kTokenSize characters an ICE
    /// fragment may hold, from the system's generator.
    static std::string freshFragment();
    /// The frame of a try, with its two bytes of length (RFC 4571): a STUN
    /// Binding request whose only attribute is a USERNAME that starts with
    /// `fragment`.
    static std::vector<uint8_t> knock(std::string_view fragment);

private:
    bool arrivesHere(const QString& address, uint16_t port, uint16_t leadsHereOn);
    bool answers(const QString& address, uint16_t port);

    IceMuxTcp& m_Listener;
    std::atomic<bool> m_Cancelled{false};
    uint64_t m_TryMs = mw::icemuxtcp::kTokenMs;
};

/// The thread the guard's dials are made on, one at a time (Tcp443Guard's
/// IDialler): a dial waits for up to the time a try has, and nothing the
/// router is asked for must wait behind it. It knows the probe and nothing
/// else: not the router, not the record, not what a run stands on. It says
/// nothing in the log.
///
/// It has one place for a dial. What came of a dial is given to `done` with
/// the number the dial was asked under, on this thread: `done` does nothing
/// but pass those two on to the thread the guard lives on.
///
/// ask(), cancel() and stop() are that thread's.
class Dialler final : public IDialler
{
public:
    Dialler(Probe& probe, std::function<void(uint32_t run, bool result)> done);
    /// stop()
    ~Dialler() override;
    Dialler(const Dialler&) = delete;
    Dialler& operator=(const Dialler&) = delete;

    /// The thread is started by the first dial asked for: a host that never
    /// asks for 443 has none. False, and nothing told later, when the place
    /// is taken (a dial waits there or is being made), when the thread cannot
    /// be started, or after stop().
    bool ask(uint32_t run, const Dial& dial) override;
    /// The dial in the place is cut short if it is being made, and not made
    /// at all if it still waited: `done` is told no for it, soon. With the
    /// place empty, nothing.
    void cancel() override;
    /// The same, and the thread is over by the time this returns: `done` is
    /// not called from here on, and no dial is taken any more.
    void stop();

private:
    void work();

    Probe& m_Probe;
    const std::function<void(uint32_t run, bool result)> m_Done;
    std::mutex m_Lock;
    std::condition_variable m_Wake;
    /// The one place: a dial waits there to be made...
    bool m_Waits = false;
    /// ...or is being made
    bool m_Making = false;
    /// The dial in the place is not wanted any more
    bool m_Cancelled = false;
    bool m_Stop = false;
    uint32_t m_Number = 0;
    Dial m_Dial;
    std::thread m_Thread;
};

} // namespace mw::tcp443
