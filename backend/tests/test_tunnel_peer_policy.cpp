/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * Which browser tunnels outlive a `close` from the introduction server
 * (TunnelPeerPolicy.h, plan « Coupure fallback » H1-H5). What matters: only an
 * open channel is kept, and only for a browser on the LAN or one a request has
 * vouched for; the ceiling lets the unvouched go first, oldest first.
 */

#include "../src/server/TunnelPeerPolicy.h"

#include "test_framework.h"

#include <vector>

void run_tunnel_peer_policy_tests()
{
    using namespace TunnelPeerPolicy;

    SECTION("TunnelPeerPolicy — a close keeps an open channel on the LAN or a known device");
    {
        CHECK(keepOnClose(true, true, false));
        CHECK(keepOnClose(true, false, true));
        CHECK(keepOnClose(true, true, true));
        // A stranger on the internet: taken down as before.
        CHECK(!keepOnClose(true, false, false));
        // A handshake still under way has nothing to keep.
        CHECK(!keepOnClose(false, true, true));
        CHECK(!keepOnClose(false, false, false));
    }

    SECTION("TunnelPeerPolicy — nothing goes while within the ceiling");
    {
        std::vector<Detached> d;
        CHECK(evictOne(d).isEmpty());
        for (int i = 0; i < kMaxDetached; ++i)
            d.push_back({QStringLiteral("s%1").arg(i), static_cast<quint64>(i + 1), false});
        CHECK(evictOne(d).isEmpty());
    }

    SECTION("TunnelPeerPolicy — past it, the oldest unvouched one goes first");
    {
        std::vector<Detached> d;
        for (int i = 0; i < kMaxDetached; ++i)
            d.push_back({QStringLiteral("s%1").arg(i), static_cast<quint64>(i + 1), true});
        // One over: every older one vouched for, the newest not.
        d.push_back({QStringLiteral("late"), 100, false});
        CHECK(evictOne(d) == QStringLiteral("late"));

        d[3].known = false; // s3 is unvouched and older than "late"
        CHECK(evictOne(d) == QStringLiteral("s3"));
    }

    SECTION("TunnelPeerPolicy — all vouched for: the oldest goes");
    {
        std::vector<Detached> d;
        for (int i = 0; i <= kMaxDetached; ++i)
            d.push_back({QStringLiteral("s%1").arg(i), static_cast<quint64>(50 - i), true});
        CHECK(evictOne(d) == QStringLiteral("s%1").arg(kMaxDetached));
    }

    SECTION("TunnelPeerPolicy — the deadline outlasts the bootstrap's own");
    {
        // bootstrap/v1/tunnel.js CONNECT_TIMEOUT_MS: a browser still trying is
        // never cut short by the host.
        CHECK(kHandshakeDeadlineMs > 25000);
    }

    SECTION("TunnelPeerPolicy — the prototype's bench key takes an IPv4 address and a port");
    {
        QString ip;
        quint16 port = 0;
        CHECK(parsePublicAddress(QStringLiteral("82.67.150.202:443"), &ip, &port));
        CHECK(ip == QStringLiteral("82.67.150.202"));
        CHECK_EQ(static_cast<int>(port), 443);
        CHECK(parsePublicAddress(QStringLiteral(" 10.0.0.1:65535 "), &ip, &port));
        CHECK_EQ(static_cast<int>(port), 65535);

        // Anything else leaves the key off, and nothing is written: a mistyped
        // value must never pin a port by accident.
        ip = QStringLiteral("kept");
        port = 7;
        for (const char* bad :
             {"", "443", ":443", "82.67.150.202", "82.67.150.202:", "82.67.150.202:0",
              "82.67.150.202:65536", "82.67.150:443", "82.67.150.256:443", "82.67.150.202.1:443",
              "a.b.c.d:443", "[::1]:443", "82.67.150.202:44x", "82.67.150.0202:443"}) {
            CHECK(!parsePublicAddress(QString::fromLatin1(bad), &ip, &port));
        }
        CHECK(ip == QStringLiteral("kept"));
        CHECK_EQ(static_cast<int>(port), 7);
    }
}
