/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * What the guard of TCP 443 learns by dialling (network/Tcp443Probe), on this
 * machine's loopback alone: a connection that arrives at this host's own
 * listener, straight or by another port that leads to it; one that reaches
 * something else; one that reaches nothing. And the thread those dials are
 * made on: one at a time, each answered with its number, cut short when it
 * is no longer wanted, and over before what it tells goes.
 */
#include "test_framework.h"
#include "network/IceMuxCore.h"
#include "network/IceMuxTcp.h"
#include "network/Tcp443Probe.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_type = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
using socklen_type = socklen_t;
constexpr int INVALID_SOCKET = -1;
#endif

using mw::tcp443::Dial;
using mw::tcp443::Dialler;
using mw::tcp443::Probe;

namespace {

/// A frame to `address:port`, which must come in on the shared port `shared`
Dial frameTo(const QString& address, uint16_t port, uint16_t shared)
{
    Dial dial;
    dial.kind = Dial::Kind::ArrivesHere;
    dial.address = address;
    dial.port = port;
    dial.leadsHereOn = shared;
    return dial;
}

/// `address:port` dialled with nothing sent
Dial silentTo(const QString& address, uint16_t port)
{
    Dial dial;
    dial.kind = Dial::Kind::Answers;
    dial.address = address;
    dial.port = port;
    return dial;
}

void shut(SOCKET s)
{
    if (s == INVALID_SOCKET) return;
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
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

/// A listener on a loopback port the system chose, and that port
SOCKET listenSomewhere(uint16_t* port)
{
    const SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a = loopback(0);
    socklen_type length = sizeof(a);
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0 ||
        listen(s, 8) != 0 || getsockname(s, reinterpret_cast<sockaddr*>(&a), &length) != 0) {
        shut(s);
        return INVALID_SOCKET;
    }
    *port = ntohs(a.sin_port);
    return s;
}

/// A loopback port nobody listens on right now
uint16_t freePort()
{
    uint16_t port = 0;
    shut(listenSomewhere(&port));
    return port;
}

bool readable(SOCKET s, int ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval wait{ms / 1000, (ms % 1000) * 1000};
    return select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &wait) > 0;
}

/// What stands for a router here: whatever is dialled on its port is led to
/// another port of this machine, as an entry "443 to the block port" does.
/// One way is enough: the listener it leads to never writes.
class Forward
{
public:
    explicit Forward(uint16_t to)
        : m_To(to)
    {
        m_Listener = listenSomewhere(&m_Port);
        m_Thread = std::thread([this] { run(); });
    }
    ~Forward()
    {
        m_Stop = true;
        m_Thread.join();
        shut(m_Listener);
    }
    uint16_t port() const { return m_Port; }

private:
    void run()
    {
        // No listener: nothing to lead anywhere, and nothing to wait on
        while (!m_Stop && m_Listener != INVALID_SOCKET) {
            if (!readable(m_Listener, 50)) continue;
            const SOCKET in = accept(m_Listener, nullptr, nullptr);
            if (in == INVALID_SOCKET) continue;
            const SOCKET out = socket(AF_INET, SOCK_STREAM, 0);
            const sockaddr_in to = loopback(m_To);
            if (out != INVALID_SOCKET &&
                connect(out, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0) {
                // Until whoever dialled is done, or a second went by with nothing
                char bytes[256];
                while (!m_Stop && readable(in, 1000)) {
                    const int got =
                        static_cast<int>(recv(in, bytes, static_cast<int>(sizeof(bytes)), 0));
                    if (got <= 0) break;
                    send(out, bytes, got, 0);
                }
            }
            shut(out);
            shut(in);
        }
    }

    uint16_t m_To;
    uint16_t m_Port = 0;
    SOCKET m_Listener = INVALID_SOCKET;
    std::atomic<bool> m_Stop{false};
    std::thread m_Thread;
};

bool eventually(const std::function<bool()>& what, int ms)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!what()) {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

void theFrame()
{
    SECTION("Tcp443Probe: the frame of a try names its fragment, and nothing else");
    const std::string a = Probe::freshFragment(), b = Probe::freshFragment();
    CHECK_EQ(a.size(), mw::icemuxtcp::kTokenSize);
    CHECK(a != b);
    bool plain = true;
    for (const char c : a + b)
        plain =
            plain && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
    CHECK(plain);

    const std::vector<uint8_t> frame = Probe::knock(a);
    // Its length, then a message of 44 bytes: a header, and one attribute
    CHECK_EQ(frame.size(), size_t(46));
    CHECK(frame[0] == 0 && frame[1] == 44);
    const mw::icemux::StunView view = mw::icemux::readStun(frame.data() + 2, frame.size() - 2);
    CHECK(view.kind == mw::icemux::Kind::BindingRequest);
    CHECK(view.localUfrag == a);
    // Within what a first frame may be, and its transaction is its own
    CHECK(frame.size() - 2 >= mw::icemuxtcp::kFrameMin &&
          frame.size() - 2 <= mw::icemuxtcp::kFrameMax);
    CHECK(Probe::knock(a) != frame);
    CHECK(Probe::knock(a).size() == frame.size());
}

void arriving()
{
    SECTION(
        "Tcp443Probe: a connection that arrives at this host's listener, and one that does not");
    IceMuxTcp tcp;
    tcp.keepToLoopback();
    CHECK(tcp.start());
    const uint16_t shared = freePort();
    const uint16_t nobody = freePort();
    const QString here = QStringLiteral("127.0.0.1");
    Probe probe(tcp);
    probe.setTryMs(700);

    // No shared port named, or no kind of dial: no try at all
    CHECK(!probe.make(frameTo(here, shared, 0)));
    CHECK(!probe.make(Dial{}));
    CHECK_EQ(tcp.counters().accepted, uint64_t(0));

    // Straight to the shared port: what the guard asks of a router's own
    // address before anything else
    CHECK(probe.make(frameTo(here, shared, shared)));
    CHECK_EQ(tcp.counters().tried, uint64_t(1));
    // By another port that leads to it, as 443 does once the router has the
    // entry
    {
        const Forward router(shared);
        CHECK(router.port() != 0 && router.port() != shared);
        CHECK(probe.make(frameTo(here, router.port(), shared)));
        CHECK_EQ(tcp.counters().tried, uint64_t(2));
    }
    // A port where something else answers: the connection is made, the frame
    // goes out, and nothing of it arrives here
    uint16_t elsewhere = 0;
    const SOCKET other = listenSomewhere(&elsewhere);
    CHECK(other != INVALID_SOCKET);
    const auto before = std::chrono::steady_clock::now();
    CHECK(!probe.make(frameTo(here, elsewhere, shared)));
    // ...said only once the try's time is over
    CHECK(std::chrono::steady_clock::now() - before >= std::chrono::milliseconds(600));
    // What that something got: the one frame, which says nothing it can use
    CHECK(readable(other, 1000));
    const SOCKET got = accept(other, nullptr, nullptr);
    uint8_t stray[128] = {};
    const int strayed = got != INVALID_SOCKET && readable(got, 1000)
                            ? static_cast<int>(recv(got, reinterpret_cast<char*>(stray), 128, 0))
                            : -1;
    CHECK_EQ(strayed, 46);
    CHECK(mw::icemux::readStun(stray + 2, 44).kind == mw::icemux::Kind::BindingRequest);
    shut(got);
    // A port nobody answers on, a port that is none, an address that is no
    // IPv4 address in figures
    CHECK(!probe.make(frameTo(here, nobody, shared)));
    CHECK(!probe.make(frameTo(here, 0, shared)));
    CHECK(!probe.make(frameTo(QStringLiteral("localhost"), shared, shared)));
    CHECK(!probe.make(frameTo(QStringLiteral("::1"), shared, shared)));
    CHECK(!probe.make(frameTo(QString(), shared, shared)));
    CHECK_EQ(tcp.counters().tried, uint64_t(2));
    // Cut short before it is made, a dial is not made at all: no try, no
    // connection. Armed again, it is made as any
    probe.cancel();
    CHECK(!probe.make(frameTo(here, shared, shared)));
    CHECK(!probe.make(silentTo(here, shared)));
    CHECK_EQ(tcp.counters().tried, uint64_t(2));
    probe.rearm();
    CHECK(probe.make(frameTo(here, shared, shared)));
    CHECK_EQ(tcp.counters().tried, uint64_t(3));
    // Nothing of a try was handed to anyone, and nothing is left listening
    CHECK_EQ(tcp.counters().handed, uint64_t(0));
    CHECK(eventually(
        [shared] {
            const SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
            const sockaddr_in to = loopback(shared);
            const bool made = connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0;
            shut(s);
            return !made;
        },
        2000));

    // The listener stopped: no try
    tcp.stop();
    CHECK(!probe.make(frameTo(here, shared, shared)));

    SECTION("Tcp443Probe: whether something answers, with nothing sent to it");
    CHECK(probe.make(silentTo(here, elsewhere)));
    // What answered saw a connection come and go, and not a byte
    CHECK(readable(other, 1000));
    const SOCKET seen = accept(other, nullptr, nullptr);
    CHECK(seen != INVALID_SOCKET);
    char byte = 0;
    CHECK(readable(seen, 1500) && recv(seen, &byte, 1, 0) == 0);
    shut(seen);
    shut(other);
    CHECK(!probe.make(silentTo(here, nobody)));
    CHECK(!probe.make(silentTo(here, 0)));
    CHECK(!probe.make(silentTo(QStringLiteral("not an address"), 443)));
}

/// What a dialler told, as the thread the guard lives on would be given it.
class Told
{
public:
    void add(uint32_t run, bool result)
    {
        const std::lock_guard<std::mutex> lock(m_Lock);
        m_Answers.emplace_back(run, result);
        m_From = std::this_thread::get_id();
    }
    size_t count()
    {
        const std::lock_guard<std::mutex> lock(m_Lock);
        return m_Answers.size();
    }
    /// The answer told `at`-th; {0, false} when there is none such
    std::pair<uint32_t, bool> answer(size_t at)
    {
        const std::lock_guard<std::mutex> lock(m_Lock);
        return at < m_Answers.size() ? m_Answers[at] : std::pair<uint32_t, bool>(0, false);
    }
    /// The thread the last one was told on
    std::thread::id from()
    {
        const std::lock_guard<std::mutex> lock(m_Lock);
        return m_From;
    }

private:
    std::mutex m_Lock;
    std::vector<std::pair<uint32_t, bool>> m_Answers;
    std::thread::id m_From;
};

int msSince(std::chrono::steady_clock::time_point began)
{
    using namespace std::chrono;
    return static_cast<int>(duration_cast<milliseconds>(steady_clock::now() - began).count());
}

/// Whether `port` of this machine takes a connection right now
bool takesConnections(uint16_t port)
{
    const SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    const sockaddr_in to = loopback(port);
    const bool made = connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0;
    shut(s);
    return made;
}

/// The connection that waits at `listener`, if one comes within `ms`: taken
/// and closed, so that the next one is seen to come
bool takeOne(SOCKET listener, int ms)
{
    if (!readable(listener, ms)) return false;
    shut(accept(listener, nullptr, nullptr));
    return true;
}

void onItsOwnThread()
{
    SECTION("Tcp443Probe: the dials are made on a thread of their own, one at a time");
    IceMuxTcp tcp;
    tcp.keepToLoopback();
    CHECK(tcp.start());
    const uint16_t shared = freePort();
    const QString here = QStringLiteral("127.0.0.1");
    // Something else answers there: a frame sent to it never arrives here,
    // and the dial would wait out the whole time a try has
    uint16_t elsewhere = 0;
    const SOCKET other = listenSomewhere(&elsewhere);
    CHECK(other != INVALID_SOCKET);
    // The time a try has in the server: two and a half seconds
    Probe probe(tcp);
    const auto told = [](Told& into) {
        return [&into](uint32_t run, bool result) { into.add(run, result); };
    };

    {
        // Never asked for a dial: no thread to end
        Told none;
        const Dialler idle(probe, told(none));
    }
    {
        Told heard;
        Dialler dialler(probe, told(heard));
        // Asked, and answered with the number it was asked under, on a
        // thread that is not the asker's
        CHECK(dialler.ask(7, frameTo(here, shared, shared)));
        CHECK(eventually([&heard] { return heard.count() == 1; }, 3000));
        CHECK(heard.answer(0) == std::make_pair(uint32_t(7), true));
        CHECK(heard.from() != std::this_thread::get_id());
        // No kind of dial: not taken, and nothing is told of it
        CHECK(!dialler.ask(8, Dial{}));

        // One place: while a dial is made, no other is taken
        CHECK(dialler.ask(9, frameTo(here, elsewhere, shared)));
        CHECK(!dialler.ask(10, frameTo(here, shared, shared)));
        // Its frame went elsewhere: it waits, and nothing is told meanwhile
        CHECK(takeOne(other, 2000));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK_EQ(heard.count(), size_t(1));
        // No longer wanted: over well within the time it had, and told no
        const auto cancelled = std::chrono::steady_clock::now();
        dialler.cancel();
        CHECK(eventually([&heard] { return heard.count() == 2; }, 1500));
        CHECK(msSince(cancelled) < 1500);
        CHECK(heard.answer(1) == std::make_pair(uint32_t(9), false));
        // Cut short, its try is over at the listener as any other's: nothing
        // is expected on the shared port, which is no longer listened on
        CHECK(eventually([shared] { return !takesConnections(shared); }, 2000));
        // And the place is free: the next dial is made as any, with a try of
        // its own
        CHECK(dialler.ask(11, frameTo(here, shared, shared)));
        CHECK(eventually([&heard] { return heard.count() == 3; }, 3000));
        CHECK(heard.answer(2) == std::make_pair(uint32_t(11), true));

        // cancel() with the place empty is nothing, and spoils no dial after
        dialler.cancel();
        CHECK(dialler.ask(12, silentTo(here, elsewhere)));
        CHECK(eventually([&heard] { return heard.count() == 4; }, 3000));
        CHECK(heard.answer(3) == std::make_pair(uint32_t(12), true));
        CHECK(takeOne(other, 2000));
        // Nothing of all that was handed to anyone
        CHECK_EQ(tcp.counters().handed, uint64_t(0));

        // Stopped with a dial under way: over well within the time the dial
        // had, and nothing is told of it, then or later
        CHECK(dialler.ask(13, frameTo(here, elsewhere, shared)));
        CHECK(takeOne(other, 2000));
        const auto stopped = std::chrono::steady_clock::now();
        dialler.stop();
        CHECK(msSince(stopped) < 1500);
        CHECK_EQ(heard.count(), size_t(4));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK_EQ(heard.count(), size_t(4));
        CHECK(eventually([shared] { return !takesConnections(shared); }, 2000));
        // No dial is taken after that, and stopping twice is nothing
        CHECK(!dialler.ask(14, frameTo(here, shared, shared)));
        dialler.stop();
        CHECK_EQ(heard.count(), size_t(4));
    }
    {
        // It goes with a dial under way, as when the server stops: joined,
        // and what it told is not called after it is gone
        Told late;
        const auto began = std::chrono::steady_clock::now();
        {
            Dialler dialler(probe, told(late));
            CHECK(dialler.ask(21, frameTo(here, elsewhere, shared)));
            CHECK(takeOne(other, 2000));
        }
        CHECK(msSince(began) < 2000);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK_EQ(late.count(), size_t(0));
    }

#ifdef __linux__
    SECTION("Tcp443Probe: a dial that nothing answers is cut short as well");
    {
        // A listener that takes nobody in: once its queue is full, the next
        // connection gets no answer at all, as from an address that drops
        // what it is sent. That is where a dial waits longest.
        uint16_t deafPort = 0;
        const SOCKET deaf = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a = loopback(0);
        socklen_type length = sizeof(a);
        const bool listening = deaf != INVALID_SOCKET &&
                               bind(deaf, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0 &&
                               listen(deaf, 0) == 0 &&
                               getsockname(deaf, reinterpret_cast<sockaddr*>(&a), &length) == 0;
        CHECK(listening);
        deafPort = ntohs(a.sin_port);
        const auto knock = [deafPort]() {
            const SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
            fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
            const sockaddr_in to = loopback(deafPort);
            connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
            return s;
        };
        std::vector<SOCKET> crowd;
        for (int i = 0; i < 3; ++i)
            crowd.push_back(knock());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Whether this system does leave such a connection unanswered
        const SOCKET one = knock();
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(one, &writable);
        timeval wait{0, 300 * 1000};
        const bool unanswered =
            listening && select(static_cast<int>(one) + 1, nullptr, &writable, nullptr, &wait) == 0;
        shut(one);
        if (!unanswered) {
            std::fprintf(stderr, "  [SKIPPED] this system answers a connection to a listener "
                                 "whose queue is full: the cut of a dial nothing answers is not "
                                 "tried here\n");
        } else {
            Told heard;
            Dialler dialler(probe, told(heard));
            CHECK(dialler.ask(31, silentTo(here, deafPort)));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK_EQ(heard.count(), size_t(0));
            const auto cancelled = std::chrono::steady_clock::now();
            dialler.cancel();
            CHECK(eventually([&heard] { return heard.count() == 1; }, 1500));
            CHECK(msSince(cancelled) < 1500);
            CHECK(heard.answer(0) == std::make_pair(uint32_t(31), false));
        }
        for (const SOCKET s : crowd)
            shut(s);
        shut(deaf);
    }
#endif

    shut(other);
    tcp.stop();
}

} // namespace

void run_tcp443_probe_tests()
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    theFrame();
    arriving();
    onItsOwnThread();
}
