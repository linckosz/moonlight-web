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

#ifdef _WIN32
// Before anything that may bring windows.h in
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

#include "SocketTables.h"

#include <QList>

#ifdef _WIN32
#include <cstddef>
#include <vector>
#elif defined(__linux__)
#include <QFile>
#endif

namespace mw::sockettables {

Listens inProcTable(const QByteArray& text, uint16_t port, bool tcp)
{
    // "sl  local_address rem_address st ...", then one socket a line:
    // "609: 00000000:14E9 00000000:0000 07 ...", address and port in hex.
    const QList<QByteArray> lines = text.split('\n');
    if (lines.isEmpty() || !lines.first().contains("local_address")) return Listens::Unknown;
    constexpr unsigned kTcpListen = 0x0A;
    bool found = false;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).simplified();
        if (line.isEmpty()) continue;
        const QList<QByteArray> fields = line.split(' ');
        if (fields.size() < 4) return Listens::Unknown;
        const QByteArray& local = fields.at(1);
        const qsizetype colon = local.lastIndexOf(':');
        if (colon < 1) return Listens::Unknown;
        bool portRead = false;
        bool stateRead = false;
        const unsigned bound = local.mid(colon + 1).toUInt(&portRead, 16);
        const unsigned state = fields.at(3).toUInt(&stateRead, 16);
        if (!portRead || !stateRead || bound > 65535) return Listens::Unknown;
        if (bound != port) continue;
        // A TCP socket bound there that does not listen takes no connection
        if (tcp && state != kTcpListen) continue;
        found = true;
    }
    return found ? Listens::Yes : Listens::No;
}

#ifdef _WIN32

namespace {

/// One of the system's tables, whole: it may grow between the call that says
/// how large it is and the one that fills it.
template <typename Fetch> bool fetch(Fetch table, std::vector<unsigned char>& buffer)
{
    buffer.resize(32 * 1024);
    for (int attempt = 0; attempt < 5; ++attempt) {
        DWORD size = static_cast<DWORD>(buffer.size());
        const DWORD rc = table(buffer.data(), &size);
        if (rc == NO_ERROR) return true;
        if (rc != ERROR_INSUFFICIENT_BUFFER) return false;
        buffer.resize(static_cast<size_t>(size) + 4096);
    }
    return false;
}

/// Whether one row of a table read into `buffer` has `port` as its local one.
template <typename Table> Listens rowsHave(const std::vector<unsigned char>& buffer, uint16_t port)
{
    if (buffer.size() < offsetof(Table, table)) return Listens::Unknown;
    const auto* table = reinterpret_cast<const Table*>(buffer.data());
    const size_t room = (buffer.size() - offsetof(Table, table)) / sizeof(table->table[0]);
    if (table->dwNumEntries > room) return Listens::Unknown;
    for (DWORD i = 0; i < table->dwNumEntries; ++i)
        if (ntohs(static_cast<u_short>(table->table[i].dwLocalPort)) == port) return Listens::Yes;
    return Listens::No;
}

template <typename Table> Listens udpFamily(ULONG family, uint16_t port)
{
    std::vector<unsigned char> buffer;
    if (!fetch(
            [family](void* to, DWORD* size) {
                return GetExtendedUdpTable(to, size, FALSE, family, UDP_TABLE_OWNER_PID, 0);
            },
            buffer))
        return Listens::Unknown;
    return rowsHave<Table>(buffer, port);
}

template <typename Table> Listens tcpFamily(ULONG family, uint16_t port)
{
    std::vector<unsigned char> buffer;
    if (!fetch(
            [family](void* to, DWORD* size) {
                return GetExtendedTcpTable(to, size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER,
                                           0);
            },
            buffer))
        return Listens::Unknown;
    return rowsHave<Table>(buffer, port);
}

} // namespace

Listens listens(uint16_t port, bool tcp)
{
    const Listens v4 = tcp ? tcpFamily<MIB_TCPTABLE_OWNER_PID>(AF_INET, port)
                           : udpFamily<MIB_UDPTABLE_OWNER_PID>(AF_INET, port);
    if (v4 != Listens::No) return v4;
    return tcp ? tcpFamily<MIB_TCP6TABLE_OWNER_PID>(AF_INET6, port)
               : udpFamily<MIB_UDP6TABLE_OWNER_PID>(AF_INET6, port);
}

#elif defined(__linux__)

namespace {

/// A table of /proc, which has no size to read by: to its end, within reason.
bool readTable(const char* path, QByteArray& out)
{
    constexpr qsizetype kMost = 16 * 1024 * 1024;
    QFile file(QString::fromLatin1(path));
    if (!file.open(QIODevice::ReadOnly)) return false;
    out.clear();
    for (;;) {
        const QByteArray chunk = file.read(64 * 1024);
        if (chunk.isEmpty()) break;
        out += chunk;
        if (out.size() > kMost) return false;
    }
    return !out.isEmpty();
}

} // namespace

Listens listens(uint16_t port, bool tcp)
{
    QByteArray text;
    if (!readTable(tcp ? "/proc/net/tcp" : "/proc/net/udp", text)) return Listens::Unknown;
    const Listens v4 = inProcTable(text, port, tcp);
    if (v4 != Listens::No) return v4;
    // A kernel without IPv6 has no such table, and no such socket
    const char* v6 = tcp ? "/proc/net/tcp6" : "/proc/net/udp6";
    if (!QFile::exists(QString::fromLatin1(v6))) return Listens::No;
    if (!readTable(v6, text)) return Listens::Unknown;
    return inProcTable(text, port, tcp);
}

#else

Listens listens(uint16_t port, bool tcp)
{
    (void)port;
    (void)tcp;
    return Listens::Unknown;
}

#endif

} // namespace mw::sockettables
