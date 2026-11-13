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

#include "IceMuxHandles.h"

#include <cstring>
#include <iterator>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <climits>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mw::icemuxhandles {

#ifdef _WIN32
namespace {

/// The integrity level of the process behind `process` (0x2000 medium, 0x3000
/// high, 0x4000 system), or false when Windows will not say.
bool integrityOf(HANDLE process, DWORD* level)
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
    BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    DWORD size = 0;
    const BOOL read =
        ::GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size);
    ::CloseHandle(token);
    if (!read) return false;
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer);
    const UCHAR count = *::GetSidSubAuthorityCount(label->Label.Sid);
    if (count == 0) return false;
    *level = *::GetSidSubAuthority(label->Label.Sid, static_cast<DWORD>(count - 1));
    return true;
}

QString imageOf(HANDLE process)
{
    wchar_t path[32768];
    DWORD size = static_cast<DWORD>(std::size(path));
    if (!::QueryFullProcessImageNameW(process, 0, path, &size)) return {};
    return QString::fromWCharArray(path, static_cast<qsizetype>(size));
}

/// Whether `s` is a UDP socket bound to `port`.
bool udpOn(SOCKET s, uint16_t port)
{
    int type = 0;
    int typeLen = sizeof(type);
    if (::getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &typeLen) != 0 ||
        type != SOCK_DGRAM)
        return false;
    sockaddr_storage bound{};
    int boundLen = sizeof(bound);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) return false;
    if (bound.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port) == port;
    if (bound.ss_family == AF_INET)
        return ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port) == port;
    return false;
}

/// The copy of one handle of `server`, when it is a UDP socket bound to
/// `port`. INVALID_SOCKET otherwise, the copy closed.
SOCKET copyOf(HANDLE server, const Wanted& wanted, QString* reason)
{
    HANDLE copy = nullptr;
    if (!::DuplicateHandle(server, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wanted.handle)),
                           ::GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
        !copy) {
        *reason = QStringLiteral("the socket of port %1 cannot be copied, error %2")
                      .arg(wanted.port)
                      .arg(::GetLastError());
        return INVALID_SOCKET;
    }
    // Whatever the server named, only a UDP socket bound to that port is kept.
    const SOCKET s = reinterpret_cast<SOCKET>(copy);
    int type = 0;
    int typeLen = sizeof(type);
    if (::getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &typeLen) != 0) {
        // Not a socket at all: closed as the handle it is
        ::CloseHandle(copy);
        *reason = QStringLiteral("what was copied for port %1 is not a socket").arg(wanted.port);
        return INVALID_SOCKET;
    }
    if (!udpOn(s, wanted.port)) {
        ::closesocket(s);
        *reason =
            QStringLiteral("what was copied is not the UDP socket of port %1").arg(wanted.port);
        return INVALID_SOCKET;
    }
    return s;
}

} // namespace
#else
namespace {

/// Whether `fd` is a UDP socket bound to `port`.
bool udpOn(int fd, uint16_t port)
{
    int type = 0;
    socklen_t typeLen = sizeof(type);
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &typeLen) != 0 || type != SOCK_DGRAM)
        return false;
    sockaddr_storage bound{};
    socklen_t boundLen = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) return false;
    if (bound.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port) == port;
    if (bound.ss_family == AF_INET)
        return ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port) == port;
    return false;
}

/// Whether a number is one a server hands a socket under.
bool handedNumber(uint64_t number)
{
    return number > 2 && number <= static_cast<uint64_t>(INT_MAX);
}

} // namespace
#endif

std::vector<uintptr_t> adopt(uint32_t serverPid, const std::vector<Wanted>& wanted,
                             const std::function<bool(const QString& image)>& isServer,
                             QString* why)
{
    const auto fail = [why](const QString& reason) {
        if (why) *why = reason;
        return std::vector<uintptr_t>();
    };
#ifdef _WIN32
    if (serverPid == 0 || wanted.empty() || wanted.size() > kMax || !isServer)
        return fail(QStringLiteral("no server to take the sockets from, or not one to four"));
    for (const Wanted& w : wanted)
        if (w.handle == 0 || w.port == 0) return fail(QStringLiteral("a socket without a number"));

    // To copy handles and to be asked what it is: no other right, and not kept.
    const HANDLE server =
        ::OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, serverPid);
    if (!server)
        return fail(QStringLiteral("the server cannot be opened, error %1").arg(::GetLastError()));

    QString reason;
    std::vector<uintptr_t> copies;
    DWORD mine = 0, theirs = 0;
    if (!isServer(imageOf(server))) {
        reason = QStringLiteral("the process to take the sockets from is not the server");
    } else if (!integrityOf(::GetCurrentProcess(), &mine) || !integrityOf(server, &theirs)) {
        reason =
            QStringLiteral("the server's level cannot be read, error %1").arg(::GetLastError());
    } else if (mine < theirs) {
        // The right to copy its handles is one over the whole process: only a
        // process at least as privileged as the server takes it.
        reason = QStringLiteral("this process runs below the server");
    } else {
        for (const Wanted& w : wanted) {
            const SOCKET s = copyOf(server, w, &reason);
            if (s == INVALID_SOCKET) break;
            copies.push_back(static_cast<uintptr_t>(s));
        }
    }
    ::CloseHandle(server);

    if (copies.size() != wanted.size()) {
        for (const uintptr_t s : copies)
            ::closesocket(static_cast<SOCKET>(s));
        return fail(reason);
    }
    return copies;
#else
    Q_UNUSED(serverPid);
    Q_UNUSED(wanted);
    Q_UNUSED(isServer);
    return fail(QStringLiteral("not on this system"));
#endif
}

uintptr_t open(const std::vector<uint8_t>& description, uint16_t port, QString* why)
{
    const auto fail = [why](const QString& reason) {
        if (why) *why = reason;
        return kNoSocket;
    };
#ifdef _WIN32
    if (description.size() != sizeof(WSAPROTOCOL_INFOW) || port == 0)
        return fail(
            QStringLiteral("what was handed for port %1 is no socket's description").arg(port));
    WSAPROTOCOL_INFOW info;
    std::memcpy(&info, description.data(), sizeof(info));
    const SOCKET s = ::WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info,
                                  0, WSA_FLAG_OVERLAPPED);
    if (s == INVALID_SOCKET)
        return fail(QStringLiteral("the socket of port %1 cannot be opened, error %2")
                        .arg(port)
                        .arg(::WSAGetLastError()));
    // Not inheritable, as a copy is not: no process this one starts gets it.
    // A socket made from a description comes out inheritable whatever is
    // asked of WSASocket, so it is said of the handle, and a socket it
    // cannot be said of is not kept.
    if (!::SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0)) {
        const DWORD error = ::GetLastError();
        ::closesocket(s);
        return fail(QStringLiteral("the socket of port %1 cannot be kept from the processes "
                                   "this one starts, error %2")
                        .arg(port)
                        .arg(error));
    }
    if (!udpOn(s, port)) {
        ::closesocket(s);
        return fail(QStringLiteral("what was handed is not the UDP socket of port %1").arg(port));
    }
    return static_cast<uintptr_t>(s);
#else
    Q_UNUSED(description);
    Q_UNUSED(port);
    return fail(QStringLiteral("not on this system"));
#endif
}

std::vector<uintptr_t> inherited(const std::vector<Wanted>& told, QString* why)
{
    const auto fail = [why](const QString& reason) {
        if (why) *why = reason;
        return std::vector<uintptr_t>();
    };
#ifdef _WIN32
    Q_UNUSED(told);
    return fail(QStringLiteral("not on this system"));
#else
    // First, whatever else is true of them: none goes to a process this one
    // starts. Harmless on a number that turns out to be something else.
    for (const Wanted& w : told)
        if (handedNumber(w.handle)) ::fcntl(static_cast<int>(w.handle), F_SETFD, FD_CLOEXEC);

    if (told.empty() || told.size() > kMax)
        return fail(QStringLiteral("no socket was handed, or more than four"));
    for (size_t i = 0; i < told.size(); ++i) {
        if (!handedNumber(told[i].handle) || told[i].port == 0)
            return fail(QStringLiteral("a socket without a number a server hands"));
        for (size_t j = 0; j < i; ++j)
            if (told[j].handle == told[i].handle)
                return fail(QStringLiteral("one number for two sockets"));
    }

    // Every one is looked at, whatever the others turn out to be
    std::vector<uintptr_t> sockets;
    uint16_t port = 0;
    for (const Wanted& w : told) {
        if (udpOn(static_cast<int>(w.handle), w.port))
            sockets.push_back(static_cast<uintptr_t>(w.handle));
        else if (port == 0)
            port = w.port;
    }
    if (sockets.size() == told.size()) return sockets;

    // Nothing is kept of the lot. Those that were what the server said were
    // handed for this, and are closed: left open here, a shared socket would
    // keep its port bound for as long as this process lives. One that was
    // not is not a socket this process was handed: whatever it is, it is
    // not closed here.
    for (const uintptr_t s : sockets)
        ::close(static_cast<int>(s));
    return fail(QStringLiteral("what this process was started with for port %1 is not its UDP "
                               "socket")
                    .arg(port));
#endif
}

std::vector<int> descriptorsToHand(const std::vector<uint64_t>& handles)
{
    std::vector<int> descriptors;
#ifdef _WIN32
    (void)handles;
#else
    if (handles.size() > kMax) return {};
    for (const uint64_t handle : handles) {
        if (!handedNumber(handle)) return {};
        const int fd = static_cast<int>(handle);
        for (const int earlier : descriptors)
            if (earlier == fd) return {};
        descriptors.push_back(fd);
    }
#endif
    return descriptors;
}

void leaveOpenAcrossExec(const int* descriptors, size_t count) noexcept
{
#ifdef _WIN32
    (void)descriptors;
    (void)count;
#else
    for (size_t i = 0; i < count; ++i) {
        const int flags = ::fcntl(descriptors[i], F_GETFD);
        if (flags != -1) ::fcntl(descriptors[i], F_SETFD, flags & ~FD_CLOEXEC);
    }
#endif
}

uintptr_t openFeed(uint16_t hopPort, uint16_t* port)
{
    const int room = 1024 * 1024;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sockaddr_in hop = local;
    hop.sin_port = htons(hopPort);
#ifdef _WIN32
    const SOCKET s = ::WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
                                  WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return kNoSocket;
    const int on = 1;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on),
                 sizeof(on));
    ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&room), sizeof(room));
    int length = sizeof(local);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 ||
        ::getsockname(s, reinterpret_cast<sockaddr*>(&local), &length) != 0 ||
        ::connect(s, reinterpret_cast<const sockaddr*>(&hop), sizeof(hop)) != 0) {
        ::closesocket(s);
        return kNoSocket;
    }
#else
#ifdef SOCK_CLOEXEC
    const int s = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
#else
    const int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s >= 0) ::fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
    if (s < 0) return kNoSocket;
    ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, &room, sizeof(room));
    socklen_t length = sizeof(local);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 ||
        ::getsockname(s, reinterpret_cast<sockaddr*>(&local), &length) != 0 ||
        ::connect(s, reinterpret_cast<const sockaddr*>(&hop), sizeof(hop)) != 0) {
        ::close(s);
        return kNoSocket;
    }
#endif
    *port = ntohs(local.sin_port);
    return static_cast<uintptr_t>(s);
}

} // namespace mw::icemuxhandles
