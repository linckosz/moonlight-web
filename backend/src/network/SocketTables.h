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

#include <QByteArray>
#include <QList>

#include <cstdint>
#include <optional>

/// Whether something on this machine listens on a port, read from the
/// system's own tables of sockets.
///
/// Read, never tried: a bind made to find out would take the port for a
/// moment from a program that is starting, and in UDP can succeed on a port
/// that is in use.
namespace mw::sockettables {

enum class Listens
{
    No,
    Yes,
    /// The tables could not be read, or not understood: nothing is known.
    Unknown,
};

/// In UDP, a socket of any process bound to `port`, on any address. In TCP,
/// one that listens on it. Windows and Linux; Unknown on any other system.
/// On Linux the tables are those of this process's network namespace: a
/// program in another one (a container) is not seen.
Listens listens(uint16_t port, bool tcp);

/// The same from the text of one Linux table (/proc/net/udp, udp6, tcp or
/// tcp6). Unknown for a text that is not such a table.
Listens inProcTable(const QByteArray& text, uint16_t port, bool tcp);

// ── Another MoonlightWeb server on this machine ─────────────────────────────

/// One process, as the system lists it.
struct Process
{
    uint32_t pid = 0;
    /// Its program's file name: "MoonlightWeb.exe", "MoonlightWeb".
    QByteArray name;
};

/// Whether a program's file name is one of the editions', exactly:
/// MoonlightWeb and MoonlightWebDev, with ".exe" and in any case under Windows,
/// as they are elsewhere.
bool serverName(const QByteArray& name, bool windows);

/// Whether, among `processes`, one other than this process (`self`) runs one
/// of the editions' programs. This process's own stream workers count too: a
/// doubt, never a removal. `known`: false when the list may lack some (a
/// process the system would not say anything of), and then a list without
/// one says Unknown, not No.
Listens otherServerAmong(const QList<Process>& processes, uint32_t self, bool windows, bool known);

/// Whether another MoonlightWeb server runs on this machine, of either
/// edition, for any account and in any session.
///
/// Windows: the owners of the sockets that listen in TCP, their program read
/// from a snapshot of the processes, none opened. Every server listens in
/// TCP, on its web ports; the worker service and a worker between streams do
/// not, so neither counts. Linux: every process /proc shows, by the program
/// name the kernel gives it; with /proc mounted to hide other accounts'
/// processes, Unknown. Unknown on any other system, and whenever a table or
/// the list cannot be read.
Listens otherServer();

// ── Who holds the other end of a connection ─────────────────────────────────

/// One end of a TCP connection: its address as the 16 bytes of an IPv6 one,
/// an IPv4 address mapped into IPv6 (::ffff:a.b.c.d), and its port.
struct TcpEnd
{
    QByteArray address;
    uint16_t port = 0;
};

/// One socket of the system's table of TCP connections: its own end, the end
/// it is connected to, its state as the system numbers it, and who holds it:
/// an account's uid on Linux, a process id on Windows.
struct TcpSocket
{
    TcpEnd local;
    TcpEnd remote;
    uint32_t state = 0;
    uint64_t owner = 0;
};

/// A connection that is open, as each system numbers its state.
inline constexpr uint32_t kLinuxEstablished = 0x01;
inline constexpr uint32_t kWindowsEstablished = 5; // MIB_TCP_STATE_ESTAB

/// Who holds, among `sockets`, the far end of a connection this server
/// accepted: the established socket whose own end is `peer` and whose remote
/// end is `ours`, both ends matching, never one alone. Nothing when no socket
/// is that one, or when those that are have different owners: never a guess.
std::optional<uint64_t> ownerAmong(const QList<TcpSocket>& sockets, const TcpEnd& peer,
                                   const TcpEnd& ours, uint32_t established);

/// The sockets of one Linux table of TCP connections (/proc/net/tcp or tcp6)
/// as the kernel prints them, its addresses in this machine's byte order.
/// Nothing for a text that is not such a table.
std::optional<QList<TcpSocket>> inProcTcpTable(const QByteArray& text);

/// ownerAmong() over the text of /proc/net/tcp and that of /proc/net/tcp6,
/// either of which may be empty (a kernel without IPv6 has no tcp6): the uid.
/// Nothing when a text that is given is not understood.
std::optional<uint64_t> connectionOwnerInProc(const QByteArray& tcp, const QByteArray& tcp6,
                                              const TcpEnd& peer, const TcpEnd& ours);

/// The system's own answer, from its tables as they are at the moment of
/// asking, IPv4 and IPv6 alike: the uid that holds the far end on Linux, the
/// process id on Windows. Nothing on any other system, and whenever one of
/// the tables cannot be read. On Linux the tables are those of this process's
/// network namespace, the one a connection to its own loopback comes from.
std::optional<uint64_t> connectionOwner(const TcpEnd& peer, const TcpEnd& ours);

} // namespace mw::sockettables
