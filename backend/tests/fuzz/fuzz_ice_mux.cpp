/*
 * MoonlightWeb — fuzz target. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * What the server process reads of every datagram that reaches a UDP port its
 * streams share (network/IceMuxCore): the STUN header and the USERNAME of a
 * Binding request, and the routing table those feed. Whatever the bytes and
 * whoever sends them, nothing is read outside the datagram, and the table
 * stays within its bounds. And what it reads of a TCP connection to one of
 * those ports (network/IceMuxTcpCore): its first frame, and the table of the
 * connections that wait or were handed to a worker.
 *
 * libFuzzer, built by no CMake file of the repository. With clang:
 *   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined \
 *           -I<backend>/src fuzz_ice_mux.cpp <backend>/src/network/IceMuxCore.cpp \
 *           <backend>/src/network/IceMuxTcpCore.cpp -o fuzz_ice_mux
 *   ./fuzz_ice_mux -max_len=256 -max_total_time=60
 */
#include "network/IceMuxCore.h"
#include "network/IceMuxTcpCore.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>

using namespace mw::icemux;

namespace {

/// The same input as what TCP connections send, cut the same way: the first
/// byte of each piece says what happens, the rest is what a connection sends.
/// Whatever comes, each socket the table is given is let go of once, it never
/// holds more than it has places for, and it asks for no byte beyond a frame.
void tcpTable(const uint8_t* data, size_t size)
{
    using namespace mw::icemuxtcp;
    static Table* table = nullptr;
    static std::set<uintptr_t>* held = nullptr;
    static uintptr_t next = 1;
    static uint64_t now = 0;
    static uint32_t last = 0;
    static const uint16_t ports[] = {3478, 48550};
    static const char* const names[] = {"hostAAAA1234", "hostBBBB5678"};
    if (!table) {
        table = new Table;
        held = new std::set<uintptr_t>;
    }
    Closed closed;
    size_t pos = 0;
    while (size - pos >= 2) {
        const uint8_t what = data[pos];
        const size_t len = std::min<size_t>(data[pos + 1], size - pos - 2);
        pos += 2;
        now += what;
        const uint32_t feed = 1 + ((what >> 3) & 1);
        switch (what & 7) {
        case 0: table->open(feed, names[feed - 1], ports, 1 + ((what >> 4) & 1), now, feed); break;
        case 1:
            // An agent's end, or its worker's
            if (what & 0x40)
                table->releaseOwner(feed, closed);
            else
                table->close(feed, closed);
            break;
        case 2: table->expire(now, closed); break;
        case 3:
            // A worker's answer, for the last one handed or for none by name;
            // or a server that finds nobody to hand it to
            if (what & 0x80)
                table->letGo(last, closed);
            else
                table->answered(feed, (what & 0x40) ? last : 0, closed);
            break;
        default: {
            const uintptr_t socket = next++;
            held->insert(socket);
            const uint8_t address[4] = {192, 0, 2, static_cast<uint8_t>(what >> 5)};
            const int place =
                table->accept(socket, sourceOf(4, address), ports[(what >> 4) & 1], now, closed);
            if (place < 0) break;
            size_t at = 0;
            Table::Step step = Table::Step::More;
            while (at < len && step == Table::Step::More) {
                const Table::Need need = table->need(place);
                if (!need.into || need.count == 0 || need.count > 2 + kFrameMax) std::abort();
                const size_t n = std::min(need.count, len - at);
                std::memcpy(need.into, data + pos + at, n);
                at += n;
                step = table->got(place, n);
            }
            if (step == Table::Step::Close)
                table->refuse(place, closed);
            else if (step == Table::Step::Whole) {
                // Handed, and now and then named to its worker
                const Table::Handed handed = table->route(place, now, closed);
                if (handed.feed != 0) last = handed.number;
                if (handed.feed != 0 && (what & 0x40)) table->name(last);
            } else if (what & 0x80)
                table->drop(place, closed);
            break;
        }
        }
        for (const Gone& gone : closed)
            if (held->erase(gone.socket) != 1) std::abort();
        closed.clear();
        if (held->size() > kPlaces + kHandedMax) std::abort();
        pos += len;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // The reader alone, on the bytes as they are
    const StunView view = readStun(data, size);
    if (view.kind == Kind::BindingRequest && !view.localUfrag.empty()) {
        const auto* begin = reinterpret_cast<const uint8_t*>(view.localUfrag.data());
        if (begin < data || begin + view.localUfrag.size() > data + size) std::abort();
    }
    if (size < 4) return 0;
    tcpTable(data, size);

    // The table: two agents, and the input cut into datagrams from senders it
    // names itself. The first byte of each piece picks the sender and the
    // clock's step, the second its length.
    static Router* router = nullptr;
    static uint64_t now = 0;
    if (!router) {
        router = new Router;
        router->addAgent(1, "hostAAAA1234", 0);
        router->addAgent(2, "hostBBBB5678", 0);
    }
    size_t pos = 0;
    while (size - pos >= 2) {
        const uint8_t who = data[pos];
        const size_t len = std::min<size_t>(data[pos + 1], size - pos - 2);
        pos += 2;
        Peer from;
        from.family = (who & 1) ? 6 : 4;
        from.address[0] = static_cast<uint8_t>(who >> 2);
        from.port = static_cast<uint16_t>(1000 + who);
        from.via = static_cast<uint8_t>((who >> 1) & 1);
        now += who;
        const Route route = router->route(from, data + pos, len, now);
        if (route.feeds.size() > 2) std::abort();
        if (route.feeds.size() == 2 && route.feeds[0] == route.feeds[1]) std::abort();
        for (uint32_t feed : route.feeds) {
            if (feed != 1 && feed != 2) std::abort();
            if (who & 0x80) router->confirm(feed, from, now);
        }
        // Now and then the socket it came in on goes: what was learned
        // through it goes too, and the shares below still add up
        if ((who & 0x7C) == 0x7C) router->forgetVia(from.via);
        if (router->provisionalCount(1) > Router::kProvisionalPerFeedMax ||
            router->provisionalCount(2) > Router::kProvisionalPerFeedMax ||
            router->provisionalCount(1) + router->provisionalCount(2) <
                router->provisionalCount() ||
            router->provisionalCount() < router->provisionalCount(1) ||
            router->provisionalCount() < router->provisionalCount(2) ||
            router->confirmedCount() > 2 * Router::kConfirmedPerFeedMax)
            std::abort();
        pos += len;
    }
    return 0;
}
