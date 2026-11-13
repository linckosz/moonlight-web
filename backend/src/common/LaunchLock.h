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

#include <mutex>

/// The lock a stream worker is started under.
///
/// Where the system cannot make a handle or a descriptor that a child process
/// does not get from the instant it exists (a connection as accept() returns
/// it, outside Linux), there is a moment between its making and the call that
/// keeps it from children. A process started in that moment would get it. So
/// whoever starts a stream worker as a child of this process holds this lock
/// for the start itself (StreamWorkerHost::startInProcess,
/// ConsoleProcess::start), and whoever makes such a handle on another thread
/// holds it from the making to that call (IceMuxTcp's accept): no worker
/// starts in between. A worker may run as another account than the server's;
/// what must never reach it is what this covers.
///
/// What it does not cover: the other processes this one starts, its helpers,
/// do not take it. They run as the server's own account and are handed no
/// socket to use; one started in that moment would hold a connection open for
/// as long as it lives, and nothing more.
///
/// Its place among the locks: taken alone. Never while IceMuxTcp's own lock
/// is held, and that one is never taken while this is held: the accept runs
/// outside it, and a worker is given its sockets before or after its start,
/// not during it.
namespace mw::launch {

inline std::mutex& lock()
{
    static std::mutex one;
    return one;
}

} // namespace mw::launch
