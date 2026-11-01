/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * A Public browser on the host's own network (NetClassify::sharesHostNetwork).
 *
 * The case it was written for, 24/10/2026: a Mac on the same Wi-Fi came in
 * through the public link, the tunnel settled on a global address, and the
 * session was planned with the public candidates only. Such a peer arrives from
 * our own public IPv4 or from inside one of our IPv6 /64 prefixes; anything
 * else stays a stranger.
 */

#include "../src/server/NetClassify.h"

#include "test_framework.h"

namespace {

const QStringList kOwnPublic{QStringLiteral("198.51.100.10")};

QList<QHostAddress> ownAddresses()
{
    return {QHostAddress(QStringLiteral("192.168.1.66")),
            QHostAddress(QStringLiteral("2001:db8:abc:de50:aaaa:bbbb:cccc:0001")),
            QHostAddress(QStringLiteral("fd7a:115c:a1e0::c036:292d")),
            QHostAddress(QStringLiteral("fe80::1"))};
}

void testOwnPublicIpv4IsOurNetwork()
{
    CHECK(NetClassify::sharesHostNetwork(QStringLiteral("198.51.100.10"), kOwnPublic,
                                         ownAddresses()));
    // As a dual-stack socket reports it.
    CHECK(NetClassify::sharesHostNetwork(QStringLiteral("::ffff:198.51.100.10"), kOwnPublic,
                                         ownAddresses()));
    // Either source of the public address will do; an empty one matches nothing.
    CHECK(NetClassify::sharesHostNetwork(QStringLiteral("198.51.100.10"),
                                         {QString(), QStringLiteral("198.51.100.10")}, {}));
    CHECK(!NetClassify::sharesHostNetwork(QStringLiteral("198.51.100.10"), {QString()}, {}));
}

void testAnotherPublicIpv4IsNot()
{
    CHECK(
        !NetClassify::sharesHostNetwork(QStringLiteral("203.0.113.9"), kOwnPublic, ownAddresses()));
}

void testSameIpv6PrefixIsOurNetwork()
{
    // A temporary address, in the host's /64.
    CHECK(NetClassify::sharesHostNetwork(QStringLiteral("2001:db8:abc:de50:aaaa:bbbb:cccc:0002"),
                                         kOwnPublic, ownAddresses()));
    // Same /56, another /64: another link, maybe another subscriber.
    CHECK(!NetClassify::sharesHostNetwork(QStringLiteral("2001:db8:abc:de51::1"), kOwnPublic,
                                          ownAddresses()));
}

void testNonGlobalAddressesNeverMatch()
{
    // A ULA or link-local peer is already trusted by classify(); this verdict
    // is only for Public peers, and a ULA prefix of ours proves nothing.
    CHECK(!NetClassify::sharesHostNetwork(QStringLiteral("fd7a:115c:a1e0::1"), kOwnPublic,
                                          ownAddresses()));
    CHECK(!NetClassify::sharesHostNetwork(QStringLiteral("fe80::2"), kOwnPublic, ownAddresses()));
    CHECK(!NetClassify::sharesHostNetwork(QStringLiteral("192.168.1.83"), kOwnPublic,
                                          ownAddresses()));
    CHECK(!NetClassify::sharesHostNetwork(QStringLiteral("127.0.0.1"), kOwnPublic, ownAddresses()));
}

void testGarbageIsNot()
{
    CHECK(!NetClassify::sharesHostNetwork(QString(), kOwnPublic, ownAddresses()));
    CHECK(!NetClassify::sharesHostNetwork(QStringLiteral("tunnel-ab12cd34"), kOwnPublic,
                                          ownAddresses()));
}

} // namespace

void run_net_classify_tests()
{
    testOwnPublicIpv4IsOurNetwork();
    testAnotherPublicIpv4IsNot();
    testSameIpv6PrefixIsOurNetwork();
    testNonGlobalAddressesNeverMatch();
    testGarbageIsNot();
}
