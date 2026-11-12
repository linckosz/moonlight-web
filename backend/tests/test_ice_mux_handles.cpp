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
    CHECK(StreamWorkerHost::muxHandoverFor(Launch::Plain) == Handover::Handed);
    // Above this process, which may not open it: it takes them itself
    CHECK(StreamWorkerHost::muxHandoverFor(Launch::TaskElevated) == Handover::Taken);
    CHECK(StreamWorkerHost::muxHandoverFor(Launch::ServiceAsSystem) == Handover::Taken);
    // The user sitting at the console, who may be another account: none
    CHECK(StreamWorkerHost::muxHandoverFor(Launch::ConsoleAsUser) == Handover::None);
}

} // namespace

#ifdef _WIN32

namespace {

using mw::icemuxhandles::adopt;
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

void run_ice_mux_handles_tests()
{
    whoGetsTheSockets();
    SECTION("IceMuxHandles: a socket copied out of the server is checked before it is used");
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const uint32_t self = GetCurrentProcessId();
    QString why;

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

void run_ice_mux_handles_tests()
{
    whoGetsTheSockets();
    SECTION("IceMuxHandles: Windows only");
    QString why;
    // Elsewhere nothing is taken, and it says so
    CHECK(mw::icemuxhandles::adopt(1, {{1, 1}}, [](const QString&) { return true; }, &why).empty());
    CHECK(!why.isEmpty());
}

#endif
