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

#include <iterator>

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
        /// Seconds left; 0 = permanent.
        uint32_t lease = 0;
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
    /// MiniUPnPd up to 1.9 (TP-Link Deco X50, issue #37): AddPortMapping
    /// identical to an entry it holds succeeds and changes nothing, lease
    /// included.
    bool ignoresRenewal = false;
    /// Ports the router refuses outright, both leases.
    QSet<uint16_t> refuse;
    int adds = 0;
    int removes = 0;

    void foreign(uint16_t port, const QString& client)
    {
        table.insert({port, QStringLiteral("UDP")}, {client, port});
    }

    /// A neighbour's TCP forward alone: a web server, a game, someone's own tunnel.
    void foreignTcp(uint16_t port, const QString& client)
    {
        table.insert({port, QStringLiteral("TCP")}, {client, port});
    }

    /// Time passes: leases count down, and an entry whose lease runs out is gone.
    void elapse(uint32_t seconds)
    {
        for (auto it = table.begin(); it != table.end();) {
            if (it->lease == 0) {
                ++it;
            } else if (it->lease <= seconds) {
                it = table.erase(it);
            } else {
                it->lease -= seconds;
                ++it;
            }
        }
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
        if (ignoresRenewal && table.contains(key) && table[key].client == lan &&
            table[key].internal == in)
            return true;
        table[key] = {lan, in, leaseSec};
        return true;
    }

    bool owner(uint16_t ext, const QString& proto, QString& client, int& leaseLeft) override
    {
        if (mute) return false;
        const auto key = qMakePair(ext, proto);
        if (!table.contains(key)) return false;
        client = table[key].client;
        leaseLeft = static_cast<int>(table[key].lease);
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
            bool owner(uint16_t ext, const QString& proto, QString& client, int& leaseLeft) override
            {
                // First look at 3478 says "nobody"; the read-back tells the truth.
                if (ext == 3478 && looks++ == 0) return false;
                return FakeGateway::owner(ext, proto, client, leaseLeft);
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

    // ── MiniUPnPd ≤ 1.9 answers a renewal without renewing (#37): the
    //    entry is removed and added again, so it never runs out ──────────
    {
        using mw::routerports::kLeaseSec;
        const auto udp = qMakePair(uint16_t(48550), QStringLiteral("UDP"));
        const auto tcp = qMakePair(uint16_t(48550), QStringLiteral("TCP"));
        FakeGateway gw;
        gw.ignoresRenewal = true;
        RouterPortCore core(gw, kAlwaysFree);
        CHECK(core.claim(mediaRequest(0)).ok);
        CHECK_EQ(gw.table[udp].lease, kLeaseSec);

        // The timer's first tick lands soon after the claim: the entry still
        // reads close to a full lease, and nothing tells the two apart yet.
        gw.elapse(300);
        gw.removes = 0;
        CHECK(core.renew().isEmpty());
        CHECK_EQ(gw.removes, 0);
        // Half an hour on, the countdown gives it away.
        gw.elapse(1800);
        CHECK(core.renew().isEmpty());
        CHECK_EQ(gw.removes, 2); // UDP and TCP
        CHECK_EQ(gw.table[udp].lease, kLeaseSec);
        CHECK_EQ(gw.table[tcp].lease, kLeaseSec);
        CHECK(core.holds(48550));
        CHECK_EQ(core.renewFailures(), 0);

        // Six hours of half-hour ticks: the hole is there between every two.
        bool alwaysThere = true;
        for (int tick = 0; tick < 12; ++tick) {
            gw.elapse(1800);
            alwaysThere = alwaysThere && gw.table.contains(udp) && gw.table.contains(tcp);
            core.renew();
        }
        CHECK(alwaysThere);
    }

    // ── A router that honours renewals is never made to churn ────────────
    {
        FakeGateway gw;
        RouterPortCore core(gw, kAlwaysFree);
        CHECK(core.claim(mediaRequest(0)).ok);
        gw.removes = 0;
        for (int tick = 0; tick < 4; ++tick) {
            gw.elapse(1800);
            CHECK(core.renew().isEmpty());
        }
        CHECK_EQ(gw.removes, 0);
        CHECK_EQ(gw.table[qMakePair(uint16_t(48550), QStringLiteral("UDP"))].lease,
                 mw::routerports::kLeaseSec);
    }

    // ── An entry this host left behind (killed, not stopped) is made
    //    afresh, not kept on its old lease ───────────────────────────────
    {
        using mw::routerports::kLeaseSec;
        const auto udp = qMakePair(uint16_t(3478), QStringLiteral("UDP"));
        const auto tcp = qMakePair(uint16_t(3478), QStringLiteral("TCP"));
        FakeGateway gw;
        gw.ignoresRenewal = true;
        gw.table.insert(udp, {gw.lan, 3478, 200}); // minutes from running out
        gw.table.insert(tcp, {gw.lan, 3478, 200});
        RouterPortCore core(gw, kAlwaysFree);

        const auto r = core.claim(tunnelRequest({3478}));
        CHECK(r.ok);
        CHECK_EQ(r.claim.external, uint16_t(3478));
        CHECK(r.claim.tcp);
        CHECK_EQ(gw.table[udp].lease, kLeaseSec);
        CHECK_EQ(gw.table[tcp].lease, kLeaseSec);
    }

    // ── The switch to permanent mappings takes on such a router too ──────
    {
        const auto udp = qMakePair(uint16_t(3478), QStringLiteral("UDP"));
        const auto tcp = qMakePair(uint16_t(3478), QStringLiteral("TCP"));
        FakeGateway gw;
        gw.ignoresRenewal = true;
        RouterPortCore core(gw, kAlwaysFree);
        CHECK(core.claim(tunnelRequest()).ok);
        gw.refuse.insert(3478);
        core.renew();
        core.renew();
        CHECK_EQ(core.leaseSec(), uint32_t(0));

        // The router answers again. A permanent write over the leased entry
        // is ignored like any other, so the entry is made again.
        gw.refuse.clear();
        gw.elapse(600);
        CHECK(core.renew().isEmpty());
        CHECK(gw.table.contains(udp));
        CHECK_EQ(gw.table[udp].lease, uint32_t(0));
        CHECK_EQ(gw.table[tcp].lease, uint32_t(0));
    }

    // ── One hole, now: a router that lost its table gets it back at once ──
    {
        const auto udp = qMakePair(uint16_t(48550), QStringLiteral("UDP"));
        const auto tcp = qMakePair(uint16_t(48550), QStringLiteral("TCP"));
        FakeGateway gw;
        RouterPortCore core(gw, kAlwaysFree);
        CHECK(core.claim(mediaRequest(0)).ok);
        CHECK(core.claim(tunnelRequest()).ok);
        gw.table.clear(); // rebooted
        const int adds = gw.adds;

        CHECK(core.renew(48550).isEmpty());
        CHECK(gw.table.contains(udp));
        CHECK(gw.table.contains(tcp));
        CHECK_EQ(gw.adds, adds + 2);
        // That one only: the tunnel's waits for its own turn.
        CHECK(!gw.table.contains(qMakePair(uint16_t(3478), QStringLiteral("UDP"))));
        // A port this host does not hold is not written.
        CHECK(core.renew(48551).isEmpty());
        CHECK_EQ(gw.adds, adds + 2);

        // One a neighbour took meanwhile is dropped and reported, as at renewal.
        gw.keepsForeignEntries = true;
        gw.table[udp].client = QStringLiteral("192.168.1.34");
        const auto lost = core.renew(48550);
        CHECK_EQ(lost.size(), 1);
        CHECK_EQ(lost.first().owner, QString("192.168.1.34"));
        CHECK(!core.holds(48550));
        CHECK(core.holds(3478));
        CHECK_EQ(core.renewFailures(), 0);
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

    // ── A neighbour's TCP forward on our UDP number is never touched ──────
    {
        using mw::routerports::kLeaseSec;
        const auto tcp = qMakePair(uint16_t(48550), QStringLiteral("TCP"));
        const QString neighbour = QStringLiteral("192.168.1.34");

        // At the claim: the UDP hole is ours, the TCP one stays theirs.
        FakeGateway gw;
        gw.foreignTcp(48550, neighbour);
        RouterPortCore core(gw, kAlwaysFree);
        const auto r = core.claim(mediaRequest(0));
        CHECK(r.ok);
        CHECK_EQ(r.claim.external, uint16_t(48550));
        CHECK(!r.claim.tcp);
        CHECK_EQ(gw.table[tcp].client, neighbour);

        // Nor at a renewal, nor at the release.
        gw.elapse(1800);
        core.renew();
        CHECK_EQ(gw.table[tcp].client, neighbour);
        core.releaseAll();
        CHECK(gw.table.contains(tcp));
        CHECK_EQ(gw.table[tcp].client, neighbour);

        // A router that keeps its old lease on a write (#37) is re-added by
        // remove-then-add: never on the neighbour's entry.
        FakeGateway mini;
        mini.ignoresRenewal = true;
        mini.table.insert({48550, QStringLiteral("UDP")}, {mini.lan, 48550, kLeaseSec / 2});
        mini.foreignTcp(48550, neighbour);
        RouterPortCore kept(mini, kAlwaysFree);
        const auto k = kept.claim(mediaRequest(0));
        CHECK(k.ok);
        CHECK(!k.claim.tcp);
        CHECK(mini.table.contains(tcp));
        CHECK_EQ(mini.table[tcp].client, neighbour);

        // Taken after our claim: our renewal and our release leave it alone.
        FakeGateway later;
        RouterPortCore core2(later, kAlwaysFree);
        const auto c = core2.claim(mediaRequest(0));
        CHECK(c.ok && c.claim.tcp);
        later.foreignTcp(48550, neighbour);
        later.elapse(1800);
        core2.renew();
        CHECK_EQ(later.table[tcp].client, neighbour);
        core2.releaseAll();
        CHECK_EQ(later.table[tcp].client, neighbour);
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
        // The DEV block sits between the installed host's pool and the media pool.
        CHECK(kTunnelPoolEnd < kDevTunnelPoolBegin);
        CHECK(kDevTunnelPoolBegin <= kDevTunnelPoolEnd);
        CHECK(kDevTunnelPoolEnd < kMediaPoolBegin);
        CHECK(kDevTunnelPoolEnd - kDevTunnelPoolBegin + 1 >= kTunnelPortCap);
    }

    // ── A --dev beside the installed host never asks for the installed
    //    host's tunnel ports (09/10/2026: a --dev held 3479, and the
    //    installed host's browsers looped on their lock screen) ─────────────
    {
        using namespace mw::routerports;
        for (const uint16_t p : kTunnelPreferred) {
            CHECK(RouterPortCore::tunnelPortAllowed(false, p));
            CHECK(!RouterPortCore::tunnelPortAllowed(true, p));
        }
        CHECK(!RouterPortCore::tunnelPortAllowed(true, kTunnelPoolBegin));
        CHECK(!RouterPortCore::tunnelPortAllowed(true, kTunnelPoolEnd));
        CHECK(RouterPortCore::tunnelPortAllowed(true, kDevTunnelPoolBegin));
        CHECK(RouterPortCore::tunnelPortAllowed(true, kDevTunnelPoolEnd));
        CHECK(!RouterPortCore::tunnelPortAllowed(false, kDevTunnelPoolBegin));
        CHECK(!RouterPortCore::tunnelPortAllowed(false, 48550));

        // The installed host's request is the one it always made.
        const auto prod = RouterPortCore::tunnelRequest(false, {3479, kDevTunnelPoolBegin});
        CHECK_EQ(prod.remembered, (QList<uint16_t>{3479}));
        CHECK_EQ(prod.preferred.size(), qsizetype(std::size(kTunnelPreferred)));
        CHECK_EQ(prod.poolBegin, kTunnelPoolBegin);
        CHECK_EQ(prod.poolEnd, kTunnelPoolEnd);

        // A --dev that remembers 3479 from before forgets it, and walks its block.
        const auto dev = RouterPortCore::tunnelRequest(true, {3479, kDevTunnelPoolBegin + 1});
        CHECK_EQ(dev.remembered, (QList<uint16_t>{uint16_t(kDevTunnelPoolBegin + 1)}));
        CHECK(dev.preferred.isEmpty());
        CHECK_EQ(dev.poolBegin, kDevTunnelPoolBegin);
        CHECK_EQ(dev.poolEnd, kDevTunnelPoolEnd);

        // Both on one machine, one LAN address, one router: the installed host
        // took 3478 and 3479, released between browsers. The --dev lands in
        // its own block whatever the table says, and the installed host's
        // next claim is still at the front of the list.
        FakeGateway gw;
        RouterPortCore installed(gw, kAlwaysFree);
        CHECK_EQ(installed.claim(RouterPortCore::tunnelRequest(false, {})).claim.external,
                 uint16_t(3478));
        RouterPortCore scratch(gw, kAlwaysFree);
        const auto r = scratch.claim(RouterPortCore::tunnelRequest(true, {3479}));
        CHECK(r.ok);
        CHECK_EQ(r.claim.external, kDevTunnelPoolBegin);
        CHECK_EQ(installed.claim(RouterPortCore::tunnelRequest(false, {})).claim.external,
                 uint16_t(3479));
    }

    // ── A custom media_base_port moves stream slots and is avoided by the
    //    control tunnel (e.g. 3480 for restrictive corporate firewalls) ───
    {
        using namespace mw::routerports;

        // With default mediaBasePort (48550), all kTunnelPreferred ports are allowed.
        for (const uint16_t p : kTunnelPreferred) {
            CHECK(RouterPortCore::tunnelPortAllowed(false, p));
            CHECK(RouterPortCore::tunnelPortAllowed(false, p, kMediaBasePort));
        }

        // With mediaBasePort = 3480, stream slots take 3480..3503:
        // 3480 and 3481 are masked from the tunnel, while 3478, 3479 and 5349 remain allowed.
        CHECK(!RouterPortCore::tunnelPortAllowed(false, 3480, 3480));
        CHECK(!RouterPortCore::tunnelPortAllowed(false, 3481, 3480));
        CHECK(RouterPortCore::tunnelPortAllowed(false, 3478, 3480));
        CHECK(RouterPortCore::tunnelPortAllowed(false, 3479, 3480));
        CHECK(RouterPortCore::tunnelPortAllowed(false, 5349, 3480));
        CHECK(
            !RouterPortCore::tunnelPortAllowed(false, uint16_t(3480 + kMediaPortCount - 1), 3480));
        // 3480 + kMediaPortCount (3504) is outside the tunnel pool entirely, so also not allowed.
        CHECK(!RouterPortCore::tunnelPortAllowed(false, uint16_t(3480 + kMediaPortCount), 3480));
        // When mediaBasePort lands in the tunnel pool, ports immediately past the media block
        // remain allowed.
        CHECK(!RouterPortCore::tunnelPortAllowed(
            false, uint16_t(kTunnelPoolBegin + kMediaPortCount - 1), kTunnelPoolBegin));
        CHECK(RouterPortCore::tunnelPortAllowed(false, uint16_t(kTunnelPoolBegin + kMediaPortCount),
                                                kTunnelPoolBegin));

        // tunnelRequest filters media ports out of remembered and preferred.
        const auto req = RouterPortCore::tunnelRequest(false, {3480, 3479}, 3480);
        CHECK_EQ(req.remembered, (QList<uint16_t>{3479}));
        CHECK(!req.preferred.contains(3480));
        CHECK(!req.preferred.contains(3481));
        CHECK(req.preferred.contains(3478));
        CHECK(req.preferred.contains(3479));
        for (uint16_t p = 5349; p <= 5352; ++p)
            CHECK(req.preferred.contains(p));
        CHECK_EQ(req.preferred, (QList<uint16_t>{3478, 3479, 5349, 5350, 5351, 5352}));

        // Default argument unchanged: 3480 remains in preferred and remembered.
        const auto defReq = RouterPortCore::tunnelRequest(false, {3480});
        CHECK(defReq.remembered.contains(3480));
        CHECK(defReq.preferred.contains(3480));

        // A media claim for internal port 3480 maps external == internal on the gateway.
        FakeGateway gw;
        RouterPortCore core(gw, kAlwaysFree);
        RouterPortCore::Request mediaReq;
        mediaReq.purpose = Purpose::Media;
        mediaReq.slot = 0;
        mediaReq.internalPort = 3480;
        mediaReq.preferred.append(3480);
        mediaReq.description = QStringLiteral("MoonlightWeb media slot 0");
        const auto mediaClaim = core.claim(mediaReq);
        CHECK(mediaClaim.ok);
        CHECK_EQ(mediaClaim.claim.external, uint16_t(3480));
        CHECK_EQ(mediaClaim.claim.internal, uint16_t(3480));
        CHECK_EQ(mediaClaim.claim.slot, 0);
        CHECK(gw.table.contains(qMakePair(uint16_t(3480), QStringLiteral("UDP"))));
        CHECK(gw.table.contains(qMakePair(uint16_t(3480), QStringLiteral("TCP"))));
    }
}
