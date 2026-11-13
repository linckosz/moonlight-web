/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * TCP on the shared ports, with its sockets (network/IceMuxTcp): that a port
 * is listened on while a connection is expected and no longer, that the
 * first frame is read and not a byte more, that the connection is kept for
 * the worker of the agent it names and lives on once that worker has it, and
 * what becomes of every other. What is decided is test_ice_mux_tcp_core.cpp's.
 * Everything here stays on this machine: the listeners take connections from
 * the loopback alone.
 */
#include "test_framework.h"
#include "network/IceMuxTcp.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using socklen_type = int;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
using socklen_type = socklen_t;
constexpr int INVALID_SOCKET = -1;
#endif

namespace {

using Bytes = std::vector<uint8_t>;
using mw::icemuxtcp::kAnswerMs;
using mw::icemuxtcp::kFirstFrameMs;

void shut(SOCKET s)
{
    if (s == INVALID_SOCKET) return;
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

void nonBlocking(SOCKET s)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

sockaddr_in loopback(uint16_t port)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    return a;
}

/// A STUN message of `type` with a USERNAME, behind its length (RFC 4571)
Bytes frameOf(uint16_t type, const char* username)
{
    const size_t n = std::strlen(username);
    const size_t padded = (n + 3) & ~size_t(3);
    Bytes b(2 + 24 + padded, 0);
    const size_t message = b.size() - 2;
    b[0] = static_cast<uint8_t>(message >> 8);
    b[1] = static_cast<uint8_t>(message);
    uint8_t* m = b.data() + 2;
    m[0] = static_cast<uint8_t>(type >> 8);
    m[1] = static_cast<uint8_t>(type);
    m[3] = static_cast<uint8_t>(4 + padded);
    m[4] = 0x21;
    m[5] = 0x12;
    m[6] = 0xA4;
    m[7] = 0x42;
    m[21] = 6; // USERNAME
    m[23] = static_cast<uint8_t>(n);
    std::memcpy(m + 24, username, n);
    return b;
}

Bytes requestFrame(const char* username)
{
    return frameOf(0x0001, username);
}

/// `count` loopback TCP ports nobody listens on right now, all different
std::vector<uint16_t> freeTcpPorts(size_t count)
{
    std::vector<SOCKET> held;
    std::vector<uint16_t> ports;
    for (size_t i = 0; i < count; ++i) {
        const SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a = loopback(0);
        socklen_type len = sizeof(a);
        bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
        getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
        ports.push_back(ntohs(a.sin_port));
        held.push_back(s);
    }
    for (const SOCKET s : held)
        shut(s);
    return ports;
}

/// A connection to the loopback's `port`; INVALID_SOCKET when none is made
/// within `ms` (nobody listens there: refused at once, or never answered).
SOCKET dial(uint16_t port, int ms = 400)
{
    const SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return s;
    nonBlocking(s);
    const sockaddr_in to = loopback(port);
    bool made = connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0;
    if (!made) {
#ifdef _WIN32
        const bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
        const bool pending = errno == EINPROGRESS;
#endif
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        timeval wait{ms / 1000, (ms % 1000) * 1000};
        int error = 0;
        socklen_type size = sizeof(error);
        made = pending && select(static_cast<int>(s) + 1, nullptr, &writable, &failed, &wait) > 0 &&
               FD_ISSET(s, &writable) && !FD_ISSET(s, &failed) &&
               getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) == 0 &&
               error == 0;
    }
    // Connected for good: it knows its other end
    sockaddr_in other{};
    socklen_type length = sizeof(other);
    if (!made || getpeername(s, reinterpret_cast<sockaddr*>(&other), &length) != 0) {
        shut(s);
        return INVALID_SOCKET;
    }
    return s;
}

bool nobodyListens(uint16_t port)
{
    const SOCKET s = dial(port, 250);
    shut(s);
    return s == INVALID_SOCKET;
}

bool sendAll(SOCKET s, const void* data, size_t size)
{
    return send(s, static_cast<const char*>(data), static_cast<int>(size), 0) ==
           static_cast<int>(size);
}

bool readable(SOCKET s, int ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval wait{ms / 1000, (ms % 1000) * 1000};
    return select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &wait) > 0;
}

/// What `s` receives within `ms`; -2 when nothing came at all
int readWithin(SOCKET s, void* buffer, size_t size, int ms)
{
    if (!readable(s, ms)) return -2;
    return static_cast<int>(recv(s, static_cast<char*>(buffer), static_cast<int>(size), 0));
}

/// The other end closed or reset the connection within `ms`, having sent
/// nothing on it
bool closedWithin(SOCKET s, int ms)
{
    char byte = 0;
    const int got = readWithin(s, &byte, 1, ms);
    return got == 0 || got == -1;
}

bool eventually(const std::function<bool()>& what, int ms)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!what()) {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

/// An end of this process's own on the connection `socket`, as a worker gets
/// one: another handle of the same connection
SOCKET ownEnd(uintptr_t socket)
{
#ifdef _WIN32
    WSAPROTOCOL_INFOW info{};
    if (WSADuplicateSocketW(static_cast<SOCKET>(socket), GetCurrentProcessId(), &info) != 0)
        return INVALID_SOCKET;
    return WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0,
                      WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#else
    return dup(static_cast<int>(socket));
#endif
}

/// What the listening thread says and hands, kept for the test to read
struct Heard
{
    std::mutex lock;
    std::vector<std::pair<uint32_t, uint32_t>> handed;
    std::vector<std::string> warned, told;

    void attach(IceMuxTcp& tcp)
    {
        tcp.setDeliver([this](uint32_t feed, uint32_t number) {
            const std::lock_guard<std::mutex> guard(lock);
            handed.emplace_back(feed, number);
        });
        tcp.setReporter(
            [this](const std::string& what) {
                const std::lock_guard<std::mutex> guard(lock);
                warned.push_back(what);
            },
            [this](const std::string& what) {
                const std::lock_guard<std::mutex> guard(lock);
                told.push_back(what);
            });
    }
    size_t count()
    {
        const std::lock_guard<std::mutex> guard(lock);
        return handed.size();
    }
    std::pair<uint32_t, uint32_t> at(size_t i)
    {
        const std::lock_guard<std::mutex> guard(lock);
        return i < handed.size() ? handed[i] : std::pair<uint32_t, uint32_t>{0, 0};
    }
    size_t warnings()
    {
        const std::lock_guard<std::mutex> guard(lock);
        return warned.size();
    }
    std::string everything()
    {
        const std::lock_guard<std::mutex> guard(lock);
        std::string all;
        for (const std::string& s : warned)
            all += s + "\n";
        for (const std::string& s : told)
            all += s + "\n";
        return all;
    }
};

#ifdef _WIN32
/// What the system gives for a connection accepted from a listener that is
/// itself kept from child processes: said, for the record. The product keeps
/// each accepted connection from them right after the accept, whatever this
/// says.
void acceptedHandleAtBirth()
{
    const SOCKET listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                                       WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    sockaddr_in at = loopback(0);
    socklen_type length = sizeof(at);
    if (listener == INVALID_SOCKET ||
        bind(listener, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) != 0 ||
        listen(listener, 1) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&at), &length) != 0) {
        shut(listener);
        return;
    }
    const SOCKET peer = dial(ntohs(at.sin_port));
    const SOCKET taken =
        peer == INVALID_SOCKET ? INVALID_SOCKET : accept(listener, nullptr, nullptr);
    DWORD flags = 0;
    if (taken != INVALID_SOCKET && GetHandleInformation(reinterpret_cast<HANDLE>(taken), &flags))
        std::fprintf(stderr,
                     "  (a connection accepted from a listener kept from child processes is %s "
                     "at birth)\n",
                     (flags & HANDLE_FLAG_INHERIT) ? "inheritable" : "not inheritable");
    shut(taken);
    shut(peer);
    shut(listener);
}
#endif

const char* const kUfrag = "hostAAAA1234";
const auto kNothing = [](uintptr_t, const uint8_t*, size_t) { return true; };

void listening()
{
    SECTION("IceMuxTcp: a port is listened on while a connection is expected, and no longer");
    IceMuxTcp tcp;
    tcp.keepToLoopback();
    Heard heard;
    heard.attach(tcp);
    const std::vector<uint16_t> ports = freeTcpPorts(2);
    const uint16_t port = ports[0], second = ports[1];

    // Not started: nothing is expected, and nothing can be
    CHECK(!tcp.open(1, kUfrag, {port}, 101));
    CHECK(!tcp.canListen({port}));
    CHECK(tcp.start());
    CHECK(!tcp.start());
    CHECK(tcp.running());

    // Nothing expected: nobody listens, and asking whether one could leaves
    // nobody listening either
    CHECK(nobodyListens(port));
    CHECK(tcp.canListen({port}));
    CHECK(tcp.canListen({0, second}));
    CHECK(!tcp.canListen({}));
    CHECK(nobodyListens(port) && nobodyListens(second));

    // Expected: listened on by the time open() returns
    CHECK(tcp.open(1, kUfrag, {port, second}, 101));
    CHECK(tcp.expects(1) && !tcp.expects(2));
    const SOCKET a = dial(port);
    const SOCKET b = dial(second);
    CHECK(a != INVALID_SOCKET && b != INVALID_SOCKET);
    CHECK(tcp.canListen({port}));
    // What the table refuses is refused here: the same agent or fragment twice
    CHECK(!tcp.open(1, "hostBBBB1234", {port}, 101));
    CHECK(!tcp.open(2, kUfrag, {port}, 102));
    CHECK(!tcp.open(2, "short", {port}, 102));
    CHECK(!tcp.open(2, "hostBBBB1234", {}, 102));

    // A second agent on one of the two: that one stays when the first is done
    CHECK(tcp.open(2, "hostBBBB1234", {second}, 102));
    tcp.close(1);
    CHECK(!tcp.expects(1) && tcp.expects(2));
    CHECK(eventually([port] { return nobodyListens(port); }, 2000));
    const SOCKET c = dial(second);
    CHECK(c != INVALID_SOCKET);
    tcp.close(2);
    CHECK(eventually([second] { return nobodyListens(second); }, 2000));
    // And what still waited for its frame has nobody to go to
    CHECK(closedWithin(a, 1500) && closedWithin(b, 1500) && closedWithin(c, 1500));
    shut(a);
    shut(b);
    shut(c);

    tcp.stop();
    CHECK(!tcp.running());
    tcp.stop();
    CHECK(heard.count() == 0 && heard.warnings() == 0);
}

void handingOver()
{
    SECTION("IceMuxTcp: the first frame, not a byte more, and the connection kept for its worker");
    IceMuxTcp tcp;
    tcp.keepToLoopback();
    Heard heard;
    heard.attach(tcp);
    CHECK(tcp.start());
    const uint16_t port = freeTcpPorts(1)[0];
    CHECK(tcp.open(7, kUfrag, {port}, 107));

    const SOCKET peer = dial(port);
    CHECK(peer != INVALID_SOCKET);
    const Bytes frame = requestFrame("hostAAAA1234:peerXXXX");
    // The frame in two pieces, then at once what follows it on the wire
    CHECK(sendAll(peer, frame.data(), 5));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(heard.count() == 0);
    CHECK(sendAll(peer, frame.data() + 5, frame.size() - 5));
    CHECK(sendAll(peer, "TAIL", 4));
    CHECK(eventually([&heard] { return heard.count() == 1; }, 3000));
    const auto [feed, number] = heard.at(0);
    CHECK(feed == 7 && number != 0);

    // Kept for that agent's worker and no other, under that number alone
    CHECK(!tcp.use(8, number, kNothing));
    CHECK(!tcp.use(7, number + 1, kNothing));
    CHECK(!tcp.use(7, number, nullptr));
    SOCKET mine = INVALID_SOCKET;
    bool asItCame = false, keptApart = false;
    const auto taking = [&](uintptr_t socket, const uint8_t* first, size_t size) {
        asItCame = size == frame.size() - 2 && std::memcmp(first, frame.data() + 2, size) == 0;
        // Nothing this process starts gets it
#ifdef _WIN32
        DWORD flags = HANDLE_FLAG_INHERIT;
        keptApart = GetHandleInformation(reinterpret_cast<HANDLE>(socket), &flags) != 0 &&
                    (flags & HANDLE_FLAG_INHERIT) == 0;
#else
        keptApart = (fcntl(static_cast<int>(socket), F_GETFD) & FD_CLOEXEC) != 0;
#endif
        mine = ownEnd(socket);
        return mine != INVALID_SOCKET;
    };
    CHECK(tcp.use(7, number, taking));
    CHECK(asItCame && keptApart && mine != INVALID_SOCKET);
    // What followed the frame was left in the connection, for whoever has it
    char got[16] = {};
    CHECK(readWithin(mine, got, sizeof(got), 2000) == 4 && std::memcmp(got, "TAIL", 4) == 0);

    // The worker has its end: this process lets go of its own, and the
    // connection lives
    tcp.answered(107, number);
    CHECK(!tcp.use(7, number, kNothing));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(sendAll(peer, "ping", 4));
    CHECK(readWithin(mine, got, sizeof(got), 2000) == 4 && std::memcmp(got, "ping", 4) == 0);
    // Nothing was ever written on it here: the first bytes the peer gets are
    // the worker's
    CHECK(sendAll(mine, "pong", 4));
    CHECK(readWithin(peer, got, sizeof(got), 2000) == 4 && std::memcmp(got, "pong", 4) == 0);

    // That agent is connected, and expects none any more. The next one is
    // reached on the very port, while the last one's connection lives.
    tcp.close(7);
    CHECK(eventually([port] { return nobodyListens(port); }, 2000));
#ifdef _WIN32
    // A process this one started and still holds a handle on, though it is
    // over: made now, so that nothing below waits on it
    void* gone = nullptr;
    {
        STARTUPINFOW start{};
        start.cb = sizeof(start);
        PROCESS_INFORMATION made{};
        wchar_t command[] = L"cmd.exe /c exit 0";
        if (CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &start, &made)) {
            gone = IceMuxTcp::holdProcess(made.dwProcessId);
            CHECK(gone != nullptr);
            WaitForSingleObject(made.hProcess, 5000);
            CloseHandle(made.hThread);
            CloseHandle(made.hProcess);
        }
    }
#endif
    CHECK(tcp.open(8, "hostBBBB1234", {port}, 108));
    const SOCKET next = dial(port);
    CHECK(next != INVALID_SOCKET);
    const Bytes other = requestFrame("hostBBBB1234:peerYYYY");
    CHECK(sendAll(next, other.data(), other.size()));
    CHECK(eventually([&heard] { return heard.count() == 2; }, 3000));
    CHECK(heard.at(1).first == 8);
    CHECK(sendAll(peer, "more", 4));
    CHECK(readWithin(mine, got, sizeof(got), 2000) == 4 && std::memcmp(got, "more", 4) == 0);

#ifdef _WIN32
    // Described for a worker's process, and for its agent's alone
    const uint32_t second = heard.at(1).second;
    std::vector<uint8_t> first;
    {
        // Described for a process that runs, by the handle held on it
        void* self = IceMuxTcp::holdProcess(GetCurrentProcessId());
        CHECK(self != nullptr);
        std::vector<uint8_t> description;
        CHECK(!tcp.describeFor(8, second, nullptr, &description, &first));
        CHECK(!tcp.describeFor(7, second, self, &description, &first));
        CHECK(tcp.describeFor(8, second, self, &description, &first));
        CHECK(description.size() == sizeof(WSAPROTOCOL_INFOW));
        // And opened from it: the same connection
        WSAPROTOCOL_INFOW info{};
        std::memcpy(&info, description.data(), sizeof(info));
        const SOCKET described =
            WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0,
                       WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
        CHECK(described != INVALID_SOCKET);
        CHECK(sendAll(next, "here", 4));
        CHECK(readWithin(described, got, sizeof(got), 2000) == 4);
        shut(described);
        IceMuxTcp::letGoOfProcess(self);
        // For a process that is gone: nothing is described
        if (gone) {
            CHECK(!tcp.describeFor(8, second, gone, &description, &first));
            IceMuxTcp::letGoOfProcess(gone);
        }
        CHECK(IceMuxTcp::holdProcess(0) == nullptr);
    }
#endif

    // A worker that says it will not have it: closed, and the peer sees it
    tcp.answered(108, 0);
    CHECK(closedWithin(next, 1500));
    tcp.close(8);
    shut(next);
    shut(mine);
    shut(peer);

    CHECK(eventually([&tcp] { return tcp.counters().answered == 2; }, 1000));
    const mw::icemuxtcp::Table::Counters c = tcp.counters();
    CHECK_EQ(c.handed, uint64_t(2));
    CHECK_EQ(c.unanswered, uint64_t(0));
    tcp.stop();
    // What was said of all this names no peer and no agent
    const std::string said = heard.everything();
    CHECK(said.find("connection(s)") != std::string::npos);
    CHECK(said.find("127.0.0.1") == std::string::npos);
    CHECK(said.find("hostAAAA") == std::string::npos && said.find("peerXXXX") == std::string::npos);
}

void refusing()
{
    SECTION("IceMuxTcp: every other connection is closed here");
    IceMuxTcp tcp;
    tcp.keepToLoopback();
    Heard heard;
    heard.attach(tcp);
    CHECK(tcp.start());
    const uint16_t port = freeTcpPorts(1)[0];
    CHECK(tcp.open(1, kUfrag, {port}, 101));

    // One at a time: a request for an agent nobody is, a response, an
    // indication, what is not STUN, and the lengths no frame has
    Bytes notStun(2 + 40, 0x16);
    notStun[0] = 0;
    notStun[1] = 40;
    const Bytes tooShort = {0, 19, 1, 2, 3};
    const Bytes tooLong = {0x04, 0x01, 1, 2, 3};
    const std::vector<Bytes> wires = {requestFrame("hostCCCC1234:x"),
                                      frameOf(0x0101, "hostAAAA1234:x"),
                                      frameOf(0x0011, "hostAAAA1234:x"),
                                      notStun,
                                      tooShort,
                                      tooLong};
    for (const Bytes& wire : wires) {
        const SOCKET s = dial(port);
        CHECK(s != INVALID_SOCKET);
        CHECK(sendAll(s, wire.data(), wire.size()));
        CHECK(closedWithin(s, 1500));
        shut(s);
    }
    mw::icemuxtcp::Table::Counters c = tcp.counters();
    CHECK_EQ(c.unrouted, uint64_t(4));
    CHECK_EQ(c.malformed, uint64_t(2));
    CHECK(heard.count() == 0);

    // One source holds two places: its third takes the place of its first
    const SOCKET first = dial(port);
    CHECK(eventually([&tcp] { return tcp.counters().accepted == 7; }, 1500));
    const SOCKET second = dial(port);
    CHECK(eventually([&tcp] { return tcp.counters().accepted == 8; }, 1500));
    const SOCKET third = dial(port);
    CHECK(first != INVALID_SOCKET && second != INVALID_SOCKET && third != INVALID_SOCKET);
    CHECK(closedWithin(first, 1500));
    CHECK(!readable(second, 100) && !readable(third, 100));
    CHECK_EQ(tcp.counters().evicted, uint64_t(1));
    // A peer that goes first frees its place
    shut(second);
    shut(third);
    shut(first);
    CHECK(eventually([&tcp] { return tcp.counters().ended == 2; }, 1500));

    // An agent is sent two its worker has not answered for: the third that
    // names it is closed here
    const Bytes mine = requestFrame("hostAAAA1234:x");
    const SOCKET one = dial(port);
    CHECK(sendAll(one, mine.data(), mine.size()));
    CHECK(eventually([&heard] { return heard.count() == 1; }, 3000));
    const SOCKET two = dial(port);
    CHECK(sendAll(two, mine.data(), mine.size()));
    CHECK(eventually([&heard] { return heard.count() == 2; }, 3000));
    const SOCKET three = dial(port);
    CHECK(sendAll(three, mine.data(), mine.size()));
    CHECK(closedWithin(three, 1500));
    CHECK(heard.count() == 2);
    CHECK_EQ(tcp.counters().bounded, uint64_t(1));
    shut(three);

    // Time: a connection that sends no frame is let go after two seconds and
    // not before, and so is one a worker says nothing of
    const SOCKET silent = dial(port);
    CHECK(silent != INVALID_SOCKET);
    CHECK(!readable(silent, 1000));
    CHECK(closedWithin(one, static_cast<int>(kAnswerMs)));
    CHECK(closedWithin(two, 1000));
    CHECK(closedWithin(silent, static_cast<int>(kFirstFrameMs)));
    c = tcp.counters();
    CHECK_EQ(c.unanswered, uint64_t(2));
    CHECK_EQ(c.timedOut, uint64_t(1));
    shut(one);
    shut(two);
    shut(silent);
    tcp.close(1);
    tcp.stop();
}

void busyPort()
{
    SECTION("IceMuxTcp: a port another program listens on");
    IceMuxTcp tcp;
    tcp.keepToLoopback();
    Heard heard;
    heard.attach(tcp);
    CHECK(tcp.start());
    const std::vector<uint16_t> ports = freeTcpPorts(2);
    const uint16_t taken = ports[0], other = ports[1];

    const SOCKET blocker = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    const int exclusive = 1;
    setsockopt(blocker, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));
#endif
    const sockaddr_in at = loopback(taken);
    CHECK(bind(blocker, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) == 0);
    CHECK(listen(blocker, 4) == 0);

    CHECK(!tcp.canListen({taken}));
    CHECK(tcp.canListen({taken, other}));
    // No port of its own can be listened on: nothing is expected for it, and
    // it is said, once
    CHECK(!tcp.open(1, kUfrag, {taken}, 101));
    CHECK(!tcp.expects(1));
    CHECK(heard.warnings() == 1);
    CHECK(!tcp.open(1, kUfrag, {taken}, 101));
    // One of two is enough
    CHECK(tcp.open(2, "hostBBBB1234", {taken, other}, 102));
    const SOCKET reached = dial(other);
    CHECK(reached != INVALID_SOCKET);
    // The other program goes: the port is listened on within the retry, and
    // that is said
    const size_t before = heard.warnings();
    shut(blocker);
    CHECK(eventually([&heard, before] { return heard.warnings() > before; }, 3000));
    const SOCKET late = dial(taken);
    CHECK(late != INVALID_SOCKET);
    CHECK(heard.everything().find("is listened on again") != std::string::npos);
    shut(reached);
    shut(late);
    tcp.stop();
}

void stopping()
{
    SECTION("IceMuxTcp: stopped with connections in hand");
    IceMuxTcp tcp;
    tcp.keepToLoopback();
    Heard heard;
    heard.attach(tcp);
    CHECK(tcp.start());
    const uint16_t port = freeTcpPorts(1)[0];
    CHECK(tcp.open(1, kUfrag, {port}, 101));
    const SOCKET silent = dial(port);
    const SOCKET named = dial(port);
    const Bytes mine = requestFrame("hostAAAA1234:x");
    CHECK(sendAll(named, mine.data(), mine.size()));
    CHECK(eventually([&heard] { return heard.count() == 1; }, 3000));

    tcp.stop();
    // The one that waited, the one that was kept, and the listener
    CHECK(closedWithin(silent, 1500) && closedWithin(named, 1500));
    CHECK(nobodyListens(port));
    CHECK(!tcp.use(1, heard.at(0).second, kNothing));
    CHECK(!tcp.open(2, "hostBBBB1234", {port}, 102));
    shut(silent);
    shut(named);

    // And started again: as new
    CHECK(tcp.start());
    CHECK(!tcp.expects(1));
    CHECK(tcp.open(1, kUfrag, {port}, 101));
    const SOCKET again = dial(port);
    CHECK(again != INVALID_SOCKET);
    shut(again);
    // Left to the destructor, with an agent that still expects one
}

/// A worker that copies its connections out of the server itself is told a
/// handle's number, and copies later. However late, the handle is still that
/// connection's: nothing lets go of it but that worker's answer, or its end.
void slowWorker()
{
    SECTION("IceMuxTcp: a connection named to a worker is kept until that worker answers");
    IceMuxTcp tcp;
    tcp.keepToLoopback();
    Heard heard;
    heard.attach(tcp);
    CHECK(tcp.start());
    const uint16_t port = freeTcpPorts(1)[0];
    const uint32_t worker = 41, other = 42;
    const Bytes mine = requestFrame("hostAAAA1234:x");
    CHECK(tcp.open(1, kUfrag, {port}, worker));
    const SOCKET peer = dial(port);
    CHECK(sendAll(peer, mine.data(), mine.size()));
    CHECK(eventually([&heard] { return heard.count() == 1; }, 3000));
    const uint32_t number = heard.at(0).second;

    uint64_t handle = 0;
    std::vector<uint8_t> first;
    uint16_t local = 0, remote = 0;
    // For its agent's worker alone, and whole or not at all
    CHECK(!tcp.nameFor(2, number, &handle, &first, &local, &remote));
    CHECK(!tcp.nameFor(1, number, nullptr, &first, &local, &remote));
    CHECK(!tcp.nameFor(1, number, &handle, &first, nullptr, &remote));
    CHECK(tcp.nameFor(1, number, &handle, &first, &local, &remote));
    CHECK(handle != 0 && first.size() == mine.size() - 2);
    CHECK(std::memcmp(first.data(), mine.data() + 2, first.size()) == 0);
    // With the two ports of the connection, as its peer knows them
    sockaddr_in mineName{};
    socklen_type length = sizeof(mineName);
    CHECK(getsockname(peer, reinterpret_cast<sockaddr*>(&mineName), &length) == 0);
    CHECK(local == port && remote == ntohs(mineName.sin_port));

    // Its agent is connected, or gone, and three seconds pass: the worker has
    // not copied it yet, and it is still there under its number
    tcp.close(1);
    CHECK(!readable(peer, 3000));
    CHECK(tcp.use(1, number, kNothing));
    // Another worker's answer, an answer that names nothing, and a server
    // that finds nobody to hand it to, change nothing
    tcp.answered(other, number);
    tcp.answered(worker, 0);
    tcp.letGo(number);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    SOCKET copied = INVALID_SOCKET;
    CHECK(tcp.use(1, number, [&copied](uintptr_t socket, const uint8_t*, size_t) {
        copied = ownEnd(socket);
        return copied != INVALID_SOCKET;
    }));
    // The worker has its copy and says so: the server's end goes, the
    // connection lives
    tcp.answered(worker, number);
    CHECK(!tcp.use(1, number, kNothing));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    char got[16] = {};
    CHECK(sendAll(peer, "ping", 4));
    CHECK(readWithin(copied, got, sizeof(got), 2000) == 4 && std::memcmp(got, "ping", 4) == 0);
    shut(copied);
    shut(peer);

    // A worker that is gone: everything kept for it is closed, named or not,
    // and its agents expect nothing
    CHECK(tcp.open(2, kUfrag, {port}, worker));
    const SOCKET one = dial(port);
    CHECK(sendAll(one, mine.data(), mine.size()));
    CHECK(eventually([&heard] { return heard.count() == 2; }, 3000));
    const SOCKET two = dial(port);
    CHECK(sendAll(two, mine.data(), mine.size()));
    CHECK(eventually([&heard] { return heard.count() == 3; }, 3000));
    CHECK(tcp.nameFor(2, heard.at(1).second, &handle, &first, &local, &remote));
    tcp.release(other);
    CHECK(!readable(one, 200) && tcp.expects(2));
    tcp.release(worker);
    CHECK(closedWithin(one, 1500) && closedWithin(two, 1500));
    CHECK(!tcp.expects(2));
    shut(one);
    shut(two);

    // And one that nobody can be handed, which was not named: closed at once
    CHECK(tcp.open(3, kUfrag, {port}, worker));
    const SOCKET orphan = dial(port);
    CHECK(sendAll(orphan, mine.data(), mine.size()));
    CHECK(eventually([&heard] { return heard.count() == 4; }, 3000));
    tcp.letGo(heard.at(3).second);
    CHECK(closedWithin(orphan, 1500));
    shut(orphan);
    tcp.stop();
}

} // namespace

void run_ice_mux_tcp_tests()
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    acceptedHandleAtBirth();
#endif
    listening();
    handingOver();
    refusing();
    busyPort();
    slowWorker();
    stopping();
}
