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

#include <cstring>

#ifdef _WIN32
#include <tlhelp32.h>

#include <cstddef>
#include <vector>
#elif defined(__linux__)
#include <QDir>
#include <QFile>

#include <unistd.h>
#endif

namespace mw::sockettables {

bool serverName(const QByteArray& name, bool windows)
{
    if (windows) {
        const QByteArray lower = name.toLower();
        return lower == "moonlightweb.exe" || lower == "moonlightwebdev.exe";
    }
    return name == "MoonlightWeb" || name == "MoonlightWebDev";
}

Listens otherServerAmong(const QList<Process>& processes, uint32_t self, bool windows, bool known)
{
    for (const Process& p : processes)
        if (p.pid != self && serverName(p.name, windows)) return Listens::Yes;
    return known ? Listens::No : Listens::Unknown;
}

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

std::optional<uint64_t> ownerAmong(const QList<TcpSocket>& sockets, const TcpEnd& peer,
                                   const TcpEnd& ours, uint32_t established)
{
    if (peer.address.size() != 16 || ours.address.size() != 16) return std::nullopt;
    std::optional<uint64_t> owner;
    for (const TcpSocket& s : sockets) {
        if (s.state != established) continue;
        if (s.local.port != peer.port || s.local.address != peer.address) continue;
        if (s.remote.port != ours.port || s.remote.address != ours.address) continue;
        if (owner && *owner != s.owner) return std::nullopt;
        owner = s.owner;
    }
    return owner;
}

namespace {

/// The IPv4 prefix of an address mapped into IPv6.
const QByteArray kMappedV4("\0\0\0\0\0\0\0\0\0\0\xff\xff", 12);

/// Eight hexadecimal digits, and nothing else.
bool hexWord(const QByteArray& text, uint32_t& out)
{
    if (text.size() != 8) return false;
    out = 0;
    for (const char c : text) {
        int digit = 0;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else
            return false;
        out = (out << 4) | static_cast<uint32_t>(digit);
    }
    return true;
}

/// One end as /proc prints it, "0100007F:1F90": the address as 32-bit words
/// in hex, each in the kernel's byte order (this machine's), then the port.
bool procEnd(const QByteArray& field, TcpEnd& out)
{
    const qsizetype colon = field.indexOf(':');
    if (colon < 0 || field.indexOf(':', colon + 1) >= 0) return false;
    const QByteArray hex = field.left(colon);
    if (hex.size() != 8 && hex.size() != 32) return false;
    const QByteArray portHex = field.mid(colon + 1);
    uint32_t port = 0;
    if (portHex.size() != 4 || !hexWord(QByteArray(4, '0') + portHex, port)) return false;
    QByteArray bytes;
    for (qsizetype i = 0; i < hex.size(); i += 8) {
        uint32_t word = 0;
        if (!hexWord(hex.mid(i, 8), word)) return false;
        char raw[4];
        std::memcpy(raw, &word, sizeof(raw));
        bytes.append(raw, sizeof(raw));
    }
    out.address = bytes.size() == 4 ? kMappedV4 + bytes : bytes;
    out.port = static_cast<uint16_t>(port);
    return true;
}

} // namespace

std::optional<QList<TcpSocket>> inProcTcpTable(const QByteArray& text)
{
    // "sl local_address rem_address st tx_queue:rx_queue tr:tm->when retrnsmt
    // uid timeout inode ...", then one socket a line.
    const QList<QByteArray> lines = text.split('\n');
    if (lines.isEmpty() || !lines.first().contains("local_address") ||
        !lines.first().contains("uid"))
        return std::nullopt;
    QList<TcpSocket> out;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).simplified();
        if (line.isEmpty()) continue;
        const QList<QByteArray> fields = line.split(' ');
        if (fields.size() < 8) return std::nullopt;
        TcpSocket s;
        if (!procEnd(fields.at(1), s.local) || !procEnd(fields.at(2), s.remote))
            return std::nullopt;
        bool stateRead = false;
        bool uidRead = false;
        s.state = fields.at(3).toUInt(&stateRead, 16);
        s.owner = fields.at(7).toULongLong(&uidRead, 10);
        if (!stateRead || !uidRead) return std::nullopt;
        out.append(s);
    }
    return out;
}

std::optional<uint64_t> connectionOwnerInProc(const QByteArray& tcp, const QByteArray& tcp6,
                                              const TcpEnd& peer, const TcpEnd& ours)
{
    QList<TcpSocket> all;
    for (const QByteArray* text : {&tcp, &tcp6}) {
        if (text->isEmpty()) continue;
        const std::optional<QList<TcpSocket>> sockets = inProcTcpTable(*text);
        if (!sockets) return std::nullopt;
        all += *sockets;
    }
    return ownerAmong(all, peer, ours, kLinuxEstablished);
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

namespace {

/// The processes that own a socket listening in TCP, in one family.
template <typename Table> bool listeners(ULONG family, std::vector<DWORD>& out)
{
    std::vector<unsigned char> buffer;
    if (!fetch(
            [family](void* to, DWORD* size) {
                return GetExtendedTcpTable(to, size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER,
                                           0);
            },
            buffer))
        return false;
    if (buffer.size() < offsetof(Table, table)) return false;
    const auto* table = reinterpret_cast<const Table*>(buffer.data());
    const size_t room = (buffer.size() - offsetof(Table, table)) / sizeof(table->table[0]);
    if (table->dwNumEntries > room) return false;
    for (DWORD i = 0; i < table->dwNumEntries; ++i)
        out.push_back(table->table[i].dwOwningPid);
    return true;
}

} // namespace

Listens otherServer()
{
    std::vector<DWORD> owners;
    if (!listeners<MIB_TCPTABLE_OWNER_PID>(AF_INET, owners) ||
        !listeners<MIB_TCP6TABLE_OWNER_PID>(AF_INET6, owners))
        return Listens::Unknown;

    // What each one runs, from a snapshot taken after the tables: no process
    // is opened, so another account's and session 0's are read alike
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return Listens::Unknown;
    QList<Process> all;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more;
         more = Process32NextW(snapshot, &entry)) {
        entry.szExeFile[MAX_PATH - 1] = L'\0';
        all.append({static_cast<uint32_t>(entry.th32ProcessID),
                    QString::fromWCharArray(entry.szExeFile).toUtf8()});
    }
    const bool listed = GetLastError() == ERROR_NO_MORE_FILES;
    CloseHandle(snapshot);
    if (!listed) return Listens::Unknown;

    // A listener gone from the snapshot may have been anything: unknown
    QList<Process> listening;
    bool known = true;
    for (const DWORD pid : owners) {
        bool found = false;
        for (const Process& p : std::as_const(all)) {
            if (p.pid != pid) continue;
            listening.append(p);
            found = true;
            break;
        }
        if (!found) known = false;
    }
    return otherServerAmong(listening, static_cast<uint32_t>(GetCurrentProcessId()), true, known);
}

namespace {

/// An IPv4 end of a row: the address as the four bytes it holds in memory,
/// which are already in network order, mapped into IPv6.
TcpEnd v4End(DWORD address, DWORD port)
{
    return {kMappedV4 + QByteArray(reinterpret_cast<const char*>(&address), 4),
            ntohs(static_cast<u_short>(port))};
}

TcpEnd v6End(const UCHAR (&address)[16], DWORD port)
{
    return {QByteArray(reinterpret_cast<const char*>(address), 16),
            ntohs(static_cast<u_short>(port))};
}

/// Every TCP connection of one family, with the process that holds it.
template <typename Table, typename Row>
bool connections(ULONG family, QList<TcpSocket>& out, Row row)
{
    std::vector<unsigned char> buffer;
    if (!fetch(
            [family](void* to, DWORD* size) {
                return GetExtendedTcpTable(to, size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0);
            },
            buffer))
        return false;
    if (buffer.size() < offsetof(Table, table)) return false;
    const auto* table = reinterpret_cast<const Table*>(buffer.data());
    const size_t room = (buffer.size() - offsetof(Table, table)) / sizeof(table->table[0]);
    if (table->dwNumEntries > room) return false;
    for (DWORD i = 0; i < table->dwNumEntries; ++i)
        out.append(row(table->table[i]));
    return true;
}

} // namespace

std::optional<uint64_t> connectionOwner(const TcpEnd& peer, const TcpEnd& ours)
{
    QList<TcpSocket> all;
    if (!connections<MIB_TCPTABLE_OWNER_PID>(AF_INET, all, [](const MIB_TCPROW_OWNER_PID& r) {
            return TcpSocket{v4End(r.dwLocalAddr, r.dwLocalPort),
                             v4End(r.dwRemoteAddr, r.dwRemotePort), r.dwState, r.dwOwningPid};
        }))
        return std::nullopt;
    if (!connections<MIB_TCP6TABLE_OWNER_PID>(AF_INET6, all, [](const MIB_TCP6ROW_OWNER_PID& r) {
            return TcpSocket{v6End(r.ucLocalAddr, r.dwLocalPort),
                             v6End(r.ucRemoteAddr, r.dwRemotePort), r.dwState, r.dwOwningPid};
        }))
        return std::nullopt;
    return ownerAmong(all, peer, ours, kWindowsEstablished);
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

std::optional<uint64_t> connectionOwner(const TcpEnd& peer, const TcpEnd& ours)
{
    QByteArray tcp;
    QByteArray tcp6;
    if (!readTable("/proc/net/tcp", tcp)) return std::nullopt;
    // A kernel without IPv6 has no such table, and no such connection
    if (QFile::exists(QStringLiteral("/proc/net/tcp6")) && !readTable("/proc/net/tcp6", tcp6))
        return std::nullopt;
    return connectionOwnerInProc(tcp, tcp6, peer, ours);
}

namespace {

/// Whether /proc is mounted so as to hide other accounts' processes
/// (hidepid): then what it lists says nothing of them. True when that cannot
/// be read.
bool procHidesOthers()
{
    QByteArray mounts;
    if (!readTable("/proc/self/mounts", mounts)) return true;
    for (const QByteArray& line : mounts.split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        if (fields.size() < 4 || fields.at(1) != "/proc" || fields.at(2) != "proc") continue;
        for (const QByteArray& option : fields.at(3).split(',')) {
            if (!option.startsWith("hidepid=")) continue;
            const QByteArray value = option.mid(8);
            if (value != "0" && value != "off") return true;
        }
    }
    return false;
}

} // namespace

Listens otherServer()
{
    if (procHidesOthers()) return Listens::Unknown;
    const QDir proc(QStringLiteral("/proc"));
    if (!proc.exists()) return Listens::Unknown;
    QList<Process> all;
    bool known = true;
    for (const QString& entry : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool number = false;
        const uint32_t pid = entry.toUInt(&number);
        if (!number || pid == 0) continue;
        // The program's name as the kernel keeps it: at most 15 characters,
        // which both editions' fit
        QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
        if (!comm.open(QIODevice::ReadOnly)) {
            // Gone meanwhile says nothing; still there and unreadable does
            if (QFile::exists(QStringLiteral("/proc/%1").arg(pid))) known = false;
            continue;
        }
        all.append({pid, comm.read(64).trimmed()});
    }
    return otherServerAmong(all, static_cast<uint32_t>(::getpid()), false, known);
}

#else

Listens listens(uint16_t port, bool tcp)
{
    (void)port;
    (void)tcp;
    return Listens::Unknown;
}

Listens otherServer()
{
    return Listens::Unknown;
}

std::optional<uint64_t> connectionOwner(const TcpEnd& peer, const TcpEnd& ours)
{
    (void)peer;
    (void)ours;
    return std::nullopt;
}

#endif

} // namespace mw::sockettables
