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

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class IceMuxTcp;

namespace mw::tcp443 {

/// What the guard of TCP 443 learns by dialling (Tcp443Guard's IProbe), for
/// real. It dials an IPv4 address in figures, from this host, and waits: the
/// guard runs on a thread that may.
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
class Probe final : public IProbe
{
public:
    explicit Probe(IceMuxTcp& listener);

    /// The shared port a connection that leads here comes in on: the block
    /// port, which is the one dialled at the guard's step 3 and the one 443
    /// leads to at its step 5. 0: no try is made. Told by whoever runs the
    /// guard, on the guard's thread, before each run.
    void leadsHereOn(uint16_t port) override { m_LocalPort = port; }
    /// For the tests: how long one try has, in all. Never above the time the
    /// listener gives a try (mw::icemuxtcp::kTokenMs).
    void setTryMs(uint64_t ms)
    {
        m_TryMs = ms < mw::icemuxtcp::kTokenMs ? ms : mw::icemuxtcp::kTokenMs;
    }

    bool arrivesHere(const QString& address, uint16_t port) override;
    bool answers(const QString& address, uint16_t port) override;

    /// A fragment for one try: mw::icemuxtcp::kTokenSize characters an ICE
    /// fragment may hold, from the system's generator.
    static std::string freshFragment();
    /// The frame of a try, with its two bytes of length (RFC 4571): a STUN
    /// Binding request whose only attribute is a USERNAME that starts with
    /// `fragment`.
    static std::vector<uint8_t> knock(std::string_view fragment);

private:
    IceMuxTcp& m_Listener;
    uint16_t m_LocalPort = 0;
    uint64_t m_TryMs = mw::icemuxtcp::kTokenMs;
};

} // namespace mw::tcp443
