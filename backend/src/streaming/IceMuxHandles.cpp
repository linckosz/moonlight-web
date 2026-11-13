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

#include <algorithm>
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
#include <sys/uio.h>
#include <unistd.h>
#include <cerrno>
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

bool connectionChecked(uintptr_t socket, const std::vector<uint16_t>& ports, QString* why)
{
    const auto fail = [why](const QString& reason) {
        if (why) *why = reason;
        return false;
    };
    if (socket == kNoSocket) return fail(QStringLiteral("no connection was handed"));
#ifdef _WIN32
    const SOCKET s = static_cast<SOCKET>(socket);
    using Length = int;
    const auto option = [s](int name, int* value) {
        Length length = sizeof(*value);
        return ::getsockopt(s, SOL_SOCKET, name, reinterpret_cast<char*>(value), &length) == 0;
    };
#else
    const int s = static_cast<int>(socket);
    using Length = socklen_t;
    const auto option = [s](int name, int* value) {
        Length length = sizeof(*value);
        return ::getsockopt(s, SOL_SOCKET, name, value, &length) == 0;
    };
#endif
    int type = 0;
    if (!option(SO_TYPE, &type) || type != SOCK_STREAM)
        return fail(QStringLiteral("what was handed is not a stream socket"));
    int listens = 0;
    if (!option(SO_ACCEPTCONN, &listens) || listens != 0)
        return fail(QStringLiteral("what was handed listens: it is no connection"));

    sockaddr_storage local{};
    sockaddr_storage peer{};
    Length localLength = sizeof(local);
    Length peerLength = sizeof(peer);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &localLength) != 0)
        return fail(QStringLiteral("what was handed has no local address"));
    if (::getpeername(s, reinterpret_cast<sockaddr*>(&peer), &peerLength) != 0)
        return fail(QStringLiteral("what was handed is not connected"));
    if (local.ss_family != peer.ss_family ||
        (local.ss_family != AF_INET && local.ss_family != AF_INET6))
        return fail(QStringLiteral("the two ends of what was handed are not of one family"));
    const uint16_t port = local.ss_family == AF_INET6
                              ? ntohs(reinterpret_cast<const sockaddr_in6*>(&local)->sin6_port)
                              : ntohs(reinterpret_cast<const sockaddr_in*>(&local)->sin_port);
    if (port == 0 || std::find(ports.begin(), ports.end(), port) == ports.end())
        return fail(QStringLiteral("the connection handed is on port %1, which this worker does "
                                   "not share")
                        .arg(port));

    // Kept from whatever this process starts, and never a call that waits
#ifdef _WIN32
    if (!::SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0))
        return fail(QStringLiteral("the connection cannot be kept from the processes this one "
                                   "starts, error %1")
                        .arg(::GetLastError()));
    u_long on = 1;
    if (::ioctlsocket(s, FIONBIO, &on) != 0)
        return fail(QStringLiteral("the connection cannot be made non-blocking, error %1")
                        .arg(::WSAGetLastError()));
#else
    const int descriptorFlags = ::fcntl(s, F_GETFD);
    if (descriptorFlags < 0 || ::fcntl(s, F_SETFD, descriptorFlags | FD_CLOEXEC) != 0)
        return fail(QStringLiteral("the connection cannot be kept from the processes this one "
                                   "starts"));
    const int statusFlags = ::fcntl(s, F_GETFL);
    if (statusFlags < 0 || ::fcntl(s, F_SETFL, statusFlags | O_NONBLOCK) != 0)
        return fail(QStringLiteral("the connection cannot be made non-blocking"));
#endif
    return true;
}

bool connectionPorts(uintptr_t socket, uint16_t* local, uint16_t* remote)
{
    if (socket == kNoSocket || !local || !remote) return false;
    const auto portOf = [](const sockaddr_storage& name) -> uint16_t {
        if (name.ss_family == AF_INET6)
            return ntohs(reinterpret_cast<const sockaddr_in6*>(&name)->sin6_port);
        if (name.ss_family == AF_INET)
            return ntohs(reinterpret_cast<const sockaddr_in*>(&name)->sin_port);
        return 0;
    };
    sockaddr_storage here{}, there{};
#ifdef _WIN32
    int hereLength = sizeof(here), thereLength = sizeof(there);
    const SOCKET s = static_cast<SOCKET>(socket);
#else
    socklen_t hereLength = sizeof(here), thereLength = sizeof(there);
    const int s = static_cast<int>(socket);
#endif
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&here), &hereLength) != 0 ||
        ::getpeername(s, reinterpret_cast<sockaddr*>(&there), &thereLength) != 0)
        return false;
    *local = portOf(here);
    *remote = portOf(there);
    return *local != 0 && *remote != 0;
}

uintptr_t openConnection(const std::vector<uint8_t>& description,
                         const std::vector<uint16_t>& ports, QString* why)
{
    const auto fail = [why](const QString& reason) {
        if (why) *why = reason;
        return kNoSocket;
    };
#ifdef _WIN32
    if (description.size() != sizeof(WSAPROTOCOL_INFOW))
        return fail(QStringLiteral("what was handed is no socket's description"));
    WSAPROTOCOL_INFOW info;
    std::memcpy(&info, description.data(), sizeof(info));
    // Asked for from the start and said again of the handle below: a socket
    // made from a description has been seen to come out inheritable all the same
    const SOCKET s = ::WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info,
                                  0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET)
        return fail(QStringLiteral("the connection handed cannot be opened, error %1")
                        .arg(::WSAGetLastError()));
    QString reason;
    if (!connectionChecked(static_cast<uintptr_t>(s), ports, &reason)) {
        ::closesocket(s);
        return fail(reason);
    }
    return static_cast<uintptr_t>(s);
#else
    Q_UNUSED(description);
    Q_UNUSED(ports);
    return fail(QStringLiteral("not on this system"));
#endif
}

std::unique_ptr<Server> Server::open(uint32_t serverPid,
                                     const std::function<bool(const QString& image)>& isServer,
                                     QString* why)
{
    const auto fail = [why](const QString& reason) {
        if (why) *why = reason;
        return std::unique_ptr<Server>();
    };
#ifdef _WIN32
    if (serverPid == 0 || !isServer) return fail(QStringLiteral("no server to take from"));
    // As adopt(): to copy handles and to be asked what it is, no other right.
    const HANDLE process =
        ::OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, serverPid);
    if (!process)
        return fail(QStringLiteral("the server cannot be opened, error %1").arg(::GetLastError()));
    QString reason;
    DWORD mine = 0, theirs = 0;
    if (!isServer(imageOf(process)))
        reason = QStringLiteral("the process to take the connections from is not the server");
    else if (!integrityOf(::GetCurrentProcess(), &mine) || !integrityOf(process, &theirs))
        reason =
            QStringLiteral("the server's level cannot be read, error %1").arg(::GetLastError());
    else if (mine < theirs)
        reason = QStringLiteral("this process runs below the server");
    if (!reason.isEmpty()) {
        ::CloseHandle(process);
        return fail(reason);
    }
    std::unique_ptr<Server> server(new Server);
    server->m_Process = process;
    return server;
#else
    Q_UNUSED(serverPid);
    Q_UNUSED(isServer);
    return fail(QStringLiteral("not on this system"));
#endif
}

Server::~Server()
{
#ifdef _WIN32
    if (m_Process) ::CloseHandle(static_cast<HANDLE>(m_Process));
#endif
}

uintptr_t Server::copyConnection(uint64_t handle, const std::vector<uint16_t>& ports, QString* why)
{
    const auto fail = [why](const QString& reason) {
        if (why) *why = reason;
        return kNoSocket;
    };
#ifdef _WIN32
    if (!m_Process || handle == 0) return fail(QStringLiteral("no connection to copy"));
    // Asked for counts, taken or not: what the server names costs a copy each
    if (m_Copies >= kCopiesPerWindow)
        return fail(QStringLiteral("too many connections were copied since the window opened"));
    ++m_Copies;
    HANDLE copy = nullptr;
    if (!::DuplicateHandle(static_cast<HANDLE>(m_Process),
                           reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)),
                           ::GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
        !copy)
        return fail(
            QStringLiteral("the connection cannot be copied, error %1").arg(::GetLastError()));
    const SOCKET s = reinterpret_cast<SOCKET>(copy);
    int type = 0;
    int typeLength = sizeof(type);
    if (::getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &typeLength) != 0) {
        // Not a socket at all: closed as the handle it is
        ::CloseHandle(copy);
        return fail(QStringLiteral("what was copied is not a socket"));
    }
    QString reason;
    if (!connectionChecked(static_cast<uintptr_t>(s), ports, &reason)) {
        ::closesocket(s);
        return fail(reason);
    }
    return static_cast<uintptr_t>(s);
#else
    Q_UNUSED(handle);
    Q_UNUSED(ports);
    return fail(QStringLiteral("not on this system"));
#endif
}

#ifndef _WIN32
namespace {

// A message of the channel: the feed's number (4 bytes) and the frame's
// length (2 bytes), both little-endian, then the frame.
constexpr size_t kChannelHeader = 6;

bool keptAndNonBlocking(int fd)
{
    const int descriptorFlags = ::fcntl(fd, F_GETFD);
    const int statusFlags = ::fcntl(fd, F_GETFL);
    return descriptorFlags >= 0 && statusFlags >= 0 &&
           ::fcntl(fd, F_SETFD, descriptorFlags | FD_CLOEXEC) == 0 &&
           ::fcntl(fd, F_SETFL, statusFlags | O_NONBLOCK) == 0;
}

} // namespace
#endif

bool makeChannel(int* serverEnd, int* workerEnd)
{
#ifdef _WIN32
    Q_UNUSED(serverEnd);
    Q_UNUSED(workerEnd);
    return false;
#else
    // Messages, not a stream: one connection is one message, whole or not at
    // all. Where the system has it, a kind that also says when the other end
    // is gone.
#ifdef __linux__
    int type = SOCK_SEQPACKET;
#else
    int type = SOCK_DGRAM;
#endif
#ifdef SOCK_CLOEXEC
    type |= SOCK_CLOEXEC;
#endif
    int ends[2] = {-1, -1};
    if (::socketpair(AF_UNIX, type, 0, ends) != 0) return false;
    bool ready = keptAndNonBlocking(ends[0]) && keptAndNonBlocking(ends[1]);
#ifdef SO_NOSIGPIPE
    // A worker that is gone must cost the server a failed send, not a signal
    // (where MSG_NOSIGNAL is not to be had on every send)
    const int quiet = 1;
    for (const int end : ends)
        ready = ready && ::setsockopt(end, SOL_SOCKET, SO_NOSIGPIPE, &quiet, sizeof(quiet)) == 0;
#endif
    if (!ready) {
        ::close(ends[0]);
        ::close(ends[1]);
        return false;
    }
    *serverEnd = ends[0];
    *workerEnd = ends[1];
    return true;
#endif
}

int channelInherited(uint64_t number, QString* why)
{
    const auto fail = [why](const QString& reason) {
        if (why) *why = reason;
        return -1;
    };
#ifdef _WIN32
    Q_UNUSED(number);
    return fail(QStringLiteral("not on this system"));
#else
    if (!handedNumber(number))
        return fail(QStringLiteral("a channel without a number a server hands"));
    const int fd = static_cast<int>(number);
    // First, whatever else is true of it: it goes to no process this one
    // starts. Harmless on a number that turns out to be something else.
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef __linux__
    const int kind = SOCK_SEQPACKET;
#else
    const int kind = SOCK_DGRAM;
#endif
    int type = 0;
    socklen_t typeLen = sizeof(type);
    sockaddr_storage local{};
    socklen_t localLen = sizeof(local);
    // A pair's end has no name: what the system says of it is its family, or
    // nothing at all
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &typeLen) != 0 || type != kind ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &localLen) != 0 ||
        (localLen > 0 && local.ss_family != AF_UNIX))
        return fail(QStringLiteral("what this process was started with is not the server's "
                                   "channel"));
    if (!keptAndNonBlocking(fd)) return fail(QStringLiteral("the channel cannot be set up"));
    return fd;
#endif
}

bool sendConnection(int channel, int connection, uint32_t feed, const char* frame, size_t size)
{
#ifdef _WIN32
    Q_UNUSED(channel);
    Q_UNUSED(connection);
    Q_UNUSED(feed);
    Q_UNUSED(frame);
    Q_UNUSED(size);
    return false;
#else
    if (channel < 0 || connection < 0 || !frame || size < kFrameMin || size > kFrameMax)
        return false;
    unsigned char header[kChannelHeader] = {
        static_cast<unsigned char>(feed),       static_cast<unsigned char>(feed >> 8),
        static_cast<unsigned char>(feed >> 16), static_cast<unsigned char>(feed >> 24),
        static_cast<unsigned char>(size),       static_cast<unsigned char>(size >> 8)};
    iovec parts[2];
    parts[0].iov_base = header;
    parts[0].iov_len = sizeof(header);
    parts[1].iov_base = const_cast<char*>(frame);
    parts[1].iov_len = size;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message{};
    message.msg_iov = parts;
    message.msg_iovlen = 2;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsghdr* carried = CMSG_FIRSTHDR(&message);
    carried->cmsg_level = SOL_SOCKET;
    carried->cmsg_type = SCM_RIGHTS;
    carried->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(carried), &connection, sizeof(int));
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const ssize_t sent = ::sendmsg(channel, &message, flags);
    return sent == static_cast<ssize_t>(sizeof(header) + size);
#endif
}

Receipt receiveConnection(int channel, const std::vector<uint16_t>& ports, Received* out,
                          QString* why)
{
#ifdef _WIN32
    Q_UNUSED(channel);
    Q_UNUSED(ports);
    Q_UNUSED(out);
    if (why) *why = QStringLiteral("not on this system");
    return Receipt::Closed;
#else
    // One byte more than the longest message: a longer one is seen as such
    char buffer[kChannelHeader + kFrameMax + 1];
    iovec part;
    part.iov_base = buffer;
    part.iov_len = sizeof(buffer);
    // Room for one descriptor and no more: a message that carries several
    // comes out cut short, and is refused for it
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message{};
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    int flags = MSG_DONTWAIT;
#ifdef MSG_CMSG_CLOEXEC
    flags |= MSG_CMSG_CLOEXEC;
#endif
    const ssize_t got = ::recvmsg(channel, &message, flags);
    if (got < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? Receipt::Nothing
                                                                         : Receipt::Closed;
    if (got == 0 && message.msg_controllen == 0) return Receipt::Closed;

    // Every descriptor that came is this process's now: one is kept at most
    int fd = -1;
    int descriptors = 0;
    for (cmsghdr* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        if (c->cmsg_len < CMSG_LEN(0)) continue;
        const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
            int one = -1;
            std::memcpy(&one, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            if (one < 0) continue;
#ifndef MSG_CMSG_CLOEXEC
            ::fcntl(one, F_SETFD, FD_CLOEXEC);
#endif
            if (descriptors++ == 0)
                fd = one;
            else
                ::close(one);
        }
    }
    const auto refuse = [&](const QString& reason) {
        if (fd >= 0) ::close(fd);
        if (why) *why = reason;
        return Receipt::Refused;
    };
    if (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC))
        return refuse(QStringLiteral("a message carried more than one connection's"));
    if (descriptors != 1) return refuse(QStringLiteral("a message carried no connection"));
    if (static_cast<size_t>(got) < kChannelHeader)
        return refuse(QStringLiteral("a message too short for a connection's"));
    const auto byte = [&buffer](size_t i) {
        return static_cast<uint32_t>(static_cast<unsigned char>(buffer[i]));
    };
    const uint32_t feed = byte(0) | (byte(1) << 8) | (byte(2) << 16) | (byte(3) << 24);
    const size_t length = byte(4) | (byte(5) << 8);
    if (length < kFrameMin || length > kFrameMax ||
        static_cast<size_t>(got) != kChannelHeader + length)
        return refuse(QStringLiteral("a message whose length is not its frame's"));
    QString reason;
    if (!connectionChecked(static_cast<uintptr_t>(fd), ports, &reason)) return refuse(reason);

    out->socket = static_cast<uintptr_t>(fd);
    out->feed = feed;
    out->frame.assign(buffer + kChannelHeader, buffer + kChannelHeader + length);
    return Receipt::Connection;
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
