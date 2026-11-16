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

#include "network/RouterPortPools.h"

#include <QtGlobal>

// Where a slot's two signaling ports live, and how many slots fit.
//
// Slot 0 keeps the historical base; slots 1.. take two consecutive ports from a
// second base, just above slot 0's own ports (base, +1 relay, +2 control) and
// pushed past every block it would land on: both editions' media blocks, and
// the ports the streaming servers sharing the machine listen on. It used to
// start right after the media block: once the block moved above the --dev
// base, production and --dev would have started their slots at the same port.
// Both media blocks, whatever the identity: a stream socket of either edition
// listens in TCP on its block, on every address, and a signaling port of the
// other on loopback would stand in its way.
//
// The foreign blocks: Sunshine and Apollo's stock ports (47984-48010, RTSP on
// 48010), and MultiSeat's seats (48100 - 5 onwards, thirty ports a seat, eight
// seats) with Wolf's video and audio pings (48100, 48200) among them.
// Production lands at 48011-48058, --dev (48501) past the two media blocks, at
// 48598-48645.
//
// Header-only and free of the server, so tests/test_session_pool.cpp checks the
// plan the server runs with.
struct SlotPortPlan
{
    quint16 extraBase = 0;
    int maxSlots = 0;
};

inline SlotPortPlan planSlotPorts(quint16 signalingBase, int ceiling)
{
    struct Block
    {
        int first, last;
    };
    using mw::routerports::kDevMediaBasePort;
    using mw::routerports::kMediaBasePort;
    using mw::routerports::kMediaPortCount;
    const Block kTaken[] = {
        {47984, 48010},
        {48095, 48399},
        {kMediaBasePort, kMediaBasePort + kMediaPortCount - 1},
        {kDevMediaBasePort, kDevMediaBasePort + kMediaPortCount - 1},
    };
    int extra = signalingBase + 4;
    for (bool moved = true; moved;) {
        moved = false;
        const int top = extra + 2 * (ceiling - 1) + 1;
        for (const Block& b : kTaken)
            if (extra <= b.last && top >= b.first) {
                extra = b.last + 1;
                moved = true;
            }
    }
    // Only a signaling base picked by hand near the top of the port space can
    // run out of room; it gets what fits there.
    const int n = qBound(1, (65535 - extra + 1) / 2, ceiling);
    return SlotPortPlan{static_cast<quint16>(extra), n};
}
