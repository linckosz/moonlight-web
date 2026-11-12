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

} // namespace mw::icemuxhandles
