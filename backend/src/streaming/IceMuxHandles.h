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
#include <memory>
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

// ── TCP connections, handed over while the stream runs ──────────────────────
//
// With TCP on the shared ports the server accepts a connection, reads its
// first frame and hands the connection to the worker whose agent the frame
// names. Unlike the UDP sockets these arrive during the stream, one at a
// time. However one came — a description, a copy, a descriptor in a message —
// it is used only once it is known to be what it must be: connectionChecked().
// The agent then runs its own gate on the frame (juice_tcp_passive_offer):
// nothing here admits anything.

/// The bounds of the first frame (RFC 4571) a connection is handed with.
inline constexpr size_t kFrameMin = 20;
inline constexpr size_t kFrameMax = 1024;

/// The most copies a worker makes of the server's handles between two windows
/// (the server keeps the same bound on what it names).
inline constexpr int kCopiesPerWindow = 32;

/// Whether `socket` is a connection a worker was rightly handed: a stream
/// socket, connected, that listens for nothing, whose local port is one of
/// `ports` (the shared ports this worker holds) and whose two ends are of one
/// family. It is then kept from whatever this process starts, and made
/// non-blocking. False, with the reason in `why` (no address in it): the
/// caller closes the socket. Where the connection comes from is read from the
/// socket by whoever uses it, never told.
bool connectionChecked(uintptr_t socket, const std::vector<uint16_t>& ports, QString* why);

/// The two ports of the connection `socket`: this machine's, and its peer's.
/// False when it has not both. What a worker that copied a connection holds
/// its copy against: the server says which two it means.
bool connectionPorts(uintptr_t socket, uint16_t* local, uint16_t* remote);

/// A connection opened in this process from the description the server made
/// of it for this process alone, and checked. kNoSocket otherwise, with the
/// reason in `why` and nothing left open. Winsock must be started. The caller
/// closes it (closesocket). Windows only.
uintptr_t openConnection(const std::vector<uint8_t>& description,
                         const std::vector<uint16_t>& ports, QString* why);

/// The server process, held by a worker that runs above it, for as long as
/// its stream: such a worker copies each connection out of the server, as it
/// copied the UDP sockets when it started (adopt()), and under the same
/// conditions, checked once when this is opened. The right it holds is to
/// copy handles and to ask what the process is; a worker of that kind is at
/// least as privileged as the server, and the server gains nothing over it.
/// Windows only: open() gives null elsewhere.
class Server
{
public:
    static std::unique_ptr<Server> open(uint32_t serverPid,
                                        const std::function<bool(const QString& image)>& isServer,
                                        QString* why);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// A window opens for one of the worker's agents: copies are counted anew.
    void windowOpened() { m_Copies = 0; }

    /// A copy of the connection the server holds under `handle`, checked.
    /// With the server's own rights on it and no more, and the server's handle
    /// left as it is. kNoSocket, with the reason in `why` and nothing left
    /// open, when it cannot be copied, is not such a connection, or more than
    /// kCopiesPerWindow were asked for since the last window opened.
    uintptr_t copyConnection(uint64_t handle, const std::vector<uint16_t>& ports, QString* why);

private:
    Server() = default;
    void* m_Process = nullptr;
    int m_Copies = 0;
};

// Outside Windows a worker is the server's own child, and the server makes a
// channel for it before it starts it: a pair of local sockets, one end of
// which crosses that one start as the shared sockets do (leaveOpenAcrossExec).
// Each connection is one message on it: the descriptor, with the number of the
// agent's feed and the first frame.

/// A connection as it came out of the channel.
struct Received
{
    uintptr_t socket = kNoSocket;
    uint32_t feed = 0;
    std::vector<char> frame;
};

enum class Receipt
{
    /// `out` holds a connection, checked: the caller's to use and to close.
    Connection,
    /// Nothing to read now.
    Nothing,
    /// A message that is not one connection as a server sends it: whatever
    /// descriptor it carried is closed, and the reason is in `why`.
    Refused,
    /// The server's end is gone.
    Closed,
};

/// The two ends, each kept from whatever its process starts, non-blocking.
/// On Linux they are so from the instant they exist. Elsewhere they are made
/// so right after, and a process started by another thread in between would
/// get them: there the server makes the channel under the lock of the very
/// launch it is for, so that no other start falls in that instant. False on
/// Windows.
bool makeChannel(int* serverEnd, int* workerEnd);

/// The worker's side, as it starts: its end of the channel, by the number
/// the server left it open under across this process's start. Like the
/// sockets (inherited()), it is kept from whatever this process starts before
/// anything else is asked of it, then checked to be a local socket of the
/// channel's kind, and made non-blocking. -1, with the reason in `why`, when
/// it is not: a number that is not such a socket is left as it is, since it is
/// then some other descriptor of this process. -1 on Windows.
int channelInherited(uint64_t number, QString* why);

/// The server's side: one connection, for the agent of `feed`, with its
/// first frame. False when it did not go whole; the connection stays the
/// caller's either way. False on Windows.
bool sendConnection(int channel, int connection, uint32_t feed, const char* frame, size_t size);

/// The worker's side: at most one message is read. A message that carries no
/// descriptor, more than one, or more than a channel's message holds is
/// refused whole. On Linux the descriptor is closed on exec from the instant
/// it exists; elsewhere it is made so right after it is read, and for that
/// instant a process this worker started from another thread would get it: a
/// worker there starts no process while it streams. Receipt::Closed on
/// Windows.
Receipt receiveConnection(int channel, const std::vector<uint16_t>& ports, Received* out,
                          QString* why);

} // namespace mw::icemuxhandles
