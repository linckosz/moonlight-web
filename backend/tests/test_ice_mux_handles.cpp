/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * A stream worker that takes the server's shared sockets itself
 * (streaming/IceMuxHandles): what it copies out of another process is a UDP
 * socket bound to the port it was told, or it is closed. Checked in this
 * process, and from a second one started for it, which is how a worker and
 * its server stand.
 */
// Winsock first: nothing below may bring the older one in ahead of it
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "test_framework.h"
#include "network/IceMux.h"
#include "network/RouterPortPools.h"
#include "streaming/IceMuxHandles.h"
#include "streaming/StreamWorkerHost.h"

#include <QString>

#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

/// Whose process a worker is decides whether it gets the shared sockets, and
/// how (StreamWorkerHost::muxHandoverFor).
void whoGetsTheSockets()
{
    SECTION("IceMux: which worker gets the shared sockets, by the way it is started");
    using Launch = StreamWorkerHost::Launch;
    using Handover = StreamWorkerHost::MuxHandover;
    // This process's own account, at its own level: handed
    CHECK(StreamWorkerHost::muxHandoverFor(Launch::Plain, false) == Handover::Handed);
    // Above this process, which may not open it: it takes them itself
    CHECK(StreamWorkerHost::muxHandoverFor(Launch::TaskElevated, false) == Handover::Taken);
    CHECK(StreamWorkerHost::muxHandoverFor(Launch::ServiceAsSystem, false) == Handover::Taken);
    // The user sitting at the console: handed when that is the account the
    // installation was made by, none for any other account
    CHECK(StreamWorkerHost::muxHandoverFor(Launch::ConsoleAsUser, false) == Handover::None);
    CHECK(StreamWorkerHost::muxHandoverFor(Launch::ConsoleAsUser, true) == Handover::Handed);
    // The owner's verdict is the console launch's alone: the others are as
    // they are whatever it says
    CHECK(StreamWorkerHost::muxHandoverFor(Launch::Plain, true) == Handover::Handed);
    CHECK(StreamWorkerHost::muxHandoverFor(Launch::TaskElevated, true) == Handover::Taken);
    CHECK(StreamWorkerHost::muxHandoverFor(Launch::ServiceAsSystem, true) == Handover::Taken);
}

#ifdef _WIN32
using NativeSocket = SOCKET;
using NativeLength = int;
#else
using NativeSocket = int;
using NativeLength = socklen_t;
#endif

void closeNative(NativeSocket s)
{
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

/// A UDP socket bound to a loopback port of its own, made the plain way.
NativeSocket plainUdp(uint16_t* port)
{
    const NativeSocket s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    NativeLength length = sizeof(a);
    bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &length);
    *port = ntohs(a.sin_port);
    return s;
}

bool sendThree(NativeSocket from, uint16_t toPort, const char* what)
{
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(toPort);
    return sendto(from, what, 3, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 3;
}

/// The first byte of what `s` receives within `ms`, -1 when nothing comes.
int firstByteWithin(NativeSocket s, int ms, uint16_t* fromPort = nullptr)
{
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(s, &readable);
    timeval wait{ms / 1000, (ms % 1000) * 1000};
    if (select(static_cast<int>(s) + 1, &readable, nullptr, nullptr, &wait) <= 0) return -1;
    char got[16];
    sockaddr_in from{};
    NativeLength length = sizeof(from);
    if (recvfrom(s, got, sizeof(got), 0, reinterpret_cast<sockaddr*>(&from), &length) <= 0)
        return -1;
    if (fromPort) *fromPort = ntohs(from.sin_port);
    return got[0];
}

/// A worker's feed: its own port, its server's hop socket and nobody else,
/// and nothing the worker starts.
void theFeed()
{
    SECTION("IceMuxHandles: a feed hears its server's hop socket and nothing else");
    using mw::icemuxhandles::kNoSocket;
    using mw::icemuxhandles::openFeed;
    uint16_t hopPort = 0, strangerPort = 0, feedPort = 0, otherPort = 0;
    const NativeSocket hop = plainUdp(&hopPort);
    const NativeSocket stranger = plainUdp(&strangerPort);
    const uintptr_t feed = openFeed(hopPort, &feedPort);
    CHECK(feed != kNoSocket);
    if (feed != kNoSocket) {
        const auto mine = static_cast<NativeSocket>(feed);
        CHECK(feedPort != 0 && feedPort != hopPort);
        // Another sender to that port is not heard; the hop is
        CHECK(sendThree(stranger, feedPort, "no!"));
        CHECK(firstByteWithin(mine, 300) == -1);
        CHECK(sendThree(hop, feedPort, "yes"));
        CHECK(firstByteWithin(mine, 2000) == 'y');
#ifdef _WIN32
        DWORD flags = HANDLE_FLAG_INHERIT;
        CHECK(GetHandleInformation(reinterpret_cast<HANDLE>(feed), &flags) != 0);
        CHECK((flags & HANDLE_FLAG_INHERIT) == 0);
#else
        CHECK((fcntl(mine, F_GETFD) & FD_CLOEXEC) != 0);
#endif
        // Each agent of a worker has a feed of its own
        const uintptr_t second = openFeed(hopPort, &otherPort);
        CHECK(second != kNoSocket && otherPort != 0 && otherPort != feedPort);
        if (second != kNoSocket) closeNative(static_cast<NativeSocket>(second));
        closeNative(mine);
    }
    closeNative(hop);
    closeNative(stranger);
}

} // namespace

#ifdef _WIN32

namespace {

using mw::icemuxhandles::adopt;
using mw::icemuxhandles::kNoSocket;
using mw::icemuxhandles::Wanted;

sockaddr_in loopback(uint16_t port)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    return a;
}

/// A socket of `type` bound to a loopback port of its own.
SOCKET boundSocket(int type, uint16_t* port)
{
    const SOCKET s = socket(AF_INET, type, 0);
    sockaddr_in a = loopback(0);
    int len = sizeof(a);
    bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    *port = ntohs(a.sin_port);
    return s;
}

/// What `s` receives within `ms`, or -1.
int readWithin(SOCKET s, char* buffer, int size, int ms)
{
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(s, &readable);
    timeval wait{ms / 1000, (ms % 1000) * 1000};
    if (select(0, &readable, nullptr, nullptr, &wait) <= 0) return -1;
    return recv(s, buffer, size, 0);
}

QString ownImage()
{
    wchar_t path[32768];
    const DWORD size = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return QString::fromWCharArray(path, static_cast<qsizetype>(size));
}

bool isThisTest(const QString& image)
{
    return !image.isEmpty() && image.compare(ownImage(), Qt::CaseInsensitive) == 0;
}

/// Whether a handle is still one this process can use.
bool handleOpen(HANDLE h)
{
    DWORD flags = 0;
    return GetHandleInformation(h, &flags) != 0;
}

/// Whether a process this one starts gets the socket by being started.
bool inherited(uintptr_t socket)
{
    DWORD flags = 0;
    if (!GetHandleInformation(reinterpret_cast<HANDLE>(socket), &flags)) return true;
    return (flags & HANDLE_FLAG_INHERIT) != 0;
}

/// This test started again as `arguments`, with or without every handle of
/// this process that can be inherited: its exit code, 99 when it did not run.
DWORD secondProcess(const std::wstring& arguments, bool withHandles)
{
    const std::wstring command = L"\"" + ownImage().toStdWString() + L"\" " + arguments;
    std::vector<wchar_t> line(command.begin(), command.end());
    line.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, withHandles ? TRUE : FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child))
        return 99;
    DWORD code = 99;
    if (WaitForSingleObject(child.hProcess, 20000) == WAIT_OBJECT_0)
        GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return code;
}

} // namespace

/// The second process of the test below: `run_tests --adopt-socket <pid>
/// <handle> <port> <dest>`. It takes the socket out of the first, as a worker
/// does out of its server, and sends one datagram through it. 0 when it did.
int run_ice_mux_adopt_child(int argc, char** argv)
{
    if (argc != 6) return 2;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 3;
    const auto pid = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
    const uint64_t handle = std::strtoull(argv[3], nullptr, 10);
    const auto port = static_cast<uint16_t>(std::strtoul(argv[4], nullptr, 10));
    const auto dest = static_cast<uint16_t>(std::strtoul(argv[5], nullptr, 10));
    QString why;
    const std::vector<uintptr_t> got = adopt(pid, {Wanted{handle, port}}, isThisTest, &why);
    if (got.size() != 1) {
        std::fprintf(stderr, "  (child: %s)\n", why.toUtf8().constData());
        return 4;
    }
    const sockaddr_in to = loopback(dest);
    const int sent = sendto(static_cast<SOCKET>(got[0]), "taken", 5, 0,
                            reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    closesocket(static_cast<SOCKET>(got[0]));
    return sent == 5 ? 0 : 5;
}

/// Another second process: `run_tests --inherited-socket <handle> <port>`,
/// started with whatever of the first can be inherited. 7 when the handle is,
/// in this process, a UDP socket bound to that port; 0 when it is not.
int run_ice_mux_inherit_child(int argc, char** argv)
{
    if (argc != 4) return 2;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 3;
    const auto s = static_cast<SOCKET>(std::strtoull(argv[2], nullptr, 10));
    const auto port = static_cast<uint16_t>(std::strtoul(argv[3], nullptr, 10));
    sockaddr_storage bound{};
    int boundLen = sizeof(bound);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) return 0;
    uint16_t boundPort = 0;
    if (bound.ss_family == AF_INET6)
        boundPort = ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port);
    else if (bound.ss_family == AF_INET)
        boundPort = ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
    return boundPort == port ? 7 : 0;
}

int run_ice_mux_started_child(int, char**)
{
    return 2;
}

int run_ice_mux_holds_child(int, char**)
{
    return 2;
}

void run_ice_mux_handles_tests()
{
    whoGetsTheSockets();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    theFeed();
    const uint32_t self = GetCurrentProcessId();
    QString why;
    // Descriptors left open across a start are another system's way
    CHECK(mw::icemuxhandles::inherited({{3, 48550}}, &why).empty());
    CHECK(!why.isEmpty());
    SECTION("IceMuxHandles: a socket copied out of the server is checked before it is used");

    uint16_t sharedPort = 0, otherPort = 0, sinkPort = 0, tcpPort = 0;
    const SOCKET shared = boundSocket(SOCK_DGRAM, &sharedPort);
    const SOCKET other = boundSocket(SOCK_DGRAM, &otherPort);
    const SOCKET sink = boundSocket(SOCK_DGRAM, &sinkPort);
    const SOCKET tcp = boundSocket(SOCK_STREAM, &tcpPort);
    const auto number = [](SOCKET s) { return static_cast<uint64_t>(s); };
    char got[64];
    const int room = static_cast<int>(sizeof(got));

    // Two sockets, in the order asked: copies, not the handles themselves
    std::vector<uintptr_t> copies =
        adopt(self, {{number(shared), sharedPort}, {number(other), otherPort}}, isThisTest, &why);
    CHECK(copies.size() == 2);
    if (copies.size() == 2) {
        CHECK(copies[0] != static_cast<uintptr_t>(shared));
        CHECK(copies[0] != copies[1]);
        // A copy is this process's alone: nothing it starts gets it
        CHECK(!inherited(copies[0]) && !inherited(copies[1]));
        // What is sent through a copy leaves from the shared port
        const sockaddr_in to = loopback(sinkPort);
        CHECK(sendto(static_cast<SOCKET>(copies[0]), "hello", 5, 0,
                     reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 5);
        sockaddr_in from{};
        int fromLen = sizeof(from);
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(sink, &readable);
        timeval wait{2, 0};
        CHECK(select(0, &readable, nullptr, nullptr, &wait) == 1);
        CHECK(recvfrom(sink, got, room, 0, reinterpret_cast<sockaddr*>(&from), &fromLen) == 5);
        CHECK(ntohs(from.sin_port) == sharedPort);
        // Closing the copies leaves the server's own sockets as they were
        closesocket(static_cast<SOCKET>(copies[0]));
        closesocket(static_cast<SOCKET>(copies[1]));
        CHECK(sendto(shared, "again", 5, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) ==
              5);
        CHECK(readWithin(sink, got, room, 2000) == 5);
    }

    SECTION("IceMuxHandles: anything else is closed, and nothing is kept of the lot");
    // Not the port that was said
    why.clear();
    CHECK(adopt(self, {{number(shared), otherPort}}, isThisTest, &why).empty());
    CHECK(!why.isEmpty());
    // A socket of another kind
    CHECK(adopt(self, {{number(tcp), tcpPort}}, isThisTest, &why).empty());
    // A handle that is no socket at all: its copy is closed, the handle is not
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(adopt(self, {{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(event)), sharedPort}},
                isThisTest, &why)
              .empty());
    CHECK(handleOpen(event));
    CHECK(SetEvent(event) != 0);
    // A number that is no handle
    CHECK(adopt(self, {{0xFFFFFFF0ull, sharedPort}}, isThisTest, &why).empty());
    // One good and one bad: none is kept, and the good one's copy is closed
    // (the server's own stays usable)
    CHECK(adopt(self, {{number(shared), sharedPort}, {number(tcp), tcpPort}}, isThisTest, &why)
              .empty());
    {
        const sockaddr_in to = loopback(sinkPort);
        CHECK(sendto(shared, "still", 5, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) ==
              5);
        CHECK(readWithin(sink, got, room, 2000) == 5);
    }
    // None, too many, or one without a number
    CHECK(adopt(self, {}, isThisTest, &why).empty());
    CHECK(adopt(self,
                std::vector<Wanted>(mw::icemuxhandles::kMax + 1, {number(shared), sharedPort}),
                isThisTest, &why)
              .empty());
    CHECK(adopt(self, {{0, sharedPort}}, isThisTest, &why).empty());
    CHECK(adopt(self, {{number(shared), 0}}, isThisTest, &why).empty());

    SECTION("IceMuxHandles: only from the server, and never from above it");
    // No server (a worker that has no pipes to one), or no way to know it
    CHECK(adopt(0, {{number(shared), sharedPort}}, isThisTest, &why).empty());
    CHECK(adopt(self, {{number(shared), sharedPort}}, nullptr, &why).empty());
    // A process that is not the server, whatever it holds
    why.clear();
    CHECK(adopt(
              self, {{number(shared), sharedPort}}, [](const QString&) { return false; }, &why)
              .empty());
    CHECK(why.contains(QStringLiteral("not the server")));
    // The image the check is given is the opened process's own
    QString seen;
    copies = adopt(
        self, {{number(shared), sharedPort}},
        [&seen](const QString& image) {
            seen = image;
            return true;
        },
        &why);
    CHECK(copies.size() == 1);
    CHECK(seen.compare(ownImage(), Qt::CaseInsensitive) == 0);
    for (const uintptr_t s : copies)
        closesocket(static_cast<SOCKET>(s));
    // A process that is gone
    CHECK(adopt(0x7FFFFFF0u, {{number(shared), sharedPort}}, isThisTest, &why).empty());

    SECTION("IceMuxHandles: the mux's own sockets, by the numbers it gives a worker above it");
    {
        using namespace mw::routerports;
        IceMux mux;
        std::string reason;
        CHECK(mux.start(&reason));
        // Two ports of the stream block nobody has, from its far end
        std::vector<uint16_t> held;
        for (int p = kMediaBasePort + kMediaPortCount - 1; p >= kMediaBasePort && held.size() < 2;
             --p)
            if (mux.hold(static_cast<uint16_t>(p)) == IceMux::Held::NewlyBound)
                held.push_back(static_cast<uint16_t>(p));
        CHECK(held.size() == 2);
        if (held.size() == 2) {
            CHECK(mux.serve(IceMux::kStandard, held[0]));
            CHECK(mux.serve(IceMux::kBlock, held[1]));
            const IceMux::Share share = mux.shareByHandle();
            CHECK(share.sockets == mux.serving());
            CHECK(share.ports == held);
            CHECK(share.handles.size() == 2 && share.descriptions.empty());
            std::vector<Wanted> wanted;
            for (size_t i = 0; i < share.handles.size(); ++i)
                wanted.push_back({share.handles[i], share.ports[i]});
            // Sockets of IPv4 and IPv6 at once: each is known by its port all the same
            const std::vector<uintptr_t> taken = adopt(self, wanted, isThisTest, &why);
            CHECK(taken.size() == 2);
            for (const uintptr_t s : taken)
                closesocket(static_cast<SOCKET>(s));
            // Each copy must be the socket of its own port: the ports the
            // other way round are refused
            std::swap(wanted[0].port, wanted[1].port);
            CHECK(adopt(self, wanted, isThisTest, &why).empty());
            // Handed instead: described for one process, no number given
            const IceMux::Share described = mux.shareWith(self, &reason);
            CHECK(described.sockets == share.sockets && described.ports == held);
            CHECK(described.descriptions.size() == 2 && described.handles.empty());
            // The process it was described for opens each, checked like a
            // copy and its own alone like one
            for (size_t i = 0; i < described.descriptions.size(); ++i) {
                const uintptr_t opened =
                    mw::icemuxhandles::open(described.descriptions[i], held[i], &why);
                CHECK(opened != kNoSocket);
                if (opened == kNoSocket) continue;
                CHECK(!inherited(opened));
                closesocket(static_cast<SOCKET>(opened));
            }
            // Not the port that was said, or no description at all
            why.clear();
            CHECK(mw::icemuxhandles::open(described.descriptions[0], held[1], &why) == kNoSocket);
            CHECK(!why.isEmpty());
            CHECK(mw::icemuxhandles::open(described.descriptions[0], 0, &why) == kNoSocket);
            std::vector<uint8_t> cut = described.descriptions[0];
            cut.pop_back();
            CHECK(mw::icemuxhandles::open(cut, held[0], &why) == kNoSocket);
            CHECK(mw::icemuxhandles::open({}, held[0], &why) == kNoSocket);
            mux.release(described.lease);

            // The mux's own sockets go to no process for being started by
            // this one. A socket made the plain way does: that is what the
            // second process can see, and what it does not see of the mux's.
            for (const uint64_t handle : share.handles)
                CHECK(!inherited(static_cast<uintptr_t>(handle)));
            CHECK(inherited(static_cast<uintptr_t>(shared)));
            const auto seenBy = [](uint64_t handle, uint16_t port) {
                return secondProcess(L"--inherited-socket " + std::to_wstring(handle) + L" " +
                                         std::to_wstring(port),
                                     true);
            };
            CHECK(seenBy(number(shared), sharedPort) == 7);
            CHECK(seenBy(share.handles[0], share.ports[0]) == 0);
            CHECK(seenBy(share.handles[1], share.ports[1]) == 0);
            mux.release(share.lease);
            // The mux reads on: taking a copy and closing it took nothing from it
            CHECK(mux.healthy() && mux.holds(held[0]) && mux.holds(held[1]));
        }
        mux.stop();
    }

    SECTION("IceMuxHandles: taken by a second process, as a worker takes it from its server");
    {
        const std::wstring command = L"\"" + ownImage().toStdWString() + L"\" --adopt-socket " +
                                     std::to_wstring(self) + L" " +
                                     std::to_wstring(number(shared)) + L" " +
                                     std::to_wstring(sharedPort) + L" " + std::to_wstring(sinkPort);
        std::vector<wchar_t> line(command.begin(), command.end());
        line.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        // No window, and none of this process's handles: the child has only
        // what it copies out
        const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE,
                                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
        CHECK(started != 0);
        if (started) {
            sockaddr_in from{};
            int fromLen = sizeof(from);
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(sink, &readable);
            timeval wait{20, 0};
            const int ready = select(0, &readable, nullptr, nullptr, &wait);
            CHECK(ready == 1);
            if (ready == 1) {
                const int n =
                    recvfrom(sink, got, room, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
                CHECK(n == 5 && std::memcmp(got, "taken", 5) == 0);
                // Sent from the shared port, by the other process
                CHECK(ntohs(from.sin_port) == sharedPort);
            }
            CHECK(WaitForSingleObject(child.hProcess, 20000) == WAIT_OBJECT_0);
            DWORD code = 99;
            GetExitCodeProcess(child.hProcess, &code);
            CHECK(code == 0);
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
        }
    }

    CloseHandle(event);
    closesocket(shared);
    closesocket(other);
    closesocket(sink);
    closesocket(tcp);
}

#else

int run_ice_mux_adopt_child(int, char**)
{
    return 2;
}

int run_ice_mux_inherit_child(int, char**)
{
    return 2;
}

namespace {

bool isOpen(int fd)
{
    return fcntl(fd, F_GETFD) != -1;
}

/// Whether nothing this process starts gets `fd`.
bool keptHere(int fd)
{
    const int flags = fcntl(fd, F_GETFD);
    return flags != -1 && (flags & FD_CLOEXEC) != 0;
}

/// Whether `fd` is, in this process, a UDP socket bound to `port`.
bool holdsUdpOn(int fd, uint16_t port)
{
    int type = 0;
    socklen_t typeLength = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &typeLength) != 0 || type != SOCK_DGRAM)
        return false;
    sockaddr_storage bound{};
    socklen_t length = sizeof(bound);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &length) != 0) return false;
    if (bound.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port) == port;
    if (bound.ss_family == AF_INET)
        return ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port) == port;
    return false;
}

#ifdef __linux__
/// This test, started again as `arguments`, the way a server starts a worker:
/// the descriptors in `handed` are left open across the start, and no other.
/// Its exit code, 99 when it did not run.
int startedAgain(const std::vector<std::string>& arguments, const std::vector<int>& handed)
{
    char self[4096];
    const ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length <= 0) return 99;
    self[length] = '\0';
    std::vector<char*> argv;
    argv.push_back(self);
    for (const std::string& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) return 99;
    if (child == 0) {
        mw::icemuxhandles::leaveOpenAcrossExec(handed.data(), handed.size());
        execv(self, argv.data());
        _exit(98);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) return 99;
    return WEXITSTATUS(status);
}
#endif

} // namespace

/// A second process: `run_tests --holds-socket <descriptor> <port>`. 7 when
/// that number is, in it, a UDP socket bound to that port; 0 when it is not.
int run_ice_mux_holds_child(int argc, char** argv)
{
    if (argc != 4) return 2;
    const int fd = static_cast<int>(std::strtol(argv[2], nullptr, 10));
    const auto port = static_cast<uint16_t>(std::strtoul(argv[3], nullptr, 10));
    return holdsUdpOn(fd, port) ? 7 : 0;
}

/// Another: `run_tests --started-with-sockets <dest> <descriptor> <port> …`,
/// a worker as its server starts it. It takes the sockets it was started
/// with, sends one datagram through the first, and starts a process of its
/// own, which must hold none of them. 0 when all of that is so.
int run_ice_mux_started_child(int argc, char** argv)
{
    if (argc < 5 || (argc - 3) % 2 != 0) return 2;
    const auto dest = static_cast<uint16_t>(std::strtoul(argv[2], nullptr, 10));
    std::vector<mw::icemuxhandles::Wanted> told;
    for (int i = 3; i + 1 < argc; i += 2)
        told.push_back({std::strtoull(argv[i], nullptr, 10),
                        static_cast<uint16_t>(std::strtoul(argv[i + 1], nullptr, 10))});
    QString why;
    const std::vector<uintptr_t> got = mw::icemuxhandles::inherited(told, &why);
    if (got.size() != told.size()) {
        std::fprintf(stderr, "  (child: %s)\n", why.toUtf8().constData());
        return 4;
    }
    if (!sendThree(static_cast<int>(got[0]), dest, "tak")) return 5;
#ifdef __linux__
    // What this process starts gets none of them
    for (const mw::icemuxhandles::Wanted& w : told)
        if (startedAgain({"--holds-socket", std::to_string(w.handle), std::to_string(w.port)},
                         {}) != 0)
            return 6;
#endif
    return 0;
}

void run_ice_mux_handles_tests()
{
    using mw::icemuxhandles::inherited;
    using mw::icemuxhandles::Wanted;
    whoGetsTheSockets();
    theFeed();
    SECTION("IceMuxHandles: nothing is taken out of a server, or opened from a description");
    QString why;
    CHECK(mw::icemuxhandles::adopt(1, {{1, 1}}, [](const QString&) { return true; }, &why).empty());
    CHECK(!why.isEmpty());
    why.clear();
    CHECK(mw::icemuxhandles::open({1, 2, 3}, 1, &why) == mw::icemuxhandles::kNoSocket);
    CHECK(!why.isEmpty());

    SECTION("IceMuxHandles: the sockets a worker was started with");
    const auto number = [](int fd) { return static_cast<uint64_t>(fd); };
    uint16_t sinkPort = 0;
    const int sink = plainUdp(&sinkPort);
    {
        // Left open across the start: as the worker finds them, they would
        // go on to whatever it starts
        uint16_t firstPort = 0, secondPort = 0;
        const int first = plainUdp(&firstPort);
        const int second = plainUdp(&secondPort);
        CHECK(!keptHere(first) && !keptHere(second));
        why.clear();
        const std::vector<uintptr_t> got =
            inherited({{number(first), firstPort}, {number(second), secondPort}}, &why);
        // In the order told, the very descriptors, and kept from what it starts
        CHECK(got == (std::vector<uintptr_t>{static_cast<uintptr_t>(first),
                                             static_cast<uintptr_t>(second)}));
        CHECK(why.isEmpty());
        CHECK(keptHere(first) && keptHere(second));
        // What is sent through one leaves from the shared port
        uint16_t from = 0;
        CHECK(sendThree(first, sinkPort, "one"));
        CHECK(firstByteWithin(sink, 2000, &from) == 'o');
        CHECK(from == firstPort);
        close(first);
        close(second);
    }

    SECTION("IceMuxHandles: anything else is refused, and only what was handed is closed");
    {
        // The second is not the socket of the port said: nothing is kept. The
        // first was what the server said, handed for this, and is closed; the
        // second is left, kept from what this process starts.
        uint16_t firstPort = 0, secondPort = 0;
        const int first = plainUdp(&firstPort);
        const int second = plainUdp(&secondPort);
        why.clear();
        CHECK(inherited({{number(first), firstPort}, {number(second), firstPort}}, &why).empty());
        CHECK(!why.isEmpty());
        CHECK(!isOpen(first));
        CHECK(isOpen(second) && keptHere(second));
        close(second);
    }
    {
        // The first is not: the second is looked at all the same, and closed
        uint16_t firstPort = 0, secondPort = 0;
        const int first = plainUdp(&firstPort);
        const int second = plainUdp(&secondPort);
        CHECK(inherited({{number(first), secondPort}, {number(second), secondPort}}, &why).empty());
        CHECK(isOpen(first) && keptHere(first));
        CHECK(!isOpen(second));
        close(first);
    }
    {
        // The second of three is not: the first and the third were handed,
        // and neither stays open here to keep its port from the server
        uint16_t ports[3] = {0, 0, 0};
        const int a = plainUdp(&ports[0]);
        const int b = plainUdp(&ports[1]);
        const int c = plainUdp(&ports[2]);
        why.clear();
        CHECK(inherited({{number(a), ports[0]}, {number(b), ports[0]}, {number(c), ports[2]}}, &why)
                  .empty());
        CHECK(!why.isEmpty());
        CHECK(!isOpen(a) && !isOpen(c));
        CHECK(isOpen(b) && keptHere(b));
        close(b);
    }
    {
        // None of them is: nothing is closed
        uint16_t firstPort = 0, secondPort = 0;
        const int first = plainUdp(&firstPort);
        const int second = plainUdp(&secondPort);
        CHECK(inherited({{number(first), secondPort}, {number(second), firstPort}}, &why).empty());
        CHECK(isOpen(first) && isOpen(second));
        CHECK(keptHere(first) && keptHere(second));
        close(first);
        close(second);
    }
    {
        // A socket of another kind, on the very port said
        const int tcp = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(a);
        CHECK(bind(tcp, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0);
        CHECK(getsockname(tcp, reinterpret_cast<sockaddr*>(&a), &length) == 0);
        CHECK(inherited({{number(tcp), ntohs(a.sin_port)}}, &why).empty());
        CHECK(isOpen(tcp) && keptHere(tcp));
        close(tcp);
    }
    {
        // A descriptor that is no socket: some other one of this process. It
        // is not closed, and still works.
        int ends[2] = {-1, -1};
        CHECK(pipe(ends) == 0);
        CHECK(inherited({{number(ends[1]), 48550}}, &why).empty());
        CHECK(isOpen(ends[1]));
        CHECK(write(ends[1], "x", 1) == 1);
        close(ends[0]);
        close(ends[1]);
    }
    {
        // The standard three are never a handed socket, and are not touched
        int before[3];
        for (int fd = 0; fd < 3; ++fd)
            before[fd] = fcntl(fd, F_GETFD);
        for (int fd = 0; fd < 3; ++fd)
            CHECK(inherited({{number(fd), 48550}}, &why).empty());
        for (int fd = 0; fd < 3; ++fd)
            CHECK(fcntl(fd, F_GETFD) == before[fd]);
    }
    {
        // One number for two sockets, a number out of range, no port, none,
        // too many, a number that is no descriptor at all
        uint16_t port = 0;
        const int one = plainUdp(&port);
        CHECK(inherited({{number(one), port}, {number(one), port}}, &why).empty());
        CHECK(isOpen(one) && keptHere(one));
        CHECK(inherited({{uint64_t(1) << 40, port}}, &why).empty());
        CHECK(inherited({{number(one), 0}}, &why).empty());
        CHECK(inherited({}, &why).empty());
        std::vector<Wanted> many;
        std::vector<int> made;
        for (size_t i = 0; i <= mw::icemuxhandles::kMax; ++i) {
            uint16_t p = 0;
            made.push_back(plainUdp(&p));
            many.push_back({number(made.back()), p});
        }
        CHECK(inherited(many, &why).empty());
        for (const int fd : made) {
            CHECK(isOpen(fd) && keptHere(fd));
            close(fd);
        }
        CHECK(isOpen(one));
        close(one);
        CHECK(inherited({{number(one), port}}, &why).empty());
    }

#ifdef __linux__
    SECTION("IceMuxHandles: the mux's sockets cross one start, the worker's, and no other");
    {
        using namespace mw::routerports;
        IceMux mux;
        std::string reason;
        CHECK(mux.start(&reason));
        // Two ports of the stream block nobody has, from its far end
        std::vector<uint16_t> held;
        for (int p = kMediaBasePort + kMediaPortCount - 1; p >= kMediaBasePort && held.size() < 2;
             --p)
            if (mux.hold(static_cast<uint16_t>(p)) == IceMux::Held::NewlyBound)
                held.push_back(static_cast<uint16_t>(p));
        CHECK(held.size() == 2);
        if (held.size() == 2) {
            CHECK(mux.serve(IceMux::kStandard, held[0]));
            CHECK(mux.serve(IceMux::kBlock, held[1]));
            // What a worker is given: this process's own descriptors, by number
            const IceMux::Share share = mux.shareByHandle();
            CHECK(share.handles.size() == 2 && share.ports == held);
            std::vector<int> fds;
            for (const uint64_t handle : share.handles)
                fds.push_back(static_cast<int>(handle));
            const auto text = [](uint64_t n) { return std::to_string(n); };
            for (const int fd : fds)
                CHECK(keptHere(fd));

            // A process started without the hand-over holds neither
            for (size_t i = 0; i < fds.size(); ++i)
                CHECK(startedAgain({"--holds-socket", text(share.handles[i]), text(held[i])}, {}) ==
                      0);
            // Started with it, it does: that is what the answer above is worth
            CHECK(startedAgain({"--holds-socket", text(share.handles[0]), text(held[0])}, fds) ==
                  7);
            // …and only what it was handed
            CHECK(startedAgain({"--holds-socket", text(share.handles[1]), text(held[1])},
                               {fds[0]}) == 0);

            // A worker: it takes both, sends from the first shared port, and
            // what it starts in turn holds none
            CHECK(startedAgain({"--started-with-sockets", text(sinkPort), text(share.handles[0]),
                                text(held[0]), text(share.handles[1]), text(held[1])},
                               fds) == 0);
            uint16_t from = 0;
            CHECK(firstByteWithin(sink, 5000, &from) == 't');
            CHECK(from == held[0]);
            // Told the ports the other way round, it takes none
            CHECK(startedAgain({"--started-with-sockets", text(sinkPort), text(share.handles[0]),
                                text(held[1]), text(share.handles[1]), text(held[0])},
                               fds) == 4);

            // The server's own are as they were: kept from every other start,
            // and the mux reads on
            for (const int fd : fds)
                CHECK(keptHere(fd));
            CHECK(mux.healthy() && mux.holds(held[0]) && mux.holds(held[1]));
            mux.release(share.lease);
        }
        mux.stop();
    }
#endif
    close(sink);
}

#endif
