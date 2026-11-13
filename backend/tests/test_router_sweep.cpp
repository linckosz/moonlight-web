/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * What a host that was killed left on the router, and what the next start may
 * take back of it (RouterPortCore::sweep): an entry that is this
 * installation's by its record, by its address, by its port and by its
 * description, and nothing else. Every rule here is an entry of another
 * machine, of another program or of another installation that must stay.
 */
#include "test_framework.h"
#include "network/RouterHoles.h"
#include "network/RouterPortCore.h"
#include "network/RouterPortPools.h"

#include <QMap>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>

#include <functional>
#include <optional>

namespace {

using mw::routerholes::Hole;
using mw::routerholes::Kind;
using mw::sockettables::Listens;
using Swept = RouterPortCore::Swept;

const QString kLan = QStringLiteral("192.168.1.20");
const QString kNeighbour = QStringLiteral("192.168.1.34");
const QString kTag = QStringLiteral("a3f9c1");
const QString kUdp = QStringLiteral("UDP");
const QString kTcp = QStringLiteral("TCP");

/// A router's table, with what a router may show and how one may misbehave.
struct TableGateway : IUpnpGateway
{
    struct Row
    {
        QString client;
        uint16_t internal = 0;
        QString description;
        /// Seconds left; 0 = permanent.
        uint32_t lease = 0;

        bool operator==(const Row& o) const
        {
            return client == o.client && internal == o.internal && description == o.description;
        }
    };
    using Key = QPair<uint16_t, QString>;

    QString lan = kLan;
    QMap<Key, Row> table;
    /// Ports it says nothing of: an error, a timeout.
    QSet<uint16_t> silent;
    /// A router that answers this entry, whatever it is asked about.
    std::optional<Row> oneAnswer;
    bool refusesRemoval = false;
    /// Says "removed" and removes nothing.
    bool keepsRemoved = false;
    /// MiniUPnPd up to 1.9: a write over an entry of the same client and port
    /// succeeds and changes nothing, description included.
    bool keepsText = false;
    /// A router that writes a text of its own, whatever it is given.
    QString itsOwnText;
    /// A Livebox: a write over another machine's entry succeeds and the
    /// table keeps pointing at that machine.
    bool keepsForeign = false;
    QSet<uint16_t> refuse;
    int adds = 0;
    int removes = 0;
    QStringList asked;
    std::function<void(uint16_t, const QString&)> beforeAdd;

    QString lanAddress() const override { return lan; }

    bool add(uint16_t ext, uint16_t in, uint32_t leaseSec, const QString& description,
             const QString& proto) override
    {
        if (beforeAdd) beforeAdd(ext, proto);
        ++adds;
        if (refuse.contains(ext)) return false;
        const Key key{ext, proto};
        if (table.contains(key)) {
            const Row& there = table[key];
            if (keepsForeign && there.client != lan) return true;
            if (keepsText && there.client == lan && there.internal == in) return true;
        }
        table[key] = {lan, in, itsOwnText.isEmpty() ? description : itsOwnText, leaseSec};
        return true;
    }

    Lookup entry(uint16_t ext, const QString& proto, Entry& out) override
    {
        asked << proto + QLatin1Char(' ') + QString::number(ext);
        if (silent.contains(ext)) return Lookup::Unknown;
        const Key key{ext, proto};
        if (!oneAnswer && !table.contains(key)) return Lookup::None;
        const Row row = oneAnswer ? *oneAnswer : table[key];
        out.client = row.client;
        out.internalPort = row.internal;
        out.description = row.description;
        out.leaseLeft = static_cast<int>(row.lease);
        return Lookup::Found;
    }

    bool remove(uint16_t ext, const QString& proto) override
    {
        ++removes;
        if (refusesRemoval) return false;
        if (keepsRemoved) return true;
        return table.remove(Key{ext, proto}) > 0;
    }
};

TableGateway::Key udp(uint16_t port)
{
    return {port, kUdp};
}

TableGateway::Key tcp(uint16_t port)
{
    return {port, kTcp};
}

/// The record, in memory.
struct MemoryRecord : mw::routerholes::IRecord
{
    QList<Hole> lines;

    QString tag() const override { return kTag; }
    QList<Hole> holes() const override { return lines; }

    bool find(uint16_t external, bool isTcp, Hole& out) const override
    {
        for (const Hole& line : lines) {
            if (line.external != external || line.tcp != isTcp) continue;
            out = line;
            return true;
        }
        return false;
    }

    void note(const Hole& hole) override
    {
        for (Hole& line : lines) {
            if (line.external != hole.external || line.tcp != hole.tcp) continue;
            line = hole;
            return;
        }
        lines.append(hole);
    }

    void strike(uint16_t external, bool isTcp) override
    {
        for (qsizetype i = 0; i < lines.size(); ++i) {
            if (lines.at(i).external != external || lines.at(i).tcp != isTcp) continue;
            lines.removeAt(i);
            return;
        }
    }

    bool has(uint16_t external, bool isTcp) const
    {
        Hole unused;
        return find(external, isTcp, unused);
    }
};

Hole tunnelLine(uint16_t port, bool isTcp = false)
{
    Hole line;
    line.external = port;
    line.internal = port;
    line.tcp = isTcp;
    line.kind = Kind::Tunnel;
    line.lan = kLan;
    return line;
}

Hole mediaLine(int slot, uint16_t external, bool isTcp = false)
{
    Hole line;
    line.external = external;
    line.internal = static_cast<uint16_t>(mw::routerports::kMediaBasePort + slot);
    line.tcp = isTcp;
    line.kind = Kind::Media;
    line.slot = slot;
    line.lan = kLan;
    return line;
}

/// The router's entry for a line, as the host that noted it wrote it.
TableGateway::Row rowOf(const Hole& line, const QString& tag)
{
    return {line.lan, line.internal, mw::routerholes::describe(line.kind, line.slot, tag), 0};
}

TableGateway::Key keyOf(const Hole& line)
{
    return {line.external, line.tcp ? kTcp : kUdp};
}

const auto kAlwaysFree = [](uint16_t) { return true; };

/// A router, a record, the policy between them, and what the machine says of
/// its own sockets.
struct Bench
{
    TableGateway gw;
    MemoryRecord record;
    RouterPortCore core;
    Listens answer = Listens::No;
    QList<QPair<uint16_t, bool>> listenAsked;

    Bench()
        : core(gw, kAlwaysFree)
    {
        core.setRecord(&record);
        core.setListens([this](uint16_t port, bool isTcp) {
            listenAsked.append({port, isTcp});
            return answer;
        });
    }

    /// A line in the record and its entry on the router, as a killed run left them.
    void left(const Hole& line, const QString& tag = kTag)
    {
        record.note(line);
        gw.table[keyOf(line)] = rowOf(line, tag);
    }
};

} // namespace

void run_router_sweep_tests()
{
    SECTION("RouterSweep: an entry this installation left is taken back");
    {
        Bench b;
        const Hole line = tunnelLine(3478);
        b.left(line);
        CHECK(b.core.sweepList() == QList<Hole>{line});
        CHECK(b.core.sweepBelieved(b.core.sweepList()));
        const auto r = b.core.sweep(line);
        CHECK(r.outcome == Swept::Removed);
        CHECK(b.gw.table.isEmpty());
        CHECK(b.record.lines.isEmpty());
        CHECK_EQ(b.gw.removes, 1);
        // A line with a tag asks nothing of the machine's own sockets
        CHECK(b.listenAsked.isEmpty());
    }
    // A stream slot's hole, forwarded to another number than its own
    {
        Bench b;
        const Hole line = mediaLine(2, 46100, true);
        b.left(line);
        CHECK(b.core.sweep(line).outcome == Swept::Removed);
        CHECK(b.gw.table.isEmpty() && b.record.lines.isEmpty());
    }

    SECTION("RouterSweep: what is not this installation's stays");
    // Left as it is, and no longer in the record: `differs` says by what.
    const auto stays = [](Bench& b, const Hole& line, const TableGateway::Row& there,
                          const char* differs) {
        b.record.note(line);
        b.gw.table[keyOf(line)] = there;
        const auto r = b.core.sweep(line);
        CHECK(r.outcome == Swept::NotOurs);
        CHECK(r.detail == QLatin1String(differs));
        CHECK(b.gw.table.value(keyOf(line)) == there);
        CHECK_EQ(b.gw.removes, 0);
        CHECK(b.record.lines.isEmpty());
    };
    // A neighbour's entry on the number we had, with our very description:
    // the address says whose it is.
    {
        Bench b;
        const Hole line = tunnelLine(3478);
        TableGateway::Row there = rowOf(line, kTag);
        there.client = kNeighbour;
        stays(b, line, there, "address");
    }
    // Another installation on this machine: the same words, another tag
    {
        Bench b;
        const Hole line = tunnelLine(3478);
        stays(b, line, rowOf(line, QStringLiteral("0b77e2")), "description");
    }
    // The product's words alone, at our address: not what this record wrote
    {
        Bench b;
        const Hole line = tunnelLine(3478);
        stays(b, line, rowOf(line, QString()), "description");
    }
    // Another program's entry on that number
    {
        Bench b;
        const Hole line = tunnelLine(3478);
        stays(b, line, {kLan, 3478, QStringLiteral("coturn"), 0}, "description");
    }
    // Our words and our tag, forwarded to another port than the one noted
    {
        Bench b;
        const Hole line = mediaLine(1, 46100);
        TableGateway::Row there = rowOf(line, kTag);
        there.internal = 48010;
        stays(b, line, there, "internal port");
    }
    // The description is compared to the letter
    {
        Bench b;
        const Hole line = tunnelLine(3478);
        TableGateway::Row there = rowOf(line, kTag);
        there.description = there.description.toUpper();
        stays(b, line, there, "description");
        Bench c;
        there = rowOf(line, kTag);
        there.description += QLatin1Char(' ');
        stays(c, line, there, "description");
    }

    SECTION("RouterSweep: nothing is removed on a doubt");
    // Nothing touched, and the line kept for the next start
    const auto kept = [](Bench& b, const Hole& line, Swept outcome) {
        const auto before = b.gw.table;
        const auto r = b.core.sweep(line);
        CHECK(r.outcome == outcome);
        CHECK(b.gw.table == before);
        CHECK(b.record.has(line.external, line.tcp));
    };
    // The router cannot say what is there
    {
        Bench b;
        const Hole line = tunnelLine(3478);
        b.left(line);
        b.gw.silent.insert(3478);
        kept(b, line, Swept::Unknown);
        CHECK_EQ(b.gw.removes, 0);
    }
    // What it says does not read as an entry
    {
        const Hole line = tunnelLine(3478);
        const auto garbled = [&](const std::function<void(TableGateway::Row&)>& spoil) {
            Bench b;
            b.left(line);
            spoil(b.gw.table[keyOf(line)]);
            kept(b, line, Swept::Unknown);
            CHECK_EQ(b.gw.removes, 0);
        };
        garbled([](TableGateway::Row& row) { row.client = QString(); });
        garbled([](TableGateway::Row& row) { row.client = QStringLiteral("192.168.1.020"); });
        garbled([](TableGateway::Row& row) { row.client = QStringLiteral("192.168.1.20 "); });
        garbled([](TableGateway::Row& row) { row.client = QStringLiteral("192.168.1"); });
        garbled([](TableGateway::Row& row) { row.client = QStringLiteral("192.168.1.256"); });
        garbled([](TableGateway::Row& row) { row.client = QStringLiteral("this-host"); });
        garbled([](TableGateway::Row& row) { row.internal = 0; });
        garbled([](TableGateway::Row& row) { row.description += QLatin1Char('\n'); });
        garbled([](TableGateway::Row& row) { row.description += QChar(0x7F); });
        garbled([](TableGateway::Row& row) { row.description = QString(200, QLatin1Char('a')); });
    }
    // The removal is refused
    {
        Bench b;
        const Hole line = tunnelLine(3478);
        b.left(line);
        b.gw.refusesRemoval = true;
        kept(b, line, Swept::Stayed);
        CHECK_EQ(b.gw.removes, 1);
    }
    // The removal is acknowledged, and the entry is still there: read back
    {
        Bench b;
        const Hole line = tunnelLine(3478);
        b.left(line);
        b.gw.keepsRemoved = true;
        kept(b, line, Swept::Stayed);
    }
    // The entry is gone already: nothing to take off, the line is done with
    {
        Bench b;
        const Hole line = tunnelLine(3478);
        b.record.note(line);
        const auto r = b.core.sweep(line);
        CHECK(r.outcome == Swept::Gone);
        CHECK_EQ(b.gw.removes, 0);
        CHECK(b.record.lines.isEmpty());
    }
    // A line noted under another address than this host has now is not
    // looked at: the router is not even asked.
    {
        Bench b;
        Hole line = tunnelLine(3478);
        line.lan = QStringLiteral("192.168.1.77");
        b.left(line);
        CHECK(b.core.sweepList().isEmpty());
        kept(b, line, Swept::OtherAddress);
        CHECK(b.gw.asked.isEmpty());
        CHECK_EQ(b.gw.removes, 0);
    }
    // A line that left the record since the list was made: nothing is asked
    {
        Bench b;
        const Hole line = tunnelLine(3478);
        b.gw.table[keyOf(line)] = rowOf(line, kTag);
        CHECK(b.core.sweep(line).outcome == Swept::Gone);
        CHECK(b.gw.asked.isEmpty());
        CHECK(b.gw.table.contains(keyOf(line)));
    }
    // Without a record there is nothing to go by
    {
        TableGateway gw;
        RouterPortCore core(gw, kAlwaysFree);
        const Hole line = tunnelLine(3478);
        gw.table[keyOf(line)] = rowOf(line, kTag);
        CHECK(core.sweepList().isEmpty());
        CHECK(core.sweep(line).outcome == Swept::Unknown);
        CHECK(gw.asked.isEmpty());
        CHECK(gw.table.contains(keyOf(line)));
    }

    SECTION("RouterSweep: UDP and TCP of one number are judged apart");
    {
        Bench b;
        const Hole ours = tunnelLine(3478, false);
        const Hole theirs = tunnelLine(3478, true);
        b.left(ours);
        // The TCP entry of that number is a neighbour's web server now
        b.record.note(theirs);
        const TableGateway::Row web{kNeighbour, 3478, QStringLiteral("web"), 0};
        b.gw.table[tcp(3478)] = web;
        CHECK(b.core.sweep(ours).outcome == Swept::Removed);
        CHECK(b.core.sweep(theirs).outcome == Swept::NotOurs);
        CHECK(!b.gw.table.contains(udp(3478)));
        CHECK(b.gw.table.value(tcp(3478)) == web);
        CHECK_EQ(b.gw.removes, 1);
    }

    SECTION("RouterSweep: the router is believed only if it answers what it is asked");
    {
        const QList<Hole> both{tunnelLine(3478), tunnelLine(3478, true)};
        const QString known = QString::number(mw::routerholes::kKnownQuestionPort);
        // No such entry, in each protocol the record has: believed
        {
            Bench b;
            CHECK(b.core.sweepBelieved(both));
            CHECK(b.gw.asked ==
                  (QStringList{kUdp + QLatin1Char(' ') + known, kTcp + QLatin1Char(' ') + known}));
        }
        {
            Bench b;
            CHECK(b.core.sweepBelieved({tunnelLine(3478)}));
            CHECK(b.gw.asked == QStringList{kUdp + QLatin1Char(' ') + known});
        }
        // Nothing to look at: nothing is asked
        {
            Bench b;
            CHECK(b.core.sweepBelieved({}));
            CHECK(b.gw.asked.isEmpty());
        }
        // A router that answers one entry whatever it is asked: our own
        {
            Bench b;
            b.gw.oneAnswer = rowOf(tunnelLine(3478), kTag);
            CHECK(!b.core.sweepBelieved(both));
        }
        // An error, a timeout
        {
            Bench b;
            b.gw.silent.insert(mw::routerholes::kKnownQuestionPort);
            CHECK(!b.core.sweepBelieved(both));
        }
        // An entry there, whoever's: not the answer that is known
        {
            Bench b;
            b.gw.table[tcp(mw::routerholes::kKnownQuestionPort)] = {kNeighbour, 80,
                                                                    QStringLiteral("web"), 0};
            CHECK(b.core.sweepBelieved({tunnelLine(3478)}));
            CHECK(!b.core.sweepBelieved(both));
        }
    }

    SECTION("RouterSweep: every write is noted before the router is asked");
    {
        Bench b;
        // At the moment of each write the record already has its line
        b.gw.beforeAdd = [&b](uint16_t ext, const QString& proto) {
            CHECK(b.record.has(ext, proto == kTcp));
        };
        const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
        CHECK(r.ok && r.claim.external == 3478 && r.claim.tcp);
        CHECK(b.record.lines == (QList<Hole>{tunnelLine(3478), tunnelLine(3478, true)}));
        // What the router's page shows ends with this installation's tag
        CHECK(b.gw.table[udp(3478)].description == QStringLiteral("MoonlightWeb tunnel a3f9c1"));
        CHECK(b.gw.table[tcp(3478)].description == QStringLiteral("MoonlightWeb tunnel a3f9c1"));
        // What this run holds is not something an earlier one left
        CHECK(b.core.sweepList().isEmpty());
        CHECK(b.core.sweep(tunnelLine(3478)).outcome == Swept::HeldAgain);
        CHECK(b.core.sweep(tunnelLine(3478, true)).outcome == Swept::HeldAgain);
        CHECK_EQ(b.gw.removes, 0);
        // A renewal writes the same lines again
        CHECK(b.core.renew().isEmpty());
        CHECK(b.record.lines == (QList<Hole>{tunnelLine(3478), tunnelLine(3478, true)}));
        // Released, and acknowledged: nothing of it is left to take back
        b.core.release(r.claim);
        CHECK(b.gw.table.isEmpty());
        CHECK(b.record.lines.isEmpty());
    }
    // A slot's hole and a shared port, each by what it is
    {
        Bench b;
        const uint16_t second = uint16_t(mw::routerports::kMediaBasePort + 1);
        const auto slot =
            b.core.claim(RouterPortCore::mediaRequest(false, false, 1, second, 46100, false));
        CHECK(slot.ok && slot.claim.external == 46100 && slot.claim.internal == second);
        CHECK(b.record.lines == (QList<Hole>{mediaLine(1, 46100), mediaLine(1, 46100, true)}));
        CHECK(b.gw.table[udp(46100)].description ==
              QStringLiteral("MoonlightWeb media slot 1 a3f9c1"));
        const auto shared =
            b.core.claim(RouterPortCore::muxRequest(false, RouterPortCore::kMuxBlock, {}));
        CHECK(shared.ok && shared.claim.external == mw::routerports::kMediaBasePort);
        Hole line;
        CHECK(b.record.find(mw::routerports::kMediaBasePort, false, line));
        CHECK(line.kind == Kind::Streams && line.slot == -1 &&
              line.internal == mw::routerports::kMediaBasePort);
        // A shared port has no TCP hole, and no TCP line
        CHECK(!b.record.has(mw::routerports::kMediaBasePort, true));
        CHECK(b.gw.table[udp(mw::routerports::kMediaBasePort)].description ==
              QStringLiteral("MoonlightWeb streams a3f9c1"));
        // Every line the policy notes is one this kind of instance may have
        for (const Hole& noted : std::as_const(b.record.lines))
            CHECK(mw::routerholes::allowed(false, noted));
    }
    // A write the router refuses leaves the record as it was
    {
        Bench b;
        b.gw.refuse.insert(3478);
        const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
        CHECK(r.ok && r.claim.external == 3479);
        CHECK(!b.record.has(3478, false) && !b.record.has(3478, true));
        CHECK(b.record.has(3479, false) && b.record.has(3479, true));
    }
    {
        Bench b;
        Hole earlier = tunnelLine(3478);
        earlier.old = true;
        b.record.note(earlier);
        b.gw.refuse.insert(3478);
        CHECK(b.core.claim(RouterPortCore::tunnelRequest(false, {})).ok);
        Hole line;
        CHECK(b.record.find(3478, false, line));
        CHECK(line == earlier);
    }
    // "Added", yet the table shows another machine: not ours, not noted
    {
        struct Repointing : TableGateway
        {
            bool add(uint16_t ext, uint16_t in, uint32_t leaseSec, const QString& description,
                     const QString& proto) override
            {
                if (ext != 3478) return TableGateway::add(ext, in, leaseSec, description, proto);
                ++adds;
                table[Key{ext, proto}] = {kNeighbour, in, description, leaseSec};
                return true;
            }
        } gw;
        MemoryRecord record;
        RouterPortCore core(gw, kAlwaysFree);
        core.setRecord(&record);
        const auto r = core.claim(RouterPortCore::tunnelRequest(false, {}));
        CHECK(r.ok && r.claim.external == 3479);
        CHECK(!record.has(3478, false));
    }
    // A hole another machine took, found at renewal: its line goes with it.
    // Nothing renews its TCP entry any more: still this host's, it is taken
    // off the router at once, and its line with it.
    {
        Bench b;
        const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
        CHECK(r.ok && r.claim.tcp);
        b.gw.keepsForeign = true;
        b.gw.table[udp(3478)].client = kNeighbour;
        CHECK_EQ(b.core.renew().size(), 1);
        CHECK(!b.record.has(3478, false));
        CHECK(!b.gw.table.contains(tcp(3478)));
        CHECK(!b.record.has(3478, true));
        CHECK_EQ(b.gw.removes, 1);
        // What the neighbour took is as it took it
        CHECK(b.gw.table[udp(3478)].client == kNeighbour);
    }
    // The same for one hole looked at now, ahead of the half hour
    {
        Bench b;
        CHECK(b.core.claim(RouterPortCore::tunnelRequest(false, {})).ok);
        b.gw.keepsForeign = true;
        b.gw.table[udp(3478)].client = kNeighbour;
        CHECK_EQ(b.core.renew(3478).size(), 1);
        CHECK(!b.gw.table.contains(tcp(3478)));
        CHECK(!b.record.has(3478, false) && !b.record.has(3478, true));
    }
    // The neighbour took the TCP number too: neither is this host's to touch.
    // The TCP line stays until a start sees whose the entry is.
    {
        Bench b;
        CHECK(b.core.claim(RouterPortCore::tunnelRequest(false, {})).ok);
        b.gw.keepsForeign = true;
        b.gw.table[udp(3478)].client = kNeighbour;
        b.gw.table[tcp(3478)].client = kNeighbour;
        CHECK_EQ(b.core.renew().size(), 1);
        CHECK_EQ(b.gw.removes, 0);
        CHECK(b.gw.table[tcp(3478)].client == kNeighbour);
        CHECK(b.record.has(3478, true));
        CHECK(b.core.sweep(tunnelLine(3478, true)).outcome == Swept::NotOurs);
        CHECK(!b.record.has(3478, true));
        CHECK(b.gw.table.contains(tcp(3478)));
    }
    // An entry at this host's address that is not the one it wrote, by its
    // text or by its port: another installation's on this machine. Not asked
    // to go, and its line kept for a start to judge.
    {
        // A router that takes a TCP write over what it has and changes nothing
        struct Stubborn : TableGateway
        {
            bool stubborn = false;
            bool add(uint16_t ext, uint16_t in, uint32_t leaseSec, const QString& description,
                     const QString& proto) override
            {
                if (stubborn && proto == kTcp && table.contains(Key{ext, proto})) {
                    ++adds;
                    return true;
                }
                return TableGateway::add(ext, in, leaseSec, description, proto);
            }
        };
        const auto lostWith = [](const std::function<void(TableGateway::Row&)>& theirs) {
            Stubborn gw;
            MemoryRecord record;
            RouterPortCore core(gw, kAlwaysFree);
            core.setRecord(&record);
            CHECK(core.claim(RouterPortCore::tunnelRequest(false, {})).ok);
            gw.keepsForeign = true;
            gw.stubborn = true;
            gw.table[udp(3478)].client = kNeighbour;
            theirs(gw.table[tcp(3478)]);
            const TableGateway::Row there = gw.table[tcp(3478)];
            CHECK_EQ(core.renew().size(), 1);
            CHECK_EQ(gw.removes, 0);
            CHECK(gw.table.value(tcp(3478)) == there);
            CHECK(record.has(3478, true));
            // The start that looks at it finds it is not this installation's
            CHECK(core.sweep(tunnelLine(3478, true)).outcome == Swept::NotOurs);
            CHECK(gw.table.value(tcp(3478)) == there);
        };
        lostWith([](TableGateway::Row& row) {
            row.description = mw::routerholes::describe(Kind::Tunnel, -1, QStringLiteral("0b77e2"));
        });
        lostWith([](TableGateway::Row& row) { row.internal = 22; });
    }
    // A removal the router does not acknowledge: the line stays for the next start
    {
        Bench b;
        CHECK(b.core.claim(RouterPortCore::tunnelRequest(false, {})).ok);
        b.gw.keepsForeign = true;
        b.gw.refusesRemoval = true;
        b.gw.table[udp(3478)].client = kNeighbour;
        CHECK_EQ(b.core.renew().size(), 1);
        CHECK(b.gw.table.contains(tcp(3478)));
        CHECK(b.record.has(3478, true));
    }
    // A shared port has no TCP entry: nothing is asked to go
    {
        Bench b;
        const auto shared =
            b.core.claim(RouterPortCore::muxRequest(false, RouterPortCore::kMuxBlock, {}));
        CHECK(shared.ok && !shared.claim.tcp);
        b.gw.keepsForeign = true;
        b.gw.table[udp(shared.claim.external)].client = kNeighbour;
        CHECK_EQ(b.core.renew().size(), 1);
        CHECK_EQ(b.gw.removes, 0);
        CHECK(b.record.lines.isEmpty());
    }
    // Without a record: the product's words alone, and nothing noted
    {
        TableGateway gw;
        RouterPortCore core(gw, kAlwaysFree);
        CHECK(core.claim(RouterPortCore::tunnelRequest(false, {})).ok);
        CHECK(gw.table[udp(3478)].description == QStringLiteral("MoonlightWeb tunnel"));
    }

    SECTION("RouterSweep: what the record says is what the router shows");
    // A leftover with another text, on a router that takes a write over its
    // own entry without changing it: written afresh, once, in each protocol.
    {
        Bench b;
        b.gw.keepsText = true;
        const Hole line = tunnelLine(3478);
        b.gw.table[udp(3478)] = rowOf(line, QString());
        b.gw.table[tcp(3478)] = rowOf(line, QString());
        const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
        CHECK(r.ok && r.claim.external == 3478 && r.claim.tcp);
        CHECK(b.gw.table[udp(3478)] == rowOf(line, kTag));
        CHECK(b.gw.table[tcp(3478)] == rowOf(line, kTag));
        CHECK_EQ(b.gw.removes, 2);
    }
    // A router that writes a text of its own: once, and no more. Nothing of
    // what it shows ever matches, so nothing of it is ever swept.
    {
        Bench b;
        b.gw.itsOwnText = QStringLiteral("upnp");
        const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
        CHECK(r.ok && r.claim.external == 3478);
        CHECK_EQ(b.gw.adds, 3);
        CHECK_EQ(b.gw.removes, 2);
        b.core.release(r.claim);
        Bench later;
        later.gw.itsOwnText = QStringLiteral("upnp");
        const Hole line = tunnelLine(3478);
        later.record.note(line);
        later.gw.table[keyOf(line)] = {kLan, 3478, QStringLiteral("upnp"), 0};
        CHECK(later.core.sweep(line).outcome == Swept::NotOurs);
        CHECK(later.gw.table.contains(keyOf(line)));
    }
    // Never at a renewal: an entry is not taken off under a stream for its text
    {
        Bench b;
        const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
        CHECK(r.ok);
        b.gw.keepsText = true;
        b.gw.table[udp(3478)].description = QStringLiteral("upnp");
        CHECK(b.core.renew().isEmpty());
        CHECK_EQ(b.gw.removes, 0);
    }
    // Without a record the description proves nothing, and is left as it is
    {
        TableGateway gw;
        gw.keepsText = true;
        gw.table[udp(3478)] = {kLan, 3478, QStringLiteral("upnp"), 0};
        RouterPortCore core(gw, kAlwaysFree);
        CHECK(core.claim(RouterPortCore::tunnelRequest(false, {})).ok);
        CHECK_EQ(gw.removes, 0);
        CHECK(gw.table[udp(3478)].description == QStringLiteral("upnp"));
    }

    SECTION("RouterSweep: an entry of a version that kept no record");
    {
        Bench b;
        Hole line = mediaLine(1, uint16_t(mw::routerports::kMediaBasePort + 1), true);
        line.old = true;
        // As such a version wrote it: the product's words alone
        b.left(line, QString());
        // Something on this machine listens on the port it leads to: it may
        // be another edition's, in use. Left, and judged again next time.
        b.answer = Listens::Yes;
        auto r = b.core.sweep(line);
        CHECK(r.outcome == Swept::InUse && r.detail == QLatin1String("listened"));
        CHECK(b.gw.table.contains(keyOf(line)));
        Hole still;
        CHECK(b.record.find(line.external, true, still) && still.old);
        CHECK(b.listenAsked == (QList<QPair<uint16_t, bool>>{{line.internal, true}}));
        // The system does not say: the same
        b.answer = Listens::Unknown;
        r = b.core.sweep(line);
        CHECK(r.outcome == Swept::InUse && r.detail == QLatin1String("unknown"));
        CHECK(b.gw.table.contains(keyOf(line)));
        CHECK_EQ(b.gw.removes, 0);
        // Nothing listens there: it leads nowhere, and is taken back
        b.answer = Listens::No;
        CHECK(b.core.sweep(line).outcome == Swept::Removed);
        CHECK(b.gw.table.isEmpty() && b.record.lines.isEmpty());
    }
    // Nobody to ask about the machine's sockets: nothing is known
    {
        TableGateway gw;
        MemoryRecord record;
        RouterPortCore core(gw, kAlwaysFree);
        core.setRecord(&record);
        Hole line = tunnelLine(3478);
        line.old = true;
        record.note(line);
        gw.table[keyOf(line)] = rowOf(line, QString());
        CHECK(core.sweep(line).outcome == Swept::InUse);
        CHECK(gw.table.contains(keyOf(line)));
    }
    // The other conditions hold for such a line too, with the words alone
    {
        Bench b;
        Hole line = tunnelLine(3478);
        line.old = true;
        b.record.note(line);
        // An entry with a tag is some installation's of today, not that version's
        b.gw.table[keyOf(line)] = rowOf(line, kTag);
        const auto r = b.core.sweep(line);
        CHECK(r.outcome == Swept::NotOurs && r.detail == QLatin1String("description"));
        CHECK(b.gw.table.contains(keyOf(line)));
        CHECK(b.listenAsked.isEmpty());
    }
    {
        Bench b;
        Hole line = tunnelLine(3478);
        line.old = true;
        b.record.note(line);
        TableGateway::Row there = rowOf(line, QString());
        there.client = kNeighbour;
        b.gw.table[keyOf(line)] = there;
        CHECK(b.core.sweep(line).outcome == Swept::NotOurs);
        CHECK(b.gw.table.value(keyOf(line)) == there);
    }
    // A claim on that number writes the entry anew: the line is of today
    {
        Bench b;
        Hole line = tunnelLine(3478);
        line.old = true;
        b.left(line, QString());
        CHECK(b.core.claim(RouterPortCore::tunnelRequest(false, {})).ok);
        Hole now;
        CHECK(b.record.find(3478, false, now) && !now.old);
        CHECK(b.gw.table[udp(3478)] == rowOf(tunnelLine(3478), kTag));
    }

    SECTION("RouterSweep: a run takes back what a run of its own kind left, and no other");
    // A DEV identity under its bench key walks the installed host's ports
    // (PortIdentity.h), on the record it keeps without the key. Killed as
    // one kind and started as the other, it leaves the first kind's entries
    // where they are, and keeps the note of them for the run that may act.
    for (const bool killedUnderKey : {true, false}) {
        using mw::routerholes::File;
        QTemporaryDir dir;
        CHECK(dir.isValid());
        const QByteArray machine = QByteArrayLiteral("machine-one");
        const mw::routerholes::Earlier none;
        QString said;
        // What the instance walks: the DEV lists unless under the key
        const bool killedAsDev = !killedUnderKey;
        const uint16_t devTunnel = mw::routerports::kDevTunnelPoolBegin;
        Hole streamsLine = tunnelLine(killedAsDev ? 48573 : 3479);
        streamsLine.kind = Kind::Streams;
        const QList<Hole> left{tunnelLine(killedAsDev ? devTunnel : 3478),
                               tunnelLine(killedAsDev ? devTunnel : 3478, true), streamsLine};
        TableGateway gw;
        QString tag;
        // The run that is killed: its lines noted, its entries on the router
        {
            auto record = File::open(dir.path(), killedAsDev, machine, none, kLan, &said);
            CHECK(record != nullptr);
            if (!record) continue;
            tag = record->tag();
            for (const Hole& line : left) {
                record->note(line);
                gw.table[keyOf(line)] = rowOf(line, tag);
            }
        }
        const auto before = gw.table;

        // Started as the other kind: nothing of it to sweep, nothing removed,
        // and a line of its own written beside what is kept
        const Hole own = tunnelLine(killedAsDev ? 5349 : static_cast<uint16_t>(devTunnel + 1));
        {
            auto record = File::open(dir.path(), !killedAsDev, machine, none, kLan, &said);
            CHECK(record != nullptr);
            if (!record) continue;
            CHECK(record->tag() == tag);
            RouterPortCore core(gw, kAlwaysFree);
            core.setRecord(record.get());
            CHECK(core.sweepList().isEmpty());
            CHECK(record->holes().isEmpty());
            for (const Hole& line : left)
                CHECK(core.sweep(line).outcome == Swept::Gone);
            CHECK_EQ(gw.removes, 0);
            CHECK(gw.table == before);
            record->note(own);
            gw.table[keyOf(own)] = rowOf(own, tag);
        }

        // Started as the kind that was killed: its entries, each of them,
        // and not the one the other kind left meanwhile
        {
            auto record = File::open(dir.path(), killedAsDev, machine, none, kLan, &said);
            CHECK(record != nullptr);
            if (!record) continue;
            RouterPortCore core(gw, kAlwaysFree);
            core.setRecord(record.get());
            const QList<Hole> list = core.sweepList();
            CHECK(list == left);
            CHECK(core.sweepBelieved(list));
            for (const Hole& line : list)
                CHECK(core.sweep(line).outcome == Swept::Removed);
            CHECK_EQ(gw.removes, static_cast<int>(left.size()));
            CHECK(gw.table.size() == 1 && gw.table.value(keyOf(own)) == rowOf(own, tag));
            CHECK(record->holes().isEmpty());
        }
        // …which the other kind takes back in its turn
        {
            auto record = File::open(dir.path(), !killedAsDev, machine, none, kLan, &said);
            CHECK(record != nullptr);
            if (!record) continue;
            RouterPortCore core(gw, kAlwaysFree);
            core.setRecord(record.get());
            CHECK(core.sweepList() == QList<Hole>{own});
            CHECK(core.sweep(own).outcome == Swept::Removed);
            CHECK(gw.table.isEmpty());
        }
    }
}
