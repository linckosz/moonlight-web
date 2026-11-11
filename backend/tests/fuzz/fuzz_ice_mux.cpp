/*
 * MoonlightWeb — fuzz target. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * What the server process reads of every datagram that reaches a UDP port its
 * streams share (network/IceMuxCore): the STUN header and the USERNAME of a
 * Binding request, and the routing table those feed. Whatever the bytes and
 * whoever sends them, nothing is read outside the datagram, and the table
 * stays within its bounds.
 *
 * libFuzzer, built by no CMake file of the repository. With clang:
 *   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined \
 *           -I<backend>/src fuzz_ice_mux.cpp <backend>/src/network/IceMuxCore.cpp \
 *           -o fuzz_ice_mux
 *   ./fuzz_ice_mux -max_len=256 -max_total_time=60
 */
#include "network/IceMuxCore.h"

#include <cstdlib>
#include <cstring>

using namespace mw::icemux;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // The reader alone, on the bytes as they are
    const StunView view = readStun(data, size);
    if (view.kind == Kind::BindingRequest && !view.localUfrag.empty()) {
        const auto* begin = reinterpret_cast<const uint8_t*>(view.localUfrag.data());
        if (begin < data || begin + view.localUfrag.size() > data + size) std::abort();
    }
    if (size < 4) return 0;

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
            if (who & 0x80) router->confirm(feed, from);
        }
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
