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

#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

/// The shared UDP sockets of the server process (network/IceMux), taken by a
/// stream worker the server cannot hand them to.
///
/// A plain child is handed the sockets: the server makes a description of
/// each for that process alone. A worker started through the
/// launcher service or the elevated task runs above the server, which may not
/// open it. So that worker opens the server instead and copies the sockets
/// out of it, the way it takes the kept devices (HidParkingTransfer). The
/// server gains no right over the worker.
///
/// The worker, for the time of the copy, holds a right over the server: it
/// could copy any handle of it. That is accepted because such a worker is at
/// least as privileged as the server (SYSTEM, or the same user elevated), and
/// it is checked here: a process below the server does not copy.
///
/// The server is the less privileged side: what it names can be any handle of
/// its own. Nothing is sent through a copy before it is known to be what it
/// must be, a UDP socket bound to the port the server said.
///
/// Handed or taken, the socket is this process's and no other's: nothing this
/// process starts gets it by being started.
///
/// Outside Windows a worker is always the server's own child, and the server
/// leaves the sockets open across that one child's start: inherited().
namespace mw::icemuxhandles {

/// One socket to take: the server's own handle of it, as a number, and the
/// UDP port it must turn out to be bound to.
struct Wanted
{
    uint64_t handle = 0;
    uint16_t port = 0;
};

/// The most sockets taken at once (IceMux::kAgentSocketsMax).
inline constexpr size_t kMax = 4;

/// The value no socket has.
inline constexpr uintptr_t kNoSocket = ~uintptr_t(0);

/// One of the server's sockets, opened in this process from the description
/// the server made of it for this process alone (IceMux::shareWith()): how a
/// worker the server hands the sockets to gets each. Checked as a copy is:
/// kept only when it is a UDP socket bound to `port`. kNoSocket otherwise,
/// with the reason in `why` and nothing left open. Winsock must be started.
/// The caller closes it (closesocket). Windows only.
///
/// Called before this process starts any other: the socket can be inherited
/// for the instant between its making and its being kept from that. A path
/// of the worker that starts a process must not run alongside this call.
uintptr_t open(const std::vector<uint8_t>& description, uint16_t port, QString* why);

/// The sockets a worker was started with, outside Windows: `told` names each
/// by its descriptor number (Wanted::handle) and its port. In that order,
/// all of them or none.
///
/// Before anything else each number told is kept from whatever this process
/// starts (close-on-exec): this is called as the config line is read, before
/// the worker starts a thread or a process. Then each must be a UDP socket
/// bound to the port said. Empty, with the reason in `why`, when a number is
/// none a server hands (0 to 2, the same twice, out of range) or one is not
/// that socket. Of such a lot, those that were that socket are closed; a
/// number that was not is left as it is, since it is then some other
/// descriptor of this process and not one it was handed.
///
/// A shared socket is the server's and every worker's at once: it is sent
/// through, and closed. Its mode, its buffers and its options are set by
/// the server alone, and it is never shut down. Empty on Windows.
std::vector<uintptr_t> inherited(const std::vector<Wanted>& told, QString* why);

/// The server's side, before it starts a worker outside Windows: the
/// descriptors of that worker's share (IceMux::shareByHandle(), this
/// process's own numbers) as the config line names them and as
/// leaveOpenAcrossExec() takes them. Empty, and then the worker is started
/// with none, when one is none inherited() accepts: 0 to 2, which are the
/// child's own standard channels by the time it starts; a number beyond a
/// descriptor's; the same twice; none, or more than kMax. Empty on Windows.
std::vector<int> descriptorsToHand(const std::vector<uint64_t>& handles);

/// The server's side of inherited(), outside Windows: called in the process
/// a worker is being started in, between its fork and its exec, with the
/// descriptors of that worker's share. They alone then cross the exec; in
/// every other process the server starts, the system closes them. It only
/// changes a flag of each descriptor, in that process's own table: safe
/// where nothing else is. Nothing on Windows.
void leaveOpenAcrossExec(const int* descriptors, size_t count) noexcept;

/// A feed: a loopback UDP socket on a port of its own (`*port`), which hears
/// the server's hop socket (`hopPort`) and nothing else, and which nothing
/// this process starts gets. kNoSocket when it cannot be made.
uintptr_t openFeed(uint16_t hopPort, uint16_t* port);

/// Copies, in this process, of the sockets the process `serverPid` holds
/// under the handles in `wanted`, in that order. All of them or none: empty,
/// with the reason in `why` and nothing left open, when
///
///   the process cannot be opened, or `isServer` does not know its image (the
///   caller's check that it is the server it was started by, made here on the
///   very process the copies come from);
///   this process is less privileged than it;
///   a handle cannot be copied, or its copy is not a UDP socket bound to the
///   port given.
///
/// The process is opened to copy handles and to be asked what it is, nothing
/// more, and is let go before this returns. Winsock must be started. The
/// caller closes the copies (closesocket). Windows only: empty elsewhere.
std::vector<uintptr_t> adopt(uint32_t serverPid, const std::vector<Wanted>& wanted,
                             const std::function<bool(const QString& image)>& isServer,
                             QString* why);

} // namespace mw::icemuxhandles
