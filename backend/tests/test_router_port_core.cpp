/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The router-port allocation policy, against a router that behaves the way
 * real ones were measured to: it never refuses an entry a neighbour holds, it
 * silently repoints it. Every rule here is a LAN with two MoonlightWeb hosts
 * that stopped working once.
 */
#include "test_framework.h"
#include "network/RouterPortCore.h"
#include "network/RouterPortPools.h"

#include <QMap>
#include <QSet>

namespace {

using Purpose = RouterPortCore::Purpose;
using Reason = RouterPortCore::SkipReason;

/// A router's table, with the knobs the bugs were found under.
struct FakeGateway : IUpnpGateway
{
    struct Entry
    {
        QString client;
        uint16_t internal = 0;
    };

    QString lan = QStringLiteral("192.168.1.20");
    /// (external, protocol) → entry
    QMap<QPair<uint16_t, QString>, Entry> table;
    /// A Livebox: AddPortMapping on an entry somebody else holds succeeds and
    /// the table keeps pointing at them.
    bool keepsForeignEntries = false;
    /// An IGD that does not implement GetSpecificPortMappingEntry.
    bool mute = false;
    /// An IGD v1 that rejects every non-zero lease.
    bool refuseLease = false;
    /// Ports the router refuses outright, both leases.
    QSet<uint16_t> refuse;
    int adds = 0;
    int removes = 0;

    void foreign(uint16_t port, const QString& client)
    {
        table.insert({port, QStringLiteral("UDP")}, {client, port});
    }

    QString lanAddress() const override { return lan; }

    bool add(uint16_t ext, uint16_t in, uint32_t leaseSec, const QString&,
             const QString& proto) override
    {
        ++adds;
        if (refuse.contains(ext)) return false;
        if (refuseLease && leaseSec > 0) return false;
        const auto key = qMakePair(ext, proto);
        if (keepsForeignEntries && table.contains(key) && table[key].client != lan) return true;
        table[key] = {lan, in};
        return true;
    }

    bool owner(uint16_t ext, const QString& proto, QString& client) override
    {
        if (mute) return false;
        const auto key = qMakePair(ext, proto);
        if (!table.contains(key)) return false;
        client = table[key].client;
        return true;
    }

    bool remove(uint16_t ext, const QString& proto) override
    {
        ++removes;
        return table.remove(qMakePair(ext, proto)) > 0;
    }
};

RouterPortCore::Request tunnelRequest(const QList<uint16_t>& remembered = {})
{
    RouterPortCore::Request r;
    r.purpose = Purpose::Tunnel;
    r.remembered = remembered;
    for (const uint16_t p : mw::routerports::kTunnelPreferred)
        r.preferred.append(p);
    r.poolBegin = mw::routerports::kTunnelPoolBegin;
    r.poolEnd = mw::routerports::kTunnelPoolEnd;
    r.description = QStringLiteral("MoonlightWeb tunnel");
    return r;
}

RouterPortCore::Request mediaRequest(int slot, uint16_t remembered = 0)
{
    RouterPortCore::Request r;
    r.purpose = Purpose::Media;
    r.slot = slot;
    r.internalPort = static_cast<uint16_t>(mw::routerports::kMediaBasePort + slot);
    if (remembered) r.remembered.append(remembered);
    r.preferred.append(r.internalPort);
    r.poolBegin = mw::routerports::kMediaPoolBegin;
    r.poolEnd = mw::routerports::kMediaPoolEnd;
    r.description = QStringLiteral("MoonlightWeb media slot %1").arg(slot);
    return r;
}

const auto kAlwaysFree = [](uint16_t) { return true; };

} // namespace

void run_router_port_core_tests()
{
    SECTION("RouterPortCore");

    // ── A lone host looks exactly as it always did ────────────────────────
    {
        FakeGateway gw;
        RouterPortCore core(gw, kAlwaysFree);

        const auto r = core.claim(tunnelRequest());
        CHECK(r.ok);
        CHECK_EQ(r.claim.external, uint16_t(3478));
        CHECK_EQ(r.claim.internal, uint16_t(3478));
        CHECK(r.claim.tcp);
        CHECK(r.skipped.isEmpty());
        CHECK(gw.table.contains(qMakePair(uint16_t(3478), QStringLiteral("UDP"))));
        CHECK(gw.table.contains(qMakePair(uint16_t(3478), QStringLiteral("TCP"))));

        // The next claim walks on: a port this host holds is never offered twice.
        const auto r2 = core.claim(tunnelRequest());
        CHECK(r2.ok);
        CHECK_EQ(r2.claim.external, uint16_t(3479));
        CHECK_EQ(core.held().size(), 2);

        const auto m = core.claim(mediaRequest(0));
        CHECK(m.ok);
        CHECK_EQ(m.claim.external, uint16_t(48550));
        CHECK_EQ(m.claim.internal, uint16_t(48550));
        CHECK_EQ(m.claim.slot, 0);
    }

    // ── A neighbour holds the front of the list: walk past it, say so ─────
    {
        FakeGateway gw;
        for (uint16_t p = 3478; p <= 3481; ++p)
            gw.foreign(p, QStringLiteral("192.168.1.34"));
        RouterPortCore core(gw, kAlwaysFree);

        const auto r = core.claim(tunnelRequest());
        CHECK(r.ok);
        CHECK_EQ(r.claim.external, uint16_t(5349));
        CHECK_EQ(r.skipped.size(), 4);
        CHECK(r.skipped.first().reason == Reason::Foreign);
        CHECK_EQ(r.skipped.first().owner, QString("192.168.1.34"));
        // The neighbour's entries were not touched.
        CHECK_EQ(gw.table[qMakePair(uint16_t(3478), QStringLiteral("UDP"))].client,
                 QString("192.168.1.34"));
    }

    // ── Every corporate port taken: the pool, one port at a time ──────────
    {
        FakeGateway gw;
        for (const uint16_t p : mw::routerports::kTunnelPreferred)
            gw.foreign(p, QStringLiteral("192.168.1.34"));
        gw.foreign(46000, QStringLiteral("192.168.1.35"));
        RouterPortCore core(gw, kAlwaysFree);

        const auto r = core.claim(tunnelRequest());
        CHECK(r.ok);
        CHECK_EQ(r.claim.external, uint16_t(46001));
        CHECK_EQ(r.skipped.size(), 9);
    }

    // ── Nothing left at all ───────────────────────────────────────────────
    {
        FakeGateway gw;
        for (const uint16_t p : mw::routerports::kTunnelPreferred)
            gw.foreign(p, QStringLiteral("192.168.1.34"));
        for (int p = mw::routerports::kTunnelPoolBegin; p <= mw::routerports::kTunnelPoolEnd; ++p)
            gw.foreign(static_cast<uint16_t>(p), QStringLiteral("192.168.1.34"));
        RouterPortCore core(gw, kAlwaysFree);

        const auto r = core.claim(tunnelRequest());
        CHECK(!r.ok);
        CHECK_EQ(r.claim.external, uint16_t(0));
        CHECK_EQ(r.skipped.size(), 8 + 32);
        CHECK(core.held().isEmpty());
    }

    // ── The Livebox: "added" yet the table still points at the neighbour ──
    {
        // The pre-write check is what normally catches this; make the router
        // lie only AFTER the write by hiding the entry from the first look.
        struct LateLiar : FakeGateway
        {
            int looks = 0;
            bool owner(uint16_t ext, const QString& proto, QString& client) override
            {
                // First look at 3478 says "nobody"; the read-back tells the truth.
                if (ext == 3478 && looks++ == 0) return false;
                return FakeGateway::owner(ext, proto, client);
            }
        } liar;
        liar.keepsForeignEntries = true;
        liar.foreign(3478, QStringLiteral("192.168.1.34"));
        RouterPortCore core(liar, kAlwaysFree);

        const auto r = core.claim(tunnelRequest());
        CHECK(r.ok);
        CHECK_EQ(r.claim.external, uint16_t(3479));
        CHECK_EQ(r.skipped.size(), 1);
        CHECK(r.skipped.first().reason == Reason::KeptElsewhere);
        CHECK_EQ(r.skipped.first().owner, QString("192.168.1.34"));
        // And the hole that leads elsewhere is not held.
        CHECK(!core.holds(3478));
    }

    // ── Remembered ports come first — that is what makes the block sticky ─
    {
        FakeGateway gw;
        RouterPortCore core(gw, kAlwaysFree);

        const auto r = core.claim(tunnelRequest({5350}));
        CHECK(r.ok);
        CHECK_EQ(r.claim.external, uint16_t(5350));
        CHECK(r.skipped.isEmpty());

        // Media: the remembered pool port beats the slot's own number.
        const auto m = core.claim(mediaRequest(0, 46107));
        CHECK(m.ok);
        CHECK_EQ(m.claim.external, uint16_t(46107));
        CHECK_EQ(m.claim.internal, uint16_t(48550));
    }

    // ── A remembered port a neighbour took meanwhile is passed over ───────
    {
        FakeGateway gw;
        gw.foreign(3478, QStringLiteral("192.168.1.34"));
        RouterPortCore core(gw, kAlwaysFree);

        const auto r = core.claim(tunnelRequest({3478}));
        CHECK(r.ok);
        CHECK_EQ(r.claim.external, uint16_t(3479));
        CHECK_EQ(r.skipped.size(), 1);
        CHECK_EQ(r.skipped.first().port, uint16_t(3478));
        CHECK(r.skipped.first().reason == Reason::Foreign);
    }

    // ── Our own stale entry after a DHCP renumbering is taken back ────────
    {
        FakeGateway gw;
        gw.foreign(3478, QStringLiteral("192.168.1.77")); // what we were
        RouterPortCore core(gw, kAlwaysFree);
        core.setPreviousLanAddress(QStringLiteral("192.168.1.77"));

        const auto r = core.claim(tunnelRequest({3478}));
        CHECK(r.ok);
        CHECK_EQ(r.claim.external, uint16_t(3478));
        CHECK_EQ(r.reclaimedFrom, QString("192.168.1.77"));
        CHECK_EQ(gw.table[qMakePair(uint16_t(3478), QStringLiteral("UDP"))].client,
                 QString("192.168.1.20"));
    }

    // ── A port another program holds on this machine is skipped ──────────
    {
        FakeGateway gw;
        RouterPortCore core(gw, [](uint16_t p) { return p != 3478; });

        const auto r = core.claim(tunnelRequest());
        CHECK(r.ok);
        CHECK_EQ(r.claim.external, uint16_t(3479));
        CHECK_EQ(r.skipped.size(), 1);
        CHECK(r.skipped.first().reason == Reason::LocalBusy);
        // The router was never asked about it: no entry to a closed port.
        CHECK(!gw.table.contains(qMakePair(uint16_t(3478), QStringLiteral("UDP"))));

        // Media never probes: the stream's own bind is its own diagnostic, and
        // the parent process cannot bind the worker's port on its behalf.
        RouterPortCore mediaCore(gw, [](uint16_t) { return false; });
        const auto m = mediaCore.claim(mediaRequest(0));
        CHECK(m.ok);
        CHECK_EQ(m.claim.external, uint16_t(48550));
    }

    // ── The media external port walks when a neighbour has the slot's own ─
    {
        FakeGateway gw;
        gw.foreign(48550, QStringLiteral("192.168.1.34"));
        RouterPortCore core(gw, kAlwaysFree);

        const auto m = core.claim(mediaRequest(0));
        CHECK(m.ok);
        CHECK_EQ(m.claim.external, uint16_t(46100));
        CHECK_EQ(m.claim.internal, uint16_t(48550));
        CHECK_EQ(gw.table[qMakePair(uint16_t(46100), QStringLiteral("UDP"))].internal,
                 uint16_t(48550));
        // Slot 1 on the same host: its own number is free, so it keeps it.
        const auto m1 = core.claim(mediaRequest(1));
        CHECK(m1.ok);
        CHECK_EQ(m1.claim.external, uint16_t(48551));
    }

    // ── A mute IGD is "nobody", not "not ours" ────────────────────────────
    {
        FakeGateway gw;
        gw.mute = true;
        gw.foreign(3478, QStringLiteral("192.168.1.34")); // invisible to us
        RouterPortCore core(gw, kAlwaysFree);

        const auto r = core.claim(tunnelRequest());
        CHECK(r.ok);
        CHECK_EQ(r.claim.external, uint16_t(3478));
        // And renewal on a mute IGD loses nothing.
        CHECK(core.renew().isEmpty());
        CHECK_EQ(core.held().size(), 1);
    }

    // ── A router that refuses leases is latched to permanent, once ────────
    {
        FakeGateway gw;
        gw.refuseLease = true;
        RouterPortCore core(gw, kAlwaysFree);

        CHECK_EQ(core.leaseSec(), mw::routerports::kLeaseSec);
        const auto r = core.claim(tunnelRequest());
        CHECK(r.ok);
        CHECK_EQ(core.leaseSec(), uint32_t(0));
        // UDP: leased then permanent; TCP: permanent straight away.
        CHECK_EQ(gw.adds, 3);
    }

    // ── A port the router refuses outright is just another skip ──────────
    {
        FakeGateway gw;
        gw.refuse.insert(3478);
        RouterPortCore core(gw, kAlwaysFree);

        const auto r = core.claim(tunnelRequest());
        CHECK(r.ok);
        CHECK_EQ(r.claim.external, uint16_t(3479));
        CHECK(r.skipped.first().reason == Reason::Refused);
        // A refusal is not a lease problem: the lease stays.
        CHECK_EQ(core.leaseSec(), mw::routerports::kLeaseSec);
    }

    // ── Renewal: a theft found at the half hour is dropped and reported ───
    {
        FakeGateway gw;
        gw.keepsForeignEntries = true;
        RouterPortCore core(gw, kAlwaysFree);
        core.claim(tunnelRequest());
        core.claim(tunnelRequest());
        CHECK_EQ(core.held().size(), 2);

        // The second host on the LAN came up and the router repointed 3478.
        gw.table[qMakePair(uint16_t(3478), QStringLiteral("UDP"))].client =
            QStringLiteral("192.168.1.34");
        const auto lost = core.renew();
        CHECK_EQ(lost.size(), 1);
        CHECK_EQ(lost.first().claim.external, uint16_t(3478));
        CHECK_EQ(lost.first().owner, QString("192.168.1.34"));
        CHECK_EQ(core.held().size(), 1);
        CHECK(core.holds(3479));
        CHECK(!core.holds(3478));
        CHECK_EQ(core.renewFailures(), 0);
    }

    // ── Renewal failing twice switches to permanent mappings ─────────────
    {
        FakeGateway gw;
        RouterPortCore core(gw, kAlwaysFree);
        core.claim(tunnelRequest());
        CHECK_EQ(core.leaseSec(), mw::routerports::kLeaseSec);

        gw.refuse.insert(3478); // the router stops answering the renewal
        CHECK(core.renew().isEmpty());
        CHECK_EQ(core.renewFailures(), 1);
        CHECK_EQ(core.leaseSec(), mw::routerports::kLeaseSec);
        CHECK(core.renew().isEmpty());
        CHECK_EQ(core.renewFailures(), 2);
        CHECK_EQ(core.leaseSec(), uint32_t(0));
        // The hole is still held: the latch is a bet on the next lease, not a
        // verdict on this mapping.
        CHECK(core.holds(3478));
    }

    // ── Release removes what was mapped, and only that ────────────────────
    {
        FakeGateway gw;
        RouterPortCore core(gw, kAlwaysFree);
        core.claim(tunnelRequest());
        core.claim(mediaRequest(0));
        gw.foreign(5349, QStringLiteral("192.168.1.34"));
        gw.removes = 0;

        core.releaseAll();
        CHECK(core.held().isEmpty());
        CHECK_EQ(gw.removes, 4); // two holes, UDP + TCP each
        CHECK(!gw.table.contains(qMakePair(uint16_t(3478), QStringLiteral("UDP"))));
        CHECK(!gw.table.contains(qMakePair(uint16_t(48550), QStringLiteral("TCP"))));
        // The neighbour's entry was never ours to remove.
        CHECK(gw.table.contains(qMakePair(uint16_t(5349), QStringLiteral("UDP"))));
    }

    // ── The pools stay clear of everything else this project binds ────────
    {
        using namespace mw::routerports;
        CHECK(kTunnelPoolEnd < kMediaPoolBegin);
        CHECK(kMediaPoolEnd < 47984); // GameStream
        // The media block sits above MultiSeat and Wolf (up to ~48340), below a
        // GameStream server moved up by a thousand (48984) and the Windows and
        // macOS ephemeral range (49152).
        CHECK(kMediaBasePort > 48399);
        CHECK(kMediaBasePort + kMediaPortCount - 1 < 48984);
        CHECK(kTunnelPreferred[0] == 3478); // the corporate port first
        CHECK(kTunnelPortCap >= 1);
    }
}
