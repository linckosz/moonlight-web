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
    struct Row
    {
        QString client;
        uint16_t internal = 0;
        /// Seconds left; 0 = permanent.
        uint32_t lease = 0;
    };

    QString lan = QStringLiteral("192.168.1.20");
    /// (external, protocol) → entry
    QMap<QPair<uint16_t, QString>, Row> table;
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

    Lookup entry(uint16_t ext, const QString& proto, Entry& out) override
    {
        if (mute) return Lookup::Unknown;
        const auto key = qMakePair(ext, proto);
        if (!table.contains(key)) return Lookup::None;
        out.client = table[key].client;
        out.internalPort = table[key].internal;
        out.leaseLeft = static_cast<int>(table[key].lease);
        return Lookup::Found;
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
            Lookup entry(uint16_t ext, const QString& proto, Entry& out) override
            {
                // First look at 3478 says "nobody"; the read-back tells the truth.
                if (ext == 3478 && looks++ == 0) return Lookup::None;
                return FakeGateway::entry(ext, proto, out);
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

    // ── The streams' shared ports: the router forwards each to itself ─────
    {
        using namespace mw::routerports;
        const int kStandard = RouterPortCore::kMuxStandard;
        const int kBlock = RouterPortCore::kMuxBlock;
        const auto udp = [](uint16_t p) { return qMakePair(p, QStringLiteral("UDP")); };
        const auto tcp = [](uint16_t p) { return qMakePair(p, QStringLiteral("TCP")); };

        // A lone host: the conferencing port and the head of the block.
        FakeGateway gw;
        RouterPortCore core(gw, kAlwaysFree);
        const auto a = core.claim(RouterPortCore::muxRequest(false, kStandard, {}));
        CHECK(a.ok);
        CHECK(a.claim.purpose == Purpose::Mux);
        CHECK_EQ(a.claim.slot, kStandard);
        CHECK_EQ(a.claim.external, uint16_t(3478));
        CHECK_EQ(a.claim.internal, uint16_t(3478));
        CHECK_EQ(gw.table[udp(3478)].internal, uint16_t(3478));
        CHECK(!gw.table.contains(tcp(3478)));
        const auto b = core.claim(RouterPortCore::muxRequest(false, kBlock, {}));
        CHECK(b.ok);
        CHECK_EQ(b.claim.slot, kBlock);
        CHECK_EQ(b.claim.external, kMediaBasePort);
        CHECK_EQ(b.claim.internal, kMediaBasePort);
        // One allocator for the tunnel and the mux: the tunnel walks past the
        // port the streams hold, and is never handed it.
        const auto t = core.claim(RouterPortCore::tunnelRequest(false, {3478}));
        CHECK(t.ok);
        CHECK_EQ(t.claim.external, uint16_t(3479));
        CHECK(t.skipped.isEmpty());
        CHECK_EQ(core.held().size(), 3);

        // The other way round: the tunnel got there first, the streams walk on.
        FakeGateway gw2;
        RouterPortCore core2(gw2, kAlwaysFree);
        CHECK_EQ(core2.claim(RouterPortCore::tunnelRequest(false, {})).claim.external,
                 uint16_t(3478));
        CHECK_EQ(core2.claim(RouterPortCore::muxRequest(false, kStandard, {})).claim.external,
                 uint16_t(3479));

        // A neighbour on the front of both walks: never its entries, and the
        // block port moves up the block, not out to the media pool — the mux
        // has no use for a hole whose number its socket does not bind.
        FakeGateway busy;
        busy.foreign(3478, QStringLiteral("192.168.1.34"));
        busy.foreign(3479, QStringLiteral("192.168.1.34"));
        busy.foreign(kMediaBasePort, QStringLiteral("192.168.1.34"));
        RouterPortCore core3(busy, kAlwaysFree);
        const auto s = core3.claim(RouterPortCore::muxRequest(false, kStandard, {}));
        CHECK(s.ok);
        CHECK_EQ(s.claim.external, uint16_t(3480));
        CHECK_EQ(s.skipped.size(), 2);
        CHECK(s.skipped.first().reason == Reason::Foreign);
        const auto k = core3.claim(RouterPortCore::muxRequest(false, kBlock, {}));
        CHECK(k.ok);
        CHECK_EQ(k.claim.external, uint16_t(kMediaBasePort + 1));
        CHECK_EQ(k.claim.internal, uint16_t(kMediaBasePort + 1));
        CHECK_EQ(busy.table[udp(3478)].client, QString("192.168.1.34"));
        CHECK_EQ(busy.table[udp(kMediaBasePort)].client, QString("192.168.1.34"));

        // All eight conferencing ports taken: none for the streams, and no
        // pool behind the list. The block port is still theirs.
        FakeGateway full;
        for (const uint16_t p : kTunnelPreferred)
            full.foreign(p, QStringLiteral("192.168.1.34"));
        RouterPortCore core4(full, kAlwaysFree);
        const auto none = core4.claim(RouterPortCore::muxRequest(false, kStandard, {}));
        CHECK(!none.ok);
        CHECK_EQ(none.skipped.size(), 8);
        CHECK(core4.held().isEmpty());
        CHECK(core4.claim(RouterPortCore::muxRequest(false, kBlock, {})).ok);

        // A remembered port comes first, as for the tunnel; one this host may
        // not hold as that port of the mux is forgotten.
        const auto req = RouterPortCore::muxRequest(false, kStandard, {5350, 46000, 48550});
        CHECK_EQ(req.remembered, (QList<uint16_t>{5350}));
        CHECK_EQ(req.preferred.size(), qsizetype(std::size(kTunnelPreferred)));
        CHECK_EQ(RouterPortCore::muxRequest(false, kBlock, {3478, uint16_t(kMediaBasePort + 2)})
                     .remembered,
                 (QList<uint16_t>{uint16_t(kMediaBasePort + 2)}));
        FakeGateway again;
        RouterPortCore core5(again, kAlwaysFree);
        CHECK_EQ(core5.claim(RouterPortCore::muxRequest(false, kStandard, {5350})).claim.external,
                 uint16_t(5350));
    }

    // ── A shared port is read in UDP alone: no TCP hole is asked for it ────
    {
        using namespace mw::routerports;
        const auto udp = [](uint16_t p) { return qMakePair(p, QStringLiteral("UDP")); };
        const auto tcp = [](uint16_t p) { return qMakePair(p, QStringLiteral("TCP")); };

        CHECK(!RouterPortCore::muxRequest(false, RouterPortCore::kMuxStandard, {}).tcp);
        CHECK(!RouterPortCore::muxRequest(false, RouterPortCore::kMuxBlock, {}).tcp);
        CHECK(!RouterPortCore::muxRequest(true, RouterPortCore::kMuxBlock, {}).tcp);
        CHECK(RouterPortCore::tunnelRequest(false, {}).tcp);

        FakeGateway gw;
        RouterPortCore core(gw, kAlwaysFree);
        const auto a =
            core.claim(RouterPortCore::muxRequest(false, RouterPortCore::kMuxStandard, {}));
        const auto b = core.claim(RouterPortCore::muxRequest(false, RouterPortCore::kMuxBlock, {}));
        CHECK(a.ok && b.ok);
        CHECK(!a.claim.tcp);
        CHECK(!b.claim.tcp);
        CHECK_EQ(gw.adds, 2);
        CHECK(gw.table.contains(udp(3478)));
        CHECK(gw.table.contains(udp(kMediaBasePort)));
        CHECK(!gw.table.contains(tcp(3478)));
        CHECK(!gw.table.contains(tcp(kMediaBasePort)));

        // A renewal writes UDP alone, the half-hourly one and the one made
        // for a single port.
        gw.elapse(1800);
        CHECK(core.renew().isEmpty());
        CHECK_EQ(gw.adds, 4);
        CHECK(core.renew(3478).isEmpty());
        CHECK_EQ(gw.adds, 5);
        CHECK(!gw.table.contains(tcp(3478)));
        CHECK(!gw.table.contains(tcp(kMediaBasePort)));

        // The same on a router that ignores a renewal and gets the entry afresh.
        FakeGateway deco;
        deco.ignoresRenewal = true;
        deco.table.insert(udp(3478), {deco.lan, 3478, 200});
        RouterPortCore core2(deco, kAlwaysFree);
        const auto c =
            core2.claim(RouterPortCore::muxRequest(false, RouterPortCore::kMuxStandard, {}));
        CHECK(c.ok);
        CHECK(!c.claim.tcp);
        CHECK_EQ(deco.table[udp(3478)].lease, kLeaseSec);
        deco.elapse(1800);
        CHECK(core2.renew().isEmpty());
        CHECK_EQ(deco.table[udp(3478)].lease, kLeaseSec);
        CHECK(!deco.table.contains(tcp(3478)));

        // A TCP forward on the same number is somebody else's business, the
        // neighbour's or another program's on this machine: not written over
        // at the claim, not at the renewal, not removed at the release.
        FakeGateway mixed;
        mixed.foreignTcp(3478, QStringLiteral("192.168.1.34"));
        mixed.foreignTcp(3479, mixed.lan);
        RouterPortCore core3(mixed, kAlwaysFree);
        const auto d =
            core3.claim(RouterPortCore::muxRequest(false, RouterPortCore::kMuxStandard, {}));
        CHECK(d.ok);
        CHECK_EQ(d.claim.external, uint16_t(3478));
        CHECK_EQ(mixed.table[tcp(3478)].client, QString("192.168.1.34"));
        const auto e =
            core3.claim(RouterPortCore::muxRequest(false, RouterPortCore::kMuxStandard, {}));
        CHECK(e.ok);
        CHECK_EQ(e.claim.external, uint16_t(3479));
        core3.renew();
        core3.releaseAll();
        CHECK(core3.held().isEmpty());
        CHECK(!mixed.table.contains(udp(3478)));
        CHECK(!mixed.table.contains(udp(3479)));
        CHECK_EQ(mixed.table[tcp(3478)].client, QString("192.168.1.34"));
        CHECK(mixed.table.contains(tcp(3479)));
        CHECK_EQ(mixed.table[tcp(3479)].lease, uint32_t(0));

        // Where something does listen in TCP, the request says so and the
        // hole comes alongside, as for the tunnel.
        FakeGateway both;
        RouterPortCore core4(both, kAlwaysFree);
        auto listening = RouterPortCore::muxRequest(false, RouterPortCore::kMuxStandard, {});
        listening.tcp = true;
        const auto f = core4.claim(listening);
        CHECK(f.ok);
        CHECK(f.claim.tcp);
        CHECK_EQ(both.table[tcp(3478)].internal, uint16_t(3478));
    }

    // ── The mux binds for real, late, and hands back what the router denies ─
    {
        using namespace mw::routerports;
        using Hold = RouterPortCore::Hold;
        const int kStandard = RouterPortCore::kMuxStandard;
        QList<uint16_t> bound;
        QList<uint16_t> handedBack;
        QSet<uint16_t> taken;   // by another program on this machine
        QSet<uint16_t> already; // bound by the mux before the claim
        const auto request = [&]() {
            auto r = RouterPortCore::muxRequest(false, kStandard, {});
            r.hold = [&](uint16_t p) {
                if (taken.contains(p)) return Hold::Failed;
                if (already.contains(p)) return Hold::AlreadyHeld;
                bound.append(p);
                return Hold::NewlyBound;
            };
            r.drop = [&](uint16_t p) { handedBack.append(p); };
            return r;
        };
        // The probe is the tunnel's: the mux never asks it once it has a hold.
        const auto kNever = [](uint16_t) { return false; };

        // A neighbour's port costs no socket; a port another program holds is
        // passed over; the first that can be bound is kept.
        FakeGateway gw;
        gw.foreign(3478, QStringLiteral("192.168.1.34"));
        taken.insert(3479);
        RouterPortCore core(gw, kNever);
        const auto r = core.claim(request());
        CHECK(r.ok);
        CHECK_EQ(r.claim.external, uint16_t(3480));
        CHECK_EQ(bound, (QList<uint16_t>{3480}));
        CHECK(handedBack.isEmpty());
        CHECK_EQ(r.skipped.size(), 2);
        CHECK(r.skipped.at(0).reason == Reason::Foreign);
        CHECK(r.skipped.at(1).reason == Reason::LocalBusy);

        // The router refuses the port once it is bound: handed back, next one.
        bound.clear();
        taken.clear();
        FakeGateway picky;
        picky.refuse.insert(3478);
        RouterPortCore core2(picky, kNever);
        const auto r2 = core2.claim(request());
        CHECK(r2.ok);
        CHECK_EQ(r2.claim.external, uint16_t(3479));
        CHECK_EQ(bound, (QList<uint16_t>{3478, 3479}));
        CHECK_EQ(handedBack, (QList<uint16_t>{3478}));

        // "Added", yet the table still aims at the neighbour: handed back too.
        bound.clear();
        handedBack.clear();
        struct LateLiar : FakeGateway
        {
            int looks = 0;
            Lookup entry(uint16_t ext, const QString& proto, Entry& out) override
            {
                if (ext == 3478 && looks++ == 0) return Lookup::None;
                return FakeGateway::entry(ext, proto, out);
            }
        } liar;
        liar.keepsForeignEntries = true;
        liar.foreign(3478, QStringLiteral("192.168.1.34"));
        RouterPortCore core3(liar, kNever);
        const auto r3 = core3.claim(request());
        CHECK(r3.ok);
        CHECK_EQ(r3.claim.external, uint16_t(3479));
        CHECK_EQ(handedBack, (QList<uint16_t>{3478}));
        CHECK(!core3.holds(3478));

        // An entry this host left behind is made afresh — and the router will
        // not have it the second time: handed back as well, and the walk goes on.
        bound.clear();
        handedBack.clear();
        struct Fickle : FakeGateway
        {
            bool remove(uint16_t ext, const QString& proto) override
            {
                refuse.insert(ext);
                return FakeGateway::remove(ext, proto);
            }
        } fickle;
        fickle.ignoresRenewal = true;
        const auto left = qMakePair(uint16_t(3478), QStringLiteral("UDP"));
        fickle.table.insert(left, {fickle.lan, 3478, 200});
        RouterPortCore core6(fickle, kNever);
        const auto r6 = core6.claim(request());
        CHECK(r6.ok);
        CHECK_EQ(r6.claim.external, uint16_t(3479));
        CHECK_EQ(bound, (QList<uint16_t>{3478, 3479}));
        CHECK_EQ(handedBack, (QList<uint16_t>{3478}));
        CHECK_EQ(r6.skipped.size(), 1);
        CHECK(r6.skipped.first().reason == Reason::Refused);
        CHECK(!fickle.table.contains(left));

        // A port the mux held before the claim — the hole was lost, the socket
        // kept — is claimed again without a new bind...
        bound.clear();
        handedBack.clear();
        already.insert(3478);
        FakeGateway back;
        RouterPortCore core7(back, kNever);
        const auto r7 = core7.claim(request());
        CHECK(r7.ok);
        CHECK_EQ(r7.claim.external, uint16_t(3478));
        CHECK(bound.isEmpty());
        CHECK(handedBack.isEmpty());
        // ...and is never handed back, whatever the router answers: that
        // socket may be serving streams. Refused, kept elsewhere, refused
        // afresh — the walk goes on, the socket stays.
        FakeGateway no;
        no.refuse.insert(3478);
        RouterPortCore core8(no, kNever);
        const auto r8 = core8.claim(request());
        CHECK(r8.ok);
        CHECK_EQ(r8.claim.external, uint16_t(3479));
        CHECK(r8.skipped.first().reason == Reason::Refused);
        CHECK_EQ(bound, (QList<uint16_t>{3479}));
        CHECK(handedBack.isEmpty());

        bound.clear();
        LateLiar liar2;
        liar2.keepsForeignEntries = true;
        liar2.foreign(3478, QStringLiteral("192.168.1.34"));
        RouterPortCore core9(liar2, kNever);
        const auto r9 = core9.claim(request());
        CHECK(r9.ok);
        CHECK_EQ(r9.claim.external, uint16_t(3479));
        CHECK(r9.skipped.first().reason == Reason::KeptElsewhere);
        CHECK(handedBack.isEmpty());

        bound.clear();
        Fickle fickle2;
        fickle2.ignoresRenewal = true;
        fickle2.table.insert(left, {fickle2.lan, 3478, 200});
        RouterPortCore core10(fickle2, kNever);
        const auto r10 = core10.claim(request());
        CHECK(r10.ok);
        CHECK_EQ(r10.claim.external, uint16_t(3479));
        CHECK_EQ(bound, (QList<uint16_t>{3479}));
        CHECK(handedBack.isEmpty());
        already.clear();

        // Nothing can be bound at all: no hole, and not one write to the router.
        bound.clear();
        handedBack.clear();
        for (const uint16_t p : kTunnelPreferred)
            taken.insert(p);
        FakeGateway idle;
        RouterPortCore core4(idle, kNever);
        const auto r4 = core4.claim(request());
        CHECK(!r4.ok);
        CHECK_EQ(r4.skipped.size(), 8);
        CHECK_EQ(idle.adds, 0);
        CHECK(bound.isEmpty());
        CHECK(handedBack.isEmpty());

        // Without a hold the probe answers, as it does for the tunnel.
        FakeGateway plain;
        RouterPortCore core5(plain, [](uint16_t p) { return p != 3478; });
        const auto r5 = core5.claim(RouterPortCore::muxRequest(false, kStandard, {}));
        CHECK(r5.ok);
        CHECK_EQ(r5.claim.external, uint16_t(3479));
        CHECK(r5.skipped.first().reason == Reason::LocalBusy);
    }

    // ── A shared port the router now aims elsewhere is reported, like any ──
    {
        FakeGateway gw;
        RouterPortCore core(gw, kAlwaysFree);
        const auto a =
            core.claim(RouterPortCore::muxRequest(false, RouterPortCore::kMuxStandard, {}));
        const auto b = core.claim(RouterPortCore::muxRequest(false, RouterPortCore::kMuxBlock, {}));
        CHECK(a.ok && b.ok);

        gw.keepsForeignEntries = true;
        gw.foreign(a.claim.external, QStringLiteral("192.168.1.34"));
        gw.elapse(1800);
        const auto lost = core.renew();
        CHECK_EQ(lost.size(), 1);
        CHECK(lost.first().claim.purpose == Purpose::Mux);
        CHECK_EQ(lost.first().claim.slot, RouterPortCore::kMuxStandard);
        CHECK_EQ(lost.first().owner, QString("192.168.1.34"));
        CHECK(!core.holds(a.claim.external));
        CHECK(core.holds(b.claim.external));
        // The next claim for that port of the mux walks past the neighbour.
        const auto next =
            core.claim(RouterPortCore::muxRequest(false, RouterPortCore::kMuxStandard, {3478}));
        CHECK(next.ok);
        CHECK_EQ(next.claim.external, uint16_t(3479));
    }

    // ── A --dev shares one port, from the top of the block, and never a
    //    conferencing port; the installed host's walk and its own never meet ─
    {
        using namespace mw::routerports;
        const int kStandard = RouterPortCore::kMuxStandard;
        const int kBlock = RouterPortCore::kMuxBlock;
        const uint16_t top = static_cast<uint16_t>(kMediaBasePort + kMediaPortCount - 1);

        for (const uint16_t p : kTunnelPreferred) {
            CHECK(RouterPortCore::muxPortAllowed(false, kStandard, p));
            CHECK(!RouterPortCore::muxPortAllowed(true, kStandard, p));
            CHECK(!RouterPortCore::muxPortAllowed(false, kBlock, p));
        }
        CHECK(!RouterPortCore::muxPortAllowed(false, kStandard, kTunnelPoolBegin));
        CHECK(!RouterPortCore::muxPortAllowed(false, kStandard, kMediaBasePort));
        CHECK(!RouterPortCore::muxPortAllowed(false, 2, kMediaBasePort));
        // Every port of the block belongs to one walk at most, never to both.
        int installedPorts = 0, devPorts = 0;
        for (int p = kMediaBasePort; p <= top; ++p) {
            const bool installed = RouterPortCore::muxPortAllowed(false, kBlock, uint16_t(p));
            const bool dev = RouterPortCore::muxPortAllowed(true, kBlock, uint16_t(p));
            CHECK(!(installed && dev));
            installedPorts += installed;
            devPorts += dev;
        }
        CHECK_EQ(installedPorts, kMuxBlockSpan);
        CHECK_EQ(devPorts, kMuxBlockSpan);
        CHECK(2 * kMuxBlockSpan <= kMediaPortCount);
        CHECK(RouterPortCore::muxPortAllowed(false, kBlock, kMediaBasePort));
        CHECK(RouterPortCore::muxPortAllowed(true, kBlock, top));
        CHECK(!RouterPortCore::muxPortAllowed(true, kBlock, kMediaBasePort));
        CHECK(!RouterPortCore::muxPortAllowed(false, kBlock, top));
        CHECK(!RouterPortCore::muxPortAllowed(false, kBlock, uint16_t(kMediaBasePort - 1)));
        CHECK(!RouterPortCore::muxPortAllowed(true, kBlock, uint16_t(top + 1)));

        // No conferencing port for a --dev: no candidate, and the router is
        // not asked a thing — whatever the --dev remembers.
        FakeGateway gw;
        RouterPortCore dev(gw, kAlwaysFree);
        const auto none = dev.claim(RouterPortCore::muxRequest(true, kStandard, {3478}));
        CHECK(!none.ok);
        CHECK(none.skipped.isEmpty());
        CHECK_EQ(gw.adds, 0);
        CHECK(gw.table.isEmpty());

        // Its one port: the top of the block, then down.
        const auto req = RouterPortCore::muxRequest(true, kBlock, {kMediaBasePort, 3478});
        CHECK(req.remembered.isEmpty());
        CHECK_EQ(req.preferred.size(), qsizetype(kMuxBlockSpan));
        CHECK_EQ(req.preferred.first(), top);
        CHECK_EQ(req.preferred.last(), uint16_t(top - kMuxBlockSpan + 1));
        const auto first = dev.claim(RouterPortCore::muxRequest(true, kBlock, {}));
        CHECK(first.ok);
        CHECK_EQ(first.claim.external, top);
        CHECK_EQ(first.claim.internal, top);
        FakeGateway gw2;
        RouterPortCore dev2(gw2, [top](uint16_t p) { return p != top; });
        CHECK_EQ(dev2.claim(RouterPortCore::muxRequest(true, kBlock, {})).claim.external,
                 uint16_t(top - 1));

        // Both on one machine, one LAN address, one router: each in its own
        // stretch, whichever started first.
        FakeGateway shared;
        RouterPortCore scratch(shared, kAlwaysFree);
        CHECK_EQ(scratch.claim(RouterPortCore::muxRequest(true, kBlock, {})).claim.external, top);
        RouterPortCore installed(shared, kAlwaysFree);
        CHECK_EQ(installed.claim(RouterPortCore::muxRequest(false, kBlock, {})).claim.external,
                 kMediaBasePort);
        CHECK_EQ(installed.claim(RouterPortCore::muxRequest(false, kStandard, {})).claim.external,
                 uint16_t(3478));
    }

    // ── A stream slot's own port, beside the mux ──────────────────────────
    {
        using namespace mw::routerports;
        const int kBlock = RouterPortCore::kMuxBlock;
        const uint16_t top = uint16_t(kMediaBasePort + kMediaPortCount - 1);
        // Without the mux, and for a DEV identity with it: the slot's number,
        // as ever.
        for (int slot = 0; slot < kMediaPortCount; ++slot) {
            CHECK_EQ(slotMediaPort(slot, false, false), uint16_t(kMediaBasePort + slot));
            CHECK_EQ(slotMediaPort(slot, false, true), uint16_t(kMediaBasePort + slot));
            CHECK_EQ(slotMediaPort(slot, true, true), uint16_t(kMediaBasePort + slot));
        }
        // With it, the installed host's slots start where the mux's stretch ends.
        CHECK_EQ(slotMediaPort(0, true, false), uint16_t(kMediaBasePort + kMuxBlockSpan));
        for (const bool dev : {false, true}) {
            QSet<uint16_t> seen;
            for (int slot = 0; slot < kMediaPortCount; ++slot) {
                const uint16_t port = slotMediaPort(slot, true, dev);
                // A port of the block, each slot its own.
                CHECK(port >= kMediaBasePort && port <= top);
                CHECK(!seen.contains(port));
                seen.insert(port);
                // The first slots, the ones a host runs, never sit in the
                // mux's stretch: whichever number the mux holds there, their
                // hole cannot lead to it.
                if (slot < kMediaPortCount - kMuxBlockSpan) CHECK(!inMuxBlockStretch(dev, port));
                CHECK_EQ(inMuxBlockStretch(dev, port),
                         RouterPortCore::muxPortAllowed(dev, kBlock, port));
            }
        }
    }

    // ── A slot's hole keeps out of the mux's numbers ──────────────────────
    {
        using namespace mw::routerports;
        const int kBlock = RouterPortCore::kMuxBlock;
        // Without the mux: what the slot had, its own number, the pool.
        const uint16_t third = uint16_t(kMediaBasePort + 3);
        const auto plain = RouterPortCore::mediaRequest(false, false, 3, third, 46120, false);
        CHECK(plain.purpose == Purpose::Media);
        CHECK_EQ(plain.slot, 3);
        CHECK_EQ(plain.internalPort, third);
        CHECK(plain.remembered == QList<uint16_t>{46120});
        CHECK(plain.preferred == QList<uint16_t>{third});
        CHECK_EQ(plain.poolBegin, kMediaPoolBegin);
        CHECK_EQ(plain.poolEnd, kMediaPoolEnd);
        CHECK(plain.tcp);
        CHECK(plain.hasCandidate());
        CHECK(RouterPortCore::mediaRequest(false, false, 3, third, 0, false).remembered.isEmpty());
        // A remembered number that is neither of the block nor of the pool is
        // no number a slot's hole may have: it is not asked for again.
        CHECK(
            RouterPortCore::mediaRequest(false, false, 3, third, 3478, false).remembered.isEmpty());
        CHECK(RouterPortCore::mediaRequest(false, false, 3, third, 48010, false)
                  .remembered.isEmpty());
        CHECK(RouterPortCore::mediaPortAllowed(kMediaBasePort));
        CHECK(RouterPortCore::mediaPortAllowed(kMediaPoolBegin));
        CHECK(RouterPortCore::mediaPortAllowed(kMediaPoolEnd));
        CHECK(!RouterPortCore::mediaPortAllowed(uint16_t(kMediaPoolEnd + 1)));
        CHECK(!RouterPortCore::mediaPortAllowed(uint16_t(kMediaBasePort + kMediaPortCount)));
        CHECK(!RouterPortCore::mediaPortAllowed(0));
        // The same request the tests above make by hand.
        const auto byHand = mediaRequest(3, 46120);
        CHECK(byHand.remembered == plain.remembered && byHand.preferred == plain.preferred);
        CHECK(byHand.poolBegin == plain.poolBegin && byHand.poolEnd == plain.poolEnd);

        // With the mux, the installed host's first slot binds 48562. It had
        // 48550 on the router before there was a mux: that number is the
        // mux's now, and is not asked for again.
        const uint16_t own = slotMediaPort(0, true, false);
        const auto beside =
            RouterPortCore::mediaRequest(true, false, 0, own, kMediaBasePort, false);
        CHECK(beside.remembered.isEmpty());
        CHECK(beside.preferred == QList<uint16_t>{own});
        // Without the mux it would have been.
        CHECK(RouterPortCore::mediaRequest(false, false, 0, kMediaBasePort, kMediaBasePort, false)
                  .remembered == QList<uint16_t>{kMediaBasePort});
        // A DEV identity's stretch is the top of the block.
        const uint16_t top = uint16_t(kMediaBasePort + kMediaPortCount - 1);
        const auto devSlot =
            RouterPortCore::mediaRequest(true, true, 0, kMediaBasePort, top, false);
        CHECK(devSlot.remembered.isEmpty());
        CHECK(devSlot.preferred == QList<uint16_t>{kMediaBasePort});

        // Whoever asks first, the mux finds its number free of this host's
        // own slots.
        {
            FakeGateway gw;
            RouterPortCore core(gw, kAlwaysFree);
            const auto slot0 = core.claim(beside);
            CHECK(slot0.ok);
            CHECK_EQ(slot0.claim.external, own);
            CHECK_EQ(slot0.claim.internal, own);
            const auto block = core.claim(RouterPortCore::muxRequest(false, kBlock, {}));
            CHECK(block.ok);
            CHECK_EQ(block.claim.external, kMediaBasePort);
        }

        // A thirteenth slot comes round to the mux's stretch. Its router-side
        // number is from the pool, never from the stretch…
        const uint16_t late = slotMediaPort(kMediaPortCount - kMuxBlockSpan, true, false);
        CHECK(inMuxBlockStretch(false, late));
        const auto pooled = RouterPortCore::mediaRequest(true, false, 12, late, late, false);
        CHECK(pooled.remembered.isEmpty() && pooled.preferred.isEmpty());
        CHECK(pooled.hasCandidate());
        {
            FakeGateway gw;
            RouterPortCore core(gw, kAlwaysFree);
            const auto hole = core.claim(pooled);
            CHECK(hole.ok);
            CHECK_EQ(hole.claim.external, kMediaPoolBegin);
            CHECK_EQ(hole.claim.internal, late);
        }
        // …and when the mux itself is bound to that port, it gets no hole at
        // all: the router is not asked a thing.
        const auto shared = RouterPortCore::mediaRequest(true, false, 12, late, 46120, true);
        CHECK(!shared.hasCandidate());
        CHECK(shared.remembered.isEmpty() && shared.preferred.isEmpty());
        {
            FakeGateway gw;
            RouterPortCore core(gw, kAlwaysFree);
            const auto none = core.claim(shared);
            CHECK(!none.ok);
            CHECK(none.skipped.isEmpty());
            CHECK_EQ(gw.adds, 0);
            CHECK(gw.table.isEmpty());
        }
        // A request that names a port has a candidate; an empty one has none.
        RouterPortCore::Request empty;
        CHECK(!empty.hasCandidate());
        empty.poolBegin = 5;
        empty.poolEnd = 4;
        CHECK(!empty.hasCandidate());
        empty.poolEnd = 5;
        CHECK(empty.hasCandidate());
        CHECK(RouterPortCore::muxRequest(false, RouterPortCore::kMuxStandard, {}).hasCandidate());
        CHECK(
            !RouterPortCore::muxRequest(true, RouterPortCore::kMuxStandard, {3478}).hasCandidate());
    }
}
