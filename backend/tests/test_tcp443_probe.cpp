/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * What the guard of TCP 443 learns by dialling (network/Tcp443Probe), on this
 * machine's loopback alone: a connection that arrives at this host's own
 * listener, straight or by another port that leads to it; one that reaches
 * something else; one that reaches nothing.
 */
#include "test_framework.h"
#include "network/IceMuxCore.h"
#include "network/IceMuxTcp.h"
#include "network/Tcp443Probe.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_type = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
using socklen_type = socklen_t;
constexpr int INVALID_SOCKET = -1;
#endif

using mw::tcp443::Probe;

namespace {

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

    // No shared port named: no try at all
    CHECK(!probe.arrivesHere(here, shared));
    CHECK_EQ(tcp.counters().accepted, uint64_t(0));
    probe.leadsHereOn(shared);

    // Straight to the shared port: what the guard asks of a router's own
    // address before anything else
    CHECK(probe.arrivesHere(here, shared));
    CHECK_EQ(tcp.counters().tried, uint64_t(1));
    // By another port that leads to it, as 443 does once the router has the
    // entry
    {
        const Forward router(shared);
        CHECK(router.port() != 0 && router.port() != shared);
        CHECK(probe.arrivesHere(here, router.port()));
        CHECK_EQ(tcp.counters().tried, uint64_t(2));
    }
    // A port where something else answers: the connection is made, the frame
    // goes out, and nothing of it arrives here
    uint16_t elsewhere = 0;
    const SOCKET other = listenSomewhere(&elsewhere);
    CHECK(other != INVALID_SOCKET);
    const auto before = std::chrono::steady_clock::now();
    CHECK(!probe.arrivesHere(here, elsewhere));
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
    CHECK(!probe.arrivesHere(here, nobody));
    CHECK(!probe.arrivesHere(here, 0));
    CHECK(!probe.arrivesHere(QStringLiteral("localhost"), shared));
    CHECK(!probe.arrivesHere(QStringLiteral("::1"), shared));
    CHECK(!probe.arrivesHere(QString(), shared));
    CHECK_EQ(tcp.counters().tried, uint64_t(2));
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
    CHECK(!probe.arrivesHere(here, shared));

    SECTION("Tcp443Probe: whether something answers, with nothing sent to it");
    CHECK(probe.answers(here, elsewhere));
    // What answered saw a connection come and go, and not a byte
    CHECK(readable(other, 1000));
    const SOCKET seen = accept(other, nullptr, nullptr);
    CHECK(seen != INVALID_SOCKET);
    char byte = 0;
    CHECK(readable(seen, 1500) && recv(seen, &byte, 1, 0) == 0);
    shut(seen);
    shut(other);
    CHECK(!probe.answers(here, nobody));
    CHECK(!probe.answers(here, 0));
    CHECK(!probe.answers(QStringLiteral("not an address"), 443));
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
}
