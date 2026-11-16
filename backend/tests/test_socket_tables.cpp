/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Whether something on this machine listens on a port, read from the
 * system's own tables of sockets (network/SocketTables) and never tried by a
 * bind. A table that cannot be read, or does not read as one, says nothing.
 */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "test_framework.h"
#include "network/SocketTables.h"

#include <QByteArray>

namespace {

using mw::sockettables::Listens;

#ifdef _WIN32
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
void closeIt(Socket s)
{
    closesocket(s);
}
#else
using Socket = int;
const Socket kNoSocket = -1;
void closeIt(Socket s)
{
    close(s);
}
#endif

/// A socket bound on the loopback to a port the system picks; a TCP one listens.
Socket boundOnLoopback(bool tcp, uint16_t* port)
{
    const Socket s = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (s == kNoSocket) return kNoSocket;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef _WIN32
    int length = sizeof(local);
#else
    socklen_t length = sizeof(local);
#endif
    if (bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&local), &length) != 0 ||
        (tcp && listen(s, 1) != 0)) {
        closeIt(s);
        return kNoSocket;
    }
    *port = ntohs(local.sin_port);
    return s;
}

} // namespace

void run_socket_tables_tests()
{
    using mw::sockettables::inProcTable;

    SECTION("SocketTables: a Linux table, as the kernel writes it");
    const QByteArray udp4 =
        "   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  "
        "timeout inode ref pointer drops\n"
        "  609: 00000000:14E9 00000000:0000 07 00000000:00000000 00:00000000 00000000   108        "
        "0 16471 2 0000000000000000 0\n"
        " 1215: 3500007F:0035 00000000:0000 07 00000000:00000000 00:00000000 00000000   101        "
        "0 18233 2 0000000000000000 0\n"
        " 2408: 0100007F:BDA6 0100007F:BDA7 01 00000000:00000000 00:00000000 00000000  1000        "
        "0 40210 2 0000000000000000 0\n";
    // 0x14E9 = 5353, 0x35 = 53, 0xBDA6 = 48550
    CHECK(inProcTable(udp4, 5353, false) == Listens::Yes);
    CHECK(inProcTable(udp4, 53, false) == Listens::Yes);
    // A UDP socket that is connected to a peer is bound to its port all the same
    CHECK(inProcTable(udp4, 48550, false) == Listens::Yes);
    CHECK(inProcTable(udp4, 48551, false) == Listens::No);
    CHECK(inProcTable(udp4, 0x14E, false) == Listens::No);

    const QByteArray tcp4 = "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
                            "retrnsmt   uid  timeout "
                            "inode\n"
                            "   0: 00000000:0016 00000000:0000 0A 00000000:00000000 00:00000000 "
                            "00000000     0        0 "
                            "23456 1 0000000000000000 100 0 0 10 0\n"
                            "   1: 1401A8C0:BDA6 2201A8C0:D431 01 00000000:00000000 00:00000000 "
                            "00000000  1000        0 "
                            "34567 1 0000000000000000 20 4 30 10 -1\n";
    // A listener on 22; on 48550 a connection that is open, and nobody listening
    CHECK(inProcTable(tcp4, 22, true) == Listens::Yes);
    CHECK(inProcTable(tcp4, 48550, true) == Listens::No);
    CHECK(inProcTable(tcp4, 23, true) == Listens::No);

    const QByteArray udp6 =
        "  sl  local_address                         remote_address                        st "
        "tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n"
        " 2408: 00000000000000000000000000000000:BDA6 00000000000000000000000000000000:0000 07 "
        "00000000:00000000 00:00000000 00000000  1000        0 41000 2 0000000000000000 0\n";
    CHECK(inProcTable(udp6, 48550, false) == Listens::Yes);
    CHECK(inProcTable(udp6, 48551, false) == Listens::No);

    // The heading alone: a table with no socket in it
    CHECK(inProcTable(udp4.left(udp4.indexOf('\n') + 1), 5353, false) == Listens::No);

    SECTION("SocketTables: a text that is no such table says nothing");
    CHECK(inProcTable(QByteArray(), 5353, false) == Listens::Unknown);
    CHECK(inProcTable("Permission denied\n", 5353, false) == Listens::Unknown);
    const QByteArray heading = udp4.left(udp4.indexOf('\n') + 1);
    CHECK(inProcTable(heading + "  609: 00000000:14E9\n", 5353, false) == Listens::Unknown);
    CHECK(inProcTable(heading + "  609: 0000000014E9 00000000:0000 07 x\n", 5353, false) ==
          Listens::Unknown);
    CHECK(inProcTable(heading + "  609: 00000000:ZZZZ 00000000:0000 07 x\n", 5353, false) ==
          Listens::Unknown);
    CHECK(inProcTable(heading + "  609: 00000000:14E9 00000000:0000 zz x\n", 5353, false) ==
          Listens::Unknown);
    CHECK(inProcTable(heading + "  609: 00000000:114E9 00000000:0000 07 x\n", 5353, false) ==
          Listens::Unknown);

    SECTION("SocketTables: this machine's own tables");
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    for (const bool tcp : {false, true}) {
        uint16_t port = 0;
        const Socket s = boundOnLoopback(tcp, &port);
        CHECK(s != kNoSocket);
        if (s == kNoSocket) continue;
        const Listens open = mw::sockettables::listens(port, tcp);
        if (open == Listens::Unknown) {
            // A system whose tables this build does not read: it says so
            std::fprintf(stderr, "  (the %s table is not read on this system)\n",
                         tcp ? "TCP" : "UDP");
            closeIt(s);
            continue;
        }
        CHECK(open == Listens::Yes);
        // The other protocol on that number is another matter
        CHECK(mw::sockettables::listens(port, !tcp) != Listens::Unknown);
        closeIt(s);
        CHECK(mw::sockettables::listens(port, tcp) == Listens::No);
    }
#if defined(_WIN32) || defined(__linux__)
    // Where the tables are read, they are read for both protocols
    CHECK(mw::sockettables::listens(1, false) != Listens::Unknown);
    CHECK(mw::sockettables::listens(1, true) != Listens::Unknown);
#endif

    SECTION("SocketTables: another MoonlightWeb server on this machine");
    {
        using mw::sockettables::otherServerAmong;
        using mw::sockettables::Process;
        using mw::sockettables::serverName;
        // The editions' program names, exactly: in any case under Windows only
        CHECK(serverName("MoonlightWeb.exe", true) && serverName("MoonlightWebDev.exe", true));
        CHECK(serverName("moonlightweb.EXE", true) && serverName("MOONLIGHTWEBDEV.exe", true));
        CHECK(serverName("MoonlightWeb", false) && serverName("MoonlightWebDev", false));
        CHECK(!serverName("moonlightweb", false) && !serverName("MoonlightWeb.exe", false));
        for (const char* other :
             {"", "MoonlightWeb", "MoonlightWebDev", "MoonlightWeb2.exe", "MoonlightWebDevX.exe",
              "moonlightweb-launch", "MoonlightWeb.exe ", "Moonlight.exe", "Sunshine.exe"})
            CHECK(!serverName(other, true));
        CHECK(!serverName("MoonlightWebDevX", false) && !serverName("moonlightweb-launch", false));

        const QList<Process> some{{4, "System"}, {900, "svchost.exe"}, {1200, "MoonlightWeb.exe"}};
        // This process is not another server
        CHECK(otherServerAmong(some, 1200, true, true) == Listens::No);
        CHECK(otherServerAmong(some, 77, true, true) == Listens::Yes);
        CHECK(otherServerAmong({{5, "MoonlightWebDev.exe"}}, 77, true, true) == Listens::Yes);
        CHECK(otherServerAmong({}, 77, true, true) == Listens::No);
        // A list that may lack some says nothing, short of a server it does list
        CHECK(otherServerAmong({{4, "System"}}, 77, true, false) == Listens::Unknown);
        CHECK(otherServerAmong(some, 77, true, false) == Listens::Yes);
        // Elsewhere, the names as the kernel gives them
        CHECK(otherServerAmong({{12, "MoonlightWeb"}}, 77, false, true) == Listens::Yes);
        CHECK(otherServerAmong({{12, "MoonlightWeb.exe"}}, 77, false, true) == Listens::No);
        // This system's own answer is one of the three, and Unknown where
        // nothing is read
        const Listens here = mw::sockettables::otherServer();
        CHECK(here == Listens::No || here == Listens::Yes || here == Listens::Unknown);
#if !defined(_WIN32) && !defined(__linux__)
        CHECK(here == Listens::Unknown);
#endif
    }
}
