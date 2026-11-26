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

#include <QDebug>
#include <QJsonObject>
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
    /// Refuses to remove these entries only.
    QList<QPair<uint16_t, QString>> keeps;
    /// MiniUPnPd up to 1.9: a write over an entry of the same client and port
    /// succeeds and changes nothing, description included.
    bool keepsText = false;
    /// A router that writes a text of its own, whatever it is given.
    QString itsOwnText;
    /// A Livebox: a write over another machine's entry succeeds and the
    /// table keeps pointing at that machine.
    bool keepsForeign = false;
    /// A Freebox: a write over an entry towards another client is refused
    /// (ConflictInMappingEntry, 718), and the entry stays as it was.
    bool conflictsOverClient = false;
    QSet<uint16_t> refuse;
    int adds = 0;
    int removes = 0;
    QStringList asked;
    std::function<void(uint16_t, const QString&)> beforeAdd;
    /// The address the system would send from towards the router now: set
    /// when a DHCP lease renumbers this machine. Empty: `lan`.
    QString now;

    QString lanAddress() const override { return lan; }
    QString addressNow() const override { return now.isEmpty() ? lan : now; }

    bool add(uint16_t ext, uint16_t in, uint32_t leaseSec, const QString& description,
             const QString& proto) override
    {
        if (beforeAdd) beforeAdd(ext, proto);
        ++adds;
        if (refuse.contains(ext)) return false;
        const Key key{ext, proto};
        if (table.contains(key)) {
            const Row& there = table[key];
            if (conflictsOverClient && there.client != lan) return false;
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
        if (refusesRemoval || keeps.contains(Key{ext, proto})) return false;
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
    /// Whose record: another installation on the same machine has its own.
    QString itsTag = kTag;

    QString tag() const override { return itsTag; }
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
    /// Whether another MoonlightWeb server runs here: `servers` first, one
    /// answer each time it is asked, then `server`.
    Listens server = Listens::No;
    QList<Listens> servers;
    int serverAsked = 0;
    /// The router looked for again from the address of now: it answers, and
    /// sees this machine there, unless told otherwise.
    int rediscovers = 0;
    bool rediscoverFails = false;

    Bench()
        : core(gw, kAlwaysFree)
    {
        core.setRecord(&record);
        core.setRediscover([this]() {
            ++rediscovers;
            if (rediscoverFails) return false;
            gw.lan = gw.addressNow();
            gw.now.clear();
            return true;
        });
        core.setListens([this](uint16_t port, bool isTcp) {
            listenAsked.append({port, isTcp});
            return answer;
        });
        core.setOtherServer([this]() {
            ++serverAsked;
            return servers.isEmpty() ? server : servers.takeFirst();
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

static void run_router_former_address_tests();

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
    // The entry on TCP 443 of a run that was killed (settings.json
    // ice_tcp_443, Tcp443Guard): the router's 443 towards the streams' shared
    // port. Taken back at the next start as any other line is, under the same
    // address, and by the same rules.
    {
        Hole line;
        line.external = 443;
        line.internal = 48550;
        line.tcp = true;
        line.kind = Kind::Tcp443;
        line.slot = -1;
        line.lan = kLan;
        Bench b;
        b.left(line);
        CHECK(b.core.sweepList() == QList<Hole>{line});
        CHECK(b.core.sweepBelieved(b.core.sweepList()));
        CHECK(b.core.sweep(line).outcome == Swept::Removed);
        CHECK(b.gw.table.isEmpty() && b.record.lines.isEmpty());
        CHECK_EQ(b.gw.removes, 1);
        // Somebody's own rule on 443, towards this very machine and port, in
        // other words than this installation's: left, and no longer noted
        Bench other;
        other.record.note(line);
        other.gw.table[keyOf(line)] = {kLan, 48550, QStringLiteral("https"), 0};
        CHECK(other.core.sweep(line).outcome == Swept::NotOurs);
        CHECK(other.gw.removes == 0 && other.gw.table.size() == 1);
        // A router that is not believed about a port nobody maps: left
        Bench liar;
        liar.left(line);
        liar.gw.oneAnswer = rowOf(line, kTag);
        CHECK(!liar.core.sweepBelieved(liar.core.sweepList()));
        // Noted under another address than today's: not this sweep's to look
        // at. That one has a rule of its own (Tcp443Guard::formerAddress).
        Hole old = line;
        old.lan = kNeighbour;
        Bench former;
        former.left(old);
        CHECK(former.core.sweepList().isEmpty());
        CHECK(former.core.sweep(old).outcome == Swept::OtherAddress);
        CHECK(former.gw.removes == 0 && former.record.has(443, true));
    }

    // The shared ports' lines of a run that had them, at a start without them
    // (the admin page's box unticked, applied by a restart): nothing claims
    // them, so the sweep takes every one back and nothing is added
    // (UI443, security review (2)).
    {
        Bench b;
        const uint16_t block = mw::routerports::kMediaBasePort;
        QList<Hole> lines;
        for (const uint16_t port : {uint16_t(3478), block})
            for (const bool isTcp : {false, true}) {
                Hole line;
                line.external = port;
                line.internal = port;
                line.tcp = isTcp;
                line.kind = Kind::Streams;
                line.slot = -1;
                line.lan = kLan;
                b.left(line);
                lines.append(line);
            }
        const QList<Hole> listed = b.core.sweepList();
        CHECK_EQ(int(listed.size()), int(lines.size()));
        CHECK(b.core.sweepBelieved(listed));
        for (const Hole& line : listed)
            CHECK(b.core.sweep(line).outcome == Swept::Removed);
        CHECK(b.gw.table.isEmpty() && b.record.lines.isEmpty());
        CHECK_EQ(b.gw.removes, int(lines.size()));
        CHECK_EQ(b.gw.adds, 0);
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
    // A line noted under another address than this host has now is looked at
    // all the same, by the rule of the former address (formerOurs); the
    // RouterSweep OLDADDR tests go through that rule. One of a version that
    // kept no record is not: an untagged entry proves nothing at another
    // address, and the router is not even asked.
    {
        Bench b;
        Hole line = tunnelLine(3478);
        line.lan = QStringLiteral("192.168.1.77");
        b.left(line);
        CHECK(b.core.sweepList() == QList<Hole>{line});
        const auto r = b.core.sweep(line);
        CHECK(r.outcome == Swept::RemovedFormer && r.former);
        CHECK(b.gw.table.isEmpty() && b.record.lines.isEmpty());
    }
    {
        Bench b;
        Hole line = tunnelLine(3478);
        line.lan = QStringLiteral("192.168.1.77");
        line.old = true;
        b.left(line, QString());
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
        // Its TCP hole alongside, and its line
        CHECK(shared.claim.tcp && b.record.has(mw::routerports::kMediaBasePort, true));
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
    // A hole asked for in UDP alone has no TCP entry: nothing is asked to go
    {
        Bench b;
        auto udpAlone = RouterPortCore::muxRequest(false, RouterPortCore::kMuxBlock, {});
        udpAlone.tcp = false;
        const auto shared = b.core.claim(udpAlone);
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
    // A leftover of an earlier version, with its words and no tag, on a router
    // that takes a write over its own entry without changing it: written
    // afresh, once, in each protocol.
    {
        Bench b;
        b.gw.keepsText = true;
        const Hole line = tunnelLine(3478);
        for (const bool isTcp : {false, true}) {
            Hole earlier = tunnelLine(3478, isTcp);
            earlier.old = true;
            b.record.note(earlier);
            b.gw.table[keyOf(earlier)] = rowOf(line, QString());
        }
        const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
        CHECK(r.ok && r.claim.external == 3478 && r.claim.tcp);
        CHECK(b.gw.table[udp(3478)] == rowOf(line, kTag));
        CHECK(b.gw.table[tcp(3478)] == rowOf(line, kTag));
        CHECK_EQ(b.gw.removes, 2);
    }
    // The same words with no line of an earlier version in the record: some
    // other edition's on this machine, passed over and left as it is
    {
        Bench b;
        const Hole line = tunnelLine(3478);
        b.gw.table[udp(3478)] = rowOf(line, QString());
        const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
        CHECK(r.ok && r.claim.external == 3479);
        CHECK(r.skipped.size() == 1 &&
              r.skipped.first().reason == RouterPortCore::SkipReason::OtherInstallation);
        CHECK(b.gw.table[udp(3478)] == rowOf(line, QString()));
        CHECK(!b.record.has(3478, false));
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
    // Without a record nothing proves an entry at this address this host's:
    // passed over, and left as it is
    {
        TableGateway gw;
        gw.keepsText = true;
        gw.table[udp(3478)] = {kLan, 3478, QStringLiteral("upnp"), 0};
        RouterPortCore core(gw, kAlwaysFree);
        const auto r = core.claim(RouterPortCore::tunnelRequest(false, {}));
        CHECK(r.ok && r.claim.external == 3479);
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
        Hole streamsLine = tunnelLine(killedAsDev ? 48597 : 3479);
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

    SECTION("RouterSweep: another installation on this machine, by its tag alone");
    // Two editions on one machine are one address to the router. Another's
    // entry there, under its tag or under no tag, is never written over,
    // renewed over or taken off; this installation's is, under its tag.
    const QString kOtherTag = QStringLiteral("0b77e2");
    // At a claim: another's passed over and said without an address, this
    // installation's own taken back
    {
        Bench b;
        const TableGateway::Row theirs = rowOf(tunnelLine(3478), kOtherTag);
        b.gw.table[udp(3478)] = theirs;
        b.gw.table[tcp(3478)] = theirs;
        b.gw.table[udp(3479)] = rowOf(tunnelLine(3479), kTag);
        const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {3478, 3479}));
        CHECK(r.ok && r.claim.external == 3479 && r.claim.tcp);
        CHECK(r.skipped.size() == 1 && r.skipped.first().port == 3478);
        CHECK(r.skipped.first().reason == RouterPortCore::SkipReason::OtherInstallation);
        CHECK(r.skipped.first().owner.isEmpty());
        CHECK(b.gw.table.value(udp(3478)) == theirs && b.gw.table.value(tcp(3478)) == theirs);
        CHECK(b.gw.table.value(udp(3479)) == rowOf(tunnelLine(3479), kTag));
        CHECK(!b.record.has(3478, false) && !b.record.has(3478, true));
        CHECK(b.record.has(3479, false) && b.record.has(3479, true));
    }
    // Its TCP entry alone on a number that is free in UDP: the UDP hole is
    // this host's, the TCP one stays the other's
    {
        Bench b;
        const TableGateway::Row theirs = rowOf(tunnelLine(3478, true), kOtherTag);
        b.gw.table[tcp(3478)] = theirs;
        const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
        CHECK(r.ok && r.claim.external == 3478 && !r.claim.tcp);
        CHECK(b.gw.table.value(tcp(3478)) == theirs);
        CHECK(!b.record.has(3478, true));
    }
    // At the address this host had before a renumbering: its own by its
    // record AND its tag, another machine's under any other words. Its own is
    // taken off first, then written here (the RouterSweep OLDADDR tests say
    // more of it).
    {
        Bench b;
        const QString before = QStringLiteral("192.168.1.77");
        Hole noted = tunnelLine(3478);
        noted.lan = before;
        b.left(noted);
        TableGateway::Row other = rowOf(tunnelLine(3479), QString());
        other.client = before;
        b.gw.table[udp(3479)] = other;
        const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {3479, 3478}));
        CHECK(r.ok && r.claim.external == 3478 && r.reclaimedFrom == before);
        CHECK(r.skipped.size() == 1 && r.skipped.first().port == 3479);
        CHECK(r.skipped.first().reason == RouterPortCore::SkipReason::Foreign);
        CHECK(b.gw.table.value(udp(3478)).client == kLan);
        CHECK(b.gw.table.value(udp(3479)) == other);
    }
    // At a renewal: an entry another installation here has written over is
    // lost, and not written back over; the TCP entry this host wrote goes
    {
        Bench b;
        const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
        CHECK(r.ok && r.claim.tcp);
        const TableGateway::Row theirs = rowOf(tunnelLine(3478), kOtherTag);
        b.gw.table[udp(3478)] = theirs;
        const int adds = b.gw.adds;
        const auto lost = b.core.renew();
        CHECK(lost.size() == 1 && lost.first().here && lost.first().owner == kLan);
        CHECK_EQ(b.gw.adds, adds);
        CHECK(b.gw.table.value(udp(3478)) == theirs);
        CHECK(!b.gw.table.contains(tcp(3478)));
        CHECK(!b.record.has(3478, false) && !b.record.has(3478, true));
        CHECK(!b.core.holds(3478));
    }
    // The same for one looked at now, written over by an edition of before
    // the tags (its words alone)
    {
        using mw::routerports::kMediaBasePort;
        Bench b;
        CHECK(b.core.claim(RouterPortCore::mediaRequest(false, false, 0, kMediaBasePort, 0, false))
                  .ok);
        const TableGateway::Row then = rowOf(mediaLine(0, kMediaBasePort), QString());
        b.gw.table[udp(kMediaBasePort)] = then;
        const int adds = b.gw.adds;
        const auto lost = b.core.renew(kMediaBasePort);
        CHECK(lost.size() == 1 && lost.first().here);
        CHECK_EQ(b.gw.adds, adds);
        CHECK(b.gw.table.value(udp(kMediaBasePort)) == then);
    }
    // To another port of this machine than the one this host binds: lost too
    {
        Bench b;
        CHECK(b.core.claim(RouterPortCore::tunnelRequest(false, {})).ok);
        b.gw.table[udp(3478)].internal = 3479;
        const auto lost = b.core.renew();
        CHECK(lost.size() == 1 && lost.first().here);
        CHECK_EQ(b.gw.table.value(udp(3478)).internal, uint16_t(3479));
    }
    // At a release, and before the process quits: another's is left (and not
    // counted as left behind), this host's own is taken off
    {
        using mw::routerports::kMediaBasePort;
        Bench b;
        const auto t = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
        const auto m = b.core.claim(
            RouterPortCore::mediaRequest(false, false, 1, uint16_t(kMediaBasePort + 1), 0, false));
        CHECK(t.ok && t.claim.tcp && m.ok && m.claim.tcp);
        const TableGateway::Row theirs = rowOf(tunnelLine(3478), kOtherTag);
        b.gw.table[udp(3478)] = theirs;
        b.gw.removes = 0;
        b.core.release(t.claim);
        CHECK(b.gw.table.value(udp(3478)) == theirs);
        CHECK(!b.gw.table.contains(tcp(3478)));
        CHECK_EQ(b.gw.removes, 1);
        CHECK(!b.record.has(3478, false) && !b.record.has(3478, true));
        const uint16_t slotPort = m.claim.external;
        const TableGateway::Row theirsToo = rowOf(mediaLine(1, slotPort), kOtherTag);
        b.gw.table[udp(slotPort)] = theirsToo;
        const auto out = b.core.releaseBeforeExit({});
        CHECK(out.released == 1 && out.left.isEmpty());
        CHECK(b.gw.table.value(udp(slotPort)) == theirsToo);
        CHECK(!b.gw.table.contains(tcp(slotPort)));
        CHECK(b.record.lines.isEmpty());
    }
    // Two installations of one identity on one machine (a DEV under its
    // bench key beside the installed host): the same numbers, told apart by
    // their tags alone. Neither takes, renews over or removes the other's.
    {
        TableGateway gw;
        MemoryRecord mine;
        MemoryRecord theirs;
        theirs.itsTag = kOtherTag;
        RouterPortCore a(gw, kAlwaysFree);
        RouterPortCore b(gw, kAlwaysFree);
        a.setRecord(&mine);
        b.setRecord(&theirs);
        const auto first = a.claim(RouterPortCore::tunnelRequest(false, {}));
        const auto second = b.claim(RouterPortCore::tunnelRequest(false, {3478}));
        CHECK(first.ok && first.claim.external == 3478);
        CHECK(second.ok && second.claim.external == 3479);
        const auto before = gw.table;
        CHECK(a.renew().isEmpty() && b.renew().isEmpty());
        CHECK(gw.table == before);
        CHECK(b.renew(3478).isEmpty());
        CHECK(gw.table == before);
        b.releaseAll();
        CHECK(gw.table.value(udp(3478)) == rowOf(tunnelLine(3478), kTag));
        CHECK(gw.table.value(tcp(3478)) == rowOf(tunnelLine(3478, true), kTag));
        CHECK(!gw.table.contains(udp(3479)) && !gw.table.contains(tcp(3479)));
        CHECK(mine.has(3478, false) && mine.has(3478, true) && theirs.lines.isEmpty());
        // A start of the other one that finds a line of its record on a
        // number this one holds now: not its own, left
        Hole was = tunnelLine(3478);
        theirs.note(was);
        CHECK(b.sweep(was).outcome == Swept::NotOurs);
        CHECK(gw.table.value(udp(3478)) == rowOf(tunnelLine(3478), kTag));
        a.releaseAll();
        CHECK(gw.table.isEmpty());
    }

    SECTION("RouterSweep: an entry of an earlier version, taken over by a claim");
    // A stream slot's entry of a version that put no tag: every edition then
    // wrote those words to those ports. Taken over only by the installed
    // host, from a line of its own record, while nothing listens on its port
    // and no other MoonlightWeb server runs here.
    {
        using mw::routerports::kMediaBasePort;
        using mw::routerports::kMediaPoolBegin;
        Hole line = mediaLine(0, kMediaPoolBegin);
        line.old = true;
        const auto request =
            RouterPortCore::mediaRequest(false, false, 0, kMediaBasePort, kMediaPoolBegin, false);
        const auto claimWith = [&](Listens listens, Listens server, bool dev, bool noted,
                                   TableGateway::Row* after) {
            Bench b;
            b.answer = listens;
            b.server = server;
            b.core.setDevInstance(dev);
            if (noted) b.record.note(line);
            b.gw.table[udp(kMediaPoolBegin)] = rowOf(line, QString());
            const auto r = b.core.claim(request);
            *after = b.gw.table.value(udp(kMediaPoolBegin));
            return r;
        };
        TableGateway::Row after;
        const TableGateway::Row then = rowOf(line, QString());
        // All of it holds: taken over, and written with this installation's tag
        auto r = claimWith(Listens::No, Listens::No, false, true, &after);
        CHECK(r.ok && r.claim.external == kMediaPoolBegin && r.claim.internal == kMediaBasePort);
        CHECK(after == rowOf(mediaLine(0, kMediaPoolBegin), kTag));
        // Anything else: passed over, left as it is, the slot's own number taken
        const auto passedOver = [&](Listens listens, Listens server, bool dev, bool noted) {
            const auto res = claimWith(listens, server, dev, noted, &after);
            CHECK(res.ok && res.claim.external == kMediaBasePort);
            CHECK(!res.skipped.isEmpty() &&
                  res.skipped.first().reason == RouterPortCore::SkipReason::OtherInstallation);
            CHECK(after == then);
        };
        passedOver(Listens::Yes, Listens::No, false, true);
        passedOver(Listens::Unknown, Listens::No, false, true);
        passedOver(Listens::No, Listens::Yes, false, true);
        passedOver(Listens::No, Listens::Unknown, false, true);
        // A DEV identity never takes one over
        passedOver(Listens::No, Listens::No, true, true);
        // No line of an earlier version in the record
        passedOver(Listens::No, Listens::No, false, false);
    }

    SECTION("RouterSweep: an earlier version's stream slot entry, and another server here");
    {
        using mw::routerports::kMediaPoolBegin;
        Hole line = mediaLine(2, kMediaPoolBegin, true);
        line.old = true;
        // Another MoonlightWeb server runs here, or the system does not say:
        // left as it is, the router not even asked, judged again next start
        for (const Listens server : {Listens::Yes, Listens::Unknown}) {
            Bench b;
            b.left(line, QString());
            b.server = server;
            const auto r = b.core.sweep(line);
            CHECK(r.outcome == Swept::InUse);
            CHECK(r.detail == (server == Listens::Yes ? QLatin1String("server")
                                                      : QLatin1String("server unknown")));
            CHECK(b.gw.table.contains(keyOf(line)) && b.record.has(line.external, true));
            CHECK(b.gw.asked.isEmpty());
            CHECK_EQ(b.gw.removes, 0);
        }
        // None when first asked, one by the time of the removal: left
        {
            Bench b;
            b.left(line, QString());
            b.servers = {Listens::No, Listens::Yes};
            const auto r = b.core.sweep(line);
            CHECK(r.outcome == Swept::InUse && r.detail == QLatin1String("server"));
            CHECK_EQ(b.serverAsked, 2);
            CHECK(b.gw.table.contains(keyOf(line)) && b.record.has(line.external, true));
            CHECK_EQ(b.gw.removes, 0);
        }
        // None either time, and nothing listening: taken back
        {
            Bench b;
            b.left(line, QString());
            CHECK(b.core.sweep(line).outcome == Swept::Removed);
            CHECK_EQ(b.serverAsked, 2);
            CHECK(b.gw.table.isEmpty() && b.record.lines.isEmpty());
        }
        // Nobody to ask: nothing is known, nothing removed
        {
            TableGateway gw;
            MemoryRecord record;
            RouterPortCore core(gw, kAlwaysFree);
            core.setRecord(&record);
            core.setListens([](uint16_t, bool) { return Listens::No; });
            record.note(line);
            gw.table[keyOf(line)] = rowOf(line, QString());
            const auto r = core.sweep(line);
            CHECK(r.outcome == Swept::InUse && r.detail == QLatin1String("server unknown"));
            CHECK(gw.table.contains(keyOf(line)));
        }
        // Still there after its removal: said, kept for the next start
        {
            Bench b;
            b.left(line, QString());
            b.gw.keepsRemoved = true;
            CHECK(b.core.sweep(line).outcome == Swept::Stayed);
            CHECK(b.record.has(line.external, true));
            CHECK_EQ(b.gw.removes, 1);
        }
        // The tunnel's entries of then were each in a block of its edition's
        // own, and a tagged line is this installation's: neither asks
        {
            Bench b;
            Hole tunnelThen = tunnelLine(3478);
            tunnelThen.old = true;
            b.left(tunnelThen, QString());
            const Hole tagged = mediaLine(1, uint16_t(kMediaPoolBegin + 1));
            b.left(tagged);
            b.server = Listens::Yes;
            CHECK(b.core.sweep(tunnelThen).outcome == Swept::Removed);
            CHECK(b.core.sweep(tagged).outcome == Swept::Removed);
            CHECK_EQ(b.serverAsked, 0);
        }
    }

    SECTION("RouterSweep: what a DEV wrote while it bound the installed host's block");
    // A DEV identity binds a block of its own now (48574-48597). What it
    // wrote on the router while it bound the installed host's is its own
    // still, taken back at its next start: with its tag as any line, its
    // shared port at the top of that block included; without one (up to
    // 0.4.3), under the rules of an earlier version's stream slot entry.
    [&]() {
        using mw::routerholes::File;
        using namespace mw::routerports;
        QTemporaryDir dir;
        CHECK(dir.isValid());
        const QByteArray machine = QByteArrayLiteral("machine-one");
        const mw::routerholes::Earlier none;
        QString said;
        TableGateway gw;
        Hole shared = tunnelLine(uint16_t(kMediaBasePort + kMediaPortCount - 1));
        shared.kind = Kind::Streams;
        const Hole slot = mediaLine(0, kMediaPoolBegin);
        const QList<Hole> left{shared, slot};
        // The DEV that was killed, before it had its block
        {
            auto record = File::open(dir.path(), true, machine, none, kLan, &said);
            CHECK(record != nullptr);
            if (!record) return;
            for (const Hole& line : left) {
                record->note(line);
                gw.table[keyOf(line)] = rowOf(line, record->tag());
            }
        }
        // The DEV of now, at its next start: both are its own, and go
        {
            auto record = File::open(dir.path(), true, machine, none, kLan, &said);
            CHECK(record != nullptr);
            if (!record) return;
            CHECK(record->holes() == left);
            RouterPortCore core(gw, kAlwaysFree);
            core.setRecord(record.get());
            core.setDevInstance(true);
            const QList<Hole> list = core.sweepList();
            CHECK(list == left);
            CHECK(core.sweepBelieved(list));
            for (const Hole& line : list)
                CHECK(core.sweep(line).outcome == Swept::Removed);
            CHECK(gw.table.isEmpty());
            CHECK(record->holes().isEmpty());
        }
    }();
    // Without a tag: a DEV up to 0.4.3's, from the numbers it remembers
    {
        using namespace mw::routerports;
        mw::routerholes::Earlier earlier;
        earlier.lan = kLan;
        earlier.media = {{0, kMediaPoolBegin}};
        const QList<Hole> lines = mw::routerholes::earlierHoles(true, earlier, kLan);
        CHECK_EQ(lines.size(), 2);
        for (const Listens server : {Listens::Yes, Listens::Unknown, Listens::No}) {
            Bench b;
            b.core.setDevInstance(true);
            b.server = server;
            for (const Hole& line : lines)
                b.left(line, QString());
            CHECK(b.core.sweepList() == lines);
            for (const Hole& line : lines)
                CHECK(b.core.sweep(line).outcome ==
                      (server == Listens::No ? Swept::Removed : Swept::InUse));
            CHECK(b.gw.table.isEmpty() == (server == Listens::No));
        }
        // Something listens on its port in TCP (a release of then streaming
        // over TCP): that one stays
        {
            Bench b;
            b.core.setDevInstance(true);
            for (const Hole& line : lines)
                b.left(line, QString());
            b.answer = Listens::Yes;
            for (const Hole& line : lines)
                CHECK(b.core.sweep(line).outcome == Swept::InUse);
            CHECK_EQ(b.gw.table.size(), 2);
        }
    }

    SECTION("RouterSweep: what the list leaves out is counted, in numbers only");
    {
        using Count = RouterPortCore::SweepCount;
        const auto same = [](const Count& a, int lines, int elsewhere, int held) {
            return a.lines == lines && a.elsewhere == elsewhere && a.held == held;
        };
        // No record: nothing, and nothing counted
        {
            TableGateway gw;
            RouterPortCore core(gw, kAlwaysFree);
            Count count{7, 7, 7};
            CHECK(core.sweepList(&count).isEmpty());
            CHECK(same(count, 0, 0, 0));
        }
        // Every line looked at
        {
            Bench b;
            b.left(tunnelLine(3479));
            b.left(mediaLine(1, 46101));
            Count count;
            CHECK_EQ(b.core.sweepList(&count).size(), 2);
            CHECK(same(count, 2, 0, 0));
        }
        // Two under another address of this machine, of which the tagged one
        // is looked at, after the others; two of a port this run holds
        {
            Bench b;
            Hole before = tunnelLine(3480);
            before.lan = QStringLiteral("192.168.1.21");
            b.left(before);
            Hole untagged = tunnelLine(3481);
            untagged.lan = QStringLiteral("192.168.1.21");
            untagged.old = true;
            b.left(untagged, QString());
            b.left(mediaLine(2, 46102));
            const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
            CHECK(r.ok && r.claim.external == 3478);
            Count count;
            const QList<Hole> list = b.core.sweepList(&count);
            CHECK((list == QList<Hole>{mediaLine(2, 46102), before}));
            CHECK(same(count, 5, 2, 2));
            CHECK_EQ(count.former, 1);
            CHECK_EQ(count.lines - count.elsewhere + count.former - count.held, list.size());
            // Without the count asked for, the same list
            CHECK(b.core.sweepList() == list);
        }
    }

    run_router_former_address_tests();
}

// ── OLDADDR: what this installation left under an address it had before ─────
//
// Seen on a Freebox: the host killed, its DHCP address changed, the host
// started again. Its entries stayed towards the former address, for good (the
// Freebox keeps no lease), and a write of the same number towards the new
// address was refused (718, ConflictInMappingEntry): the numbers were lost.

namespace {

const QString kFormerLan = QStringLiteral("192.168.1.77");
const QString kNewLan = QStringLiteral("192.168.1.30");
const QString kTheirTag = QStringLiteral("5d01aa");

Hole under(Hole line, const QString& lan, bool isTcp)
{
    line.lan = lan;
    line.tcp = isTcp;
    return line;
}

Hole streamsLine(uint16_t port)
{
    Hole line;
    line.external = port;
    line.internal = port;
    line.kind = Kind::Streams;
    line.slot = -1;
    line.lan = kLan;
    return line;
}

/// Each kind of hole a claim asks for, with the line it notes.
struct Kinded
{
    const char* name;
    Hole line;
    RouterPortCore::Request request;
};

QList<Kinded> everyKind()
{
    return {
        {"tunnel", tunnelLine(3478), RouterPortCore::tunnelRequest(false, {3478})},
        {"media slot", mediaLine(1, 46101),
         RouterPortCore::mediaRequest(false, false, 1, 48551, 46101, false)},
        {"shared conferencing port", streamsLine(3478),
         RouterPortCore::muxRequest(false, RouterPortCore::kMuxStandard, {3478})},
        {"shared block port", streamsLine(48550),
         RouterPortCore::muxRequest(false, RouterPortCore::kMuxBlock, {48550})},
    };
}

QString protocolOf(bool isTcp)
{
    return isTcp ? kTcp : kUdp;
}

} // namespace

static void run_router_former_address_tests()
{
    using Reason = RouterPortCore::SkipReason;

    SECTION("RouterSweep OLDADDR: a claim takes its number back from this host's old address");
    for (const Kinded& k : everyKind()) {
        Bench b;
        b.gw.conflictsOverClient = true;
        b.left(under(k.line, kFormerLan, false));
        b.left(under(k.line, kFormerLan, true));
        const auto r = b.core.claim(k.request);
        CHECK(r.ok && r.claim.external == k.line.external && r.claim.tcp);
        CHECK(r.reclaimedFrom == kFormerLan && r.skipped.isEmpty());
        // Both protocols written here, and noted under the address of now
        for (const bool isTcp : {false, true}) {
            const TableGateway::Row row = b.gw.table.value({k.line.external, protocolOf(isTcp)});
            CHECK(row.client == kLan && row.internal == k.line.internal);
            CHECK(row.description == mw::routerholes::describe(k.line.kind, k.line.slot, kTag));
            Hole noted;
            CHECK(b.record.find(k.line.external, isTcp, noted) && noted.lan == kLan);
        }
        CHECK_EQ(b.gw.removes, 2);
        CHECK(!b.core.formerStays());
        if (!(r.ok && r.reclaimedFrom == kFormerLan)) qWarning() << "kind:" << k.name;
    }
    // The router keeps the entry towards the old address: refused, or "removed"
    // and still there. The number is left, as another's is, and so is the line.
    for (const bool saysRemoved : {false, true}) {
        Bench b;
        b.gw.conflictsOverClient = true;
        (saysRemoved ? b.gw.keepsRemoved : b.gw.refusesRemoval) = true;
        const Hole line = under(tunnelLine(3478), kFormerLan, false);
        b.left(line);
        const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {3478}));
        CHECK(r.ok && r.claim.external == 3479 && r.reclaimedFrom.isEmpty());
        CHECK(r.skipped.size() == 1 && r.skipped.first().port == 3478);
        CHECK(r.skipped.first().reason == Reason::FormerStays);
        CHECK(r.skipped.first().owner == kFormerLan);
        CHECK(b.gw.table.value(udp(3478)) == rowOf(line, kTag));
        Hole noted;
        CHECK(b.record.find(3478, false, noted) && noted == line);
        CHECK(b.core.formerStays());
    }
    // Only its TCP entry is kept there: the UDP hole is taken back, the TCP
    // one left, and said
    {
        Bench b;
        b.gw.conflictsOverClient = true;
        const Hole tcpLine = under(tunnelLine(3478), kFormerLan, true);
        b.left(under(tunnelLine(3478), kFormerLan, false));
        b.left(tcpLine);
        b.gw.beforeAdd = [&b](uint16_t, const QString&) { b.gw.refusesRemoval = true; };
        const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {3478}));
        CHECK(r.ok && r.claim.external == 3478 && !r.claim.tcp && r.reclaimedFrom == kFormerLan);
        CHECK(b.gw.table.value(udp(3478)).client == kLan);
        CHECK(b.gw.table.value(tcp(3478)) == rowOf(tcpLine, kTag));
        CHECK(b.core.formerStays());
    }

    SECTION("RouterSweep OLDADDR: at the claim, nothing that is not provably ours is touched");
    {
        const Hole line = under(tunnelLine(3478), kFormerLan, false);
        const auto notOurs = [&](const char* what, const std::function<void(Bench&)>& set) {
            Bench b;
            b.gw.conflictsOverClient = true;
            set(b);
            const TableGateway::Row before = b.gw.table.value(udp(3478));
            const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {3478}));
            const bool ok = r.ok && r.claim.external == 3479 && r.reclaimedFrom.isEmpty() &&
                            r.skipped.size() == 1 && r.skipped.first().reason == Reason::Foreign &&
                            b.gw.table.value(udp(3478)) == before && b.gw.removes == 0 &&
                            !b.core.formerStays();
            CHECK(ok);
            if (!ok) qWarning() << "case:" << what;
        };
        // Another installation's, under its own tag: another MoonlightWeb
        // that has that address by now
        notOurs("other tag", [&](Bench& b) {
            b.record.note(line);
            b.gw.table[udp(3478)] = rowOf(line, kTheirTag);
        });
        // Towards another address than the one the line was noted under
        notOurs("other client", [&](Bench& b) {
            b.record.note(line);
            TableGateway::Row row = rowOf(line, kTag);
            row.client = QStringLiteral("192.168.1.78");
            b.gw.table[udp(3478)] = row;
        });
        notOurs("other internal port", [&](Bench& b) {
            b.record.note(line);
            TableGateway::Row row = rowOf(line, kTag);
            row.internal = 3500;
            b.gw.table[udp(3478)] = row;
        });
        notOurs("other words", [&](Bench& b) {
            b.record.note(line);
            TableGateway::Row row = rowOf(line, kTag);
            row.description = QStringLiteral("game server");
            b.gw.table[udp(3478)] = row;
        });
        // This installation's tag, and no line in its record: the tag alone
        // proves nothing at another address
        notOurs("no line", [&](Bench& b) { b.gw.table[udp(3478)] = rowOf(line, kTag); });
        // A line of a version that kept no record, and its words alone
        notOurs("untagged line", [&](Bench& b) {
            Hole old = line;
            old.old = true;
            b.record.note(old);
            b.gw.table[udp(3478)] = rowOf(old, QString());
        });
    }
    // The number is taken between the removal and the write (the router
    // answers 718): skipped like any refused number, once, and the next one
    // asked for
    {
        Bench b;
        b.gw.conflictsOverClient = true;
        b.left(under(tunnelLine(3478), kFormerLan, false));
        bool taken = false;
        b.gw.beforeAdd = [&](uint16_t ext, const QString& proto) {
            if (ext != 3478 || proto != kUdp || taken) return;
            taken = true;
            b.gw.table[udp(3478)] = {kNeighbour, 3478, QStringLiteral("game server"), 0};
        };
        const auto r = b.core.claim(RouterPortCore::tunnelRequest(false, {3478}));
        CHECK(r.ok && r.claim.external == 3479 && r.reclaimedFrom.isEmpty());
        CHECK(r.skipped.size() == 1 && r.skipped.first().reason == Reason::Refused);
        CHECK(b.gw.table.value(udp(3478)).client == kNeighbour);
        CHECK(!b.record.has(3478, false));
        CHECK_EQ(b.gw.removes, 1);
        CHECK(!b.core.formerStays());
    }

    SECTION("RouterSweep OLDADDR: the sweep takes back a line left under an old address");
    for (const Kinded& k : everyKind()) {
        for (const bool isTcp : {false, true}) {
            Bench b;
            const Hole line = under(k.line, kFormerLan, isTcp);
            b.left(line);
            CHECK(b.core.sweepList() == QList<Hole>{line});
            CHECK(b.core.sweepBelieved(b.core.sweepList()));
            const auto r = b.core.sweep(line);
            CHECK(r.outcome == Swept::RemovedFormer && r.former);
            CHECK(b.gw.table.isEmpty() && b.record.lines.isEmpty());
            CHECK_EQ(b.gw.removes, 1);
            // A tagged line asks nothing of the machine's own sockets
            CHECK(b.listenAsked.isEmpty() && b.serverAsked == 0);
        }
    }
    // What is left as it is, and whether its line stays
    {
        const Hole line = under(tunnelLine(3478), kFormerLan, false);
        const auto looked = [&](const char* what, const std::function<void(Bench&)>& set,
                                Swept outcome, const QString& detail, bool lineStays) {
            Bench b;
            b.left(line);
            set(b);
            const auto table = b.gw.table;
            const auto r = b.core.sweep(line);
            const bool ok = r.outcome == outcome && r.former && r.detail == detail &&
                            b.gw.table == table && b.record.has(3478, false) == lineStays &&
                            !b.core.formerStays();
            CHECK(ok);
            if (!ok) qWarning() << "case:" << what;
        };
        looked("gone", [](Bench& b) { b.gw.table.clear(); }, Swept::Gone, QString(), false);
        looked(
            "silent", [](Bench& b) { b.gw.silent.insert(3478); }, Swept::Unknown, QString(), true);
        looked(
            "garbled", [](Bench& b) { b.gw.table[udp(3478)].internal = 0; }, Swept::Unknown,
            QString(), true);
        looked(
            "other tag", [&](Bench& b) { b.gw.table[udp(3478)] = rowOf(line, kTheirTag); },
            Swept::NotOurs, QStringLiteral("description"), false);
        looked(
            "other client",
            [](Bench& b) { b.gw.table[udp(3478)].client = QStringLiteral("192.168.1.78"); },
            Swept::NotOurs, QStringLiteral("address"), false);
        looked(
            "this host's address now", [](Bench& b) { b.gw.table[udp(3478)].client = kLan; },
            Swept::NotOurs, QStringLiteral("address"), false);
        looked(
            "other internal port", [](Bench& b) { b.gw.table[udp(3478)].internal = 3500; },
            Swept::NotOurs, QStringLiteral("internal port"), false);
    }
    // The removal is refused: the line stays, and the admin page is told
    // until a later removal goes through
    {
        Bench b;
        const Hole line = under(tunnelLine(3478), kFormerLan, false);
        b.left(line);
        b.gw.refusesRemoval = true;
        auto r = b.core.sweep(line);
        CHECK(r.outcome == Swept::Stayed && r.former);
        CHECK(b.record.has(3478, false) && b.gw.table.contains(udp(3478)));
        CHECK(b.core.formerStays());
        b.gw.refusesRemoval = false;
        r = b.core.sweep(line);
        CHECK(r.outcome == Swept::RemovedFormer);
        CHECK(!b.core.formerStays());
        CHECK(b.gw.table.isEmpty() && b.record.lines.isEmpty());
    }
    // A line of a version that kept no record, under an old address: not
    // looked at, not listed
    {
        Bench b;
        Hole line = under(tunnelLine(3478), kFormerLan, false);
        line.old = true;
        b.left(line, QString());
        CHECK(b.core.sweepList().isEmpty());
        CHECK(b.core.sweep(line).outcome == Swept::OtherAddress);
        CHECK(b.gw.asked.isEmpty() && b.gw.removes == 0);
    }

    SECTION("RouterSweep OLDADDR: a renumbering under a running server moves every hole (D1)");
    {
        Bench b;
        b.gw.conflictsOverClient = true;
        const auto t = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
        const auto m = b.core.claim(RouterPortCore::mediaRequest(false, false, 1, 48551, 0, false));
        CHECK(t.ok && t.claim.external == 3478 && t.claim.tcp);
        CHECK(m.ok && m.claim.external == 48551 && m.claim.tcp);
        // A DHCP lease gives this machine another address
        b.gw.now = kNewLan;
        CHECK(b.core.addressMoved());
        // Nothing is renewed towards the address it no longer has
        const int adds = b.gw.adds;
        CHECK(b.core.renew().isEmpty());
        CHECK(b.core.renew(3478).isEmpty());
        CHECK_EQ(b.gw.adds, adds);
        // Followed: each entry taken off the former address, written here
        CHECK(b.core.followAddress(true).isEmpty());
        CHECK_EQ(b.rediscovers, 1);
        CHECK(b.gw.lan == kNewLan && !b.core.addressMoved());
        for (const uint16_t port : {uint16_t(3478), uint16_t(48551)})
            for (const bool isTcp : {false, true}) {
                CHECK(b.gw.table.value({port, protocolOf(isTcp)}).client == kNewLan);
                Hole noted;
                CHECK(b.record.find(port, isTcp, noted) && noted.lan == kNewLan);
            }
        CHECK(b.core.holds(3478) && b.core.holds(48551));
        CHECK(!b.core.formerStays());
        // Nothing more to follow, and the renewals go on from here
        CHECK(b.core.followAddress(true).isEmpty());
        CHECK_EQ(b.rediscovers, 1);
        CHECK(b.core.renew().isEmpty());
        CHECK(b.gw.table.value(udp(3478)).client == kNewLan);
    }
    // The router does not answer from the new address: nothing is written
    // until it does. A claim does not look again from the same address, a
    // renewal does.
    {
        Bench b;
        b.gw.conflictsOverClient = true;
        CHECK(b.core.claim(RouterPortCore::tunnelRequest(false, {})).ok);
        b.gw.now = kNewLan;
        b.rediscoverFails = true;
        const auto table = b.gw.table;
        const int adds = b.gw.adds;
        CHECK(b.core.followAddress(true).isEmpty());
        CHECK_EQ(b.rediscovers, 1);
        CHECK(b.core.renew().isEmpty());
        CHECK(b.gw.table == table && b.gw.adds == adds && b.gw.removes == 0);
        CHECK(b.core.followAddress(false).isEmpty());
        CHECK_EQ(b.rediscovers, 1);
        CHECK(b.core.followAddress(true).isEmpty());
        CHECK_EQ(b.rediscovers, 2);
        b.rediscoverFails = false;
        CHECK(b.core.followAddress(true).isEmpty());
        CHECK_EQ(b.rediscovers, 3);
        CHECK(b.gw.table.value(udp(3478)).client == kNewLan);
    }
    // The router, found again, sees this machine where it saw it: nothing moved
    {
        Bench b;
        CHECK(b.core.claim(RouterPortCore::tunnelRequest(false, {})).ok);
        b.gw.now = kNewLan;
        // What the system says stays what it said: the router is not looked
        // for again from there, and the renewals go on
        b.core.setRediscover([&b]() {
            ++b.rediscovers;
            return true;
        });
        const auto table = b.gw.table;
        CHECK(b.core.followAddress(true).isEmpty());
        CHECK_EQ(b.rediscovers, 1);
        CHECK(b.gw.table == table && b.gw.removes == 0);
        CHECK(!b.core.addressMoved());
        CHECK(b.core.followAddress(true).isEmpty());
        CHECK_EQ(b.rediscovers, 1);
        const int adds = b.gw.adds;
        CHECK(b.core.renew().isEmpty());
        CHECK(b.gw.adds > adds && b.gw.table.value(udp(3478)).client == kLan);
    }
    // The router keeps the entries towards the former address: those holes
    // are lost (the main thread forgets the numbers and asks for others),
    // their lines stay, the admin page is told, and nothing is written over
    // them
    {
        Bench b;
        b.gw.conflictsOverClient = true;
        CHECK(b.core.claim(RouterPortCore::tunnelRequest(false, {})).ok);
        CHECK(b.core.claim(RouterPortCore::mediaRequest(false, false, 1, 48551, 0, false)).ok);
        const auto table = b.gw.table;
        const int adds = b.gw.adds;
        b.gw.now = kNewLan;
        b.gw.refusesRemoval = true;
        const auto lost = b.core.followAddress(true);
        CHECK_EQ(lost.size(), 2);
        for (const RouterPortCore::Lost& l : lost)
            CHECK(l.owner == kLan && !l.here);
        CHECK(!b.core.holds(3478) && !b.core.holds(48551));
        CHECK(b.gw.table == table && b.gw.adds == adds);
        Hole noted;
        CHECK(b.record.find(3478, false, noted) && noted.lan == kLan);
        CHECK(b.core.formerStays());
    }
    // The router keeps only the TCP entry towards the former address: the
    // hole goes on in UDP from here, without TCP, and that TCP entry is never
    // touched again in this run, not even at release
    {
        Bench b;
        b.gw.conflictsOverClient = true;
        const auto t = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
        CHECK(t.ok && t.claim.external == 3478 && t.claim.tcp);
        b.gw.now = kNewLan;
        b.gw.keeps = {{uint16_t(3478), kTcp}};
        CHECK(b.core.followAddress(true).isEmpty());
        CHECK(b.core.holds(3478));
        const QList<RouterPortCore::Claim> held = b.core.held();
        CHECK(held.size() == 1 && !held.first().tcp);
        CHECK(b.gw.table.value(udp(3478)).client == kNewLan);
        CHECK(b.gw.table.value({uint16_t(3478), kTcp}).client == kLan);
        Hole noted;
        CHECK(b.record.find(3478, false, noted) && noted.lan == kNewLan);
        CHECK(b.record.find(3478, true, noted) && noted.lan == kLan);
        CHECK(b.core.formerStays());
        // Released with the claim as the main thread has it (TCP still said)
        const int removes = b.gw.removes;
        b.core.release(t.claim);
        CHECK(!b.core.holds(3478));
        CHECK(!b.gw.table.contains(udp(3478)));
        CHECK_EQ(b.gw.removes, removes + 1);
        CHECK(b.gw.table.value({uint16_t(3478), kTcp}).client == kLan);
        CHECK(b.record.find(3478, true, noted) && noted.lan == kLan);
        CHECK(b.core.formerStays());
    }
    // While the router is not found again from the new address, a claim
    // writes nothing: the address it would name may be another machine's.
    // Found again, the same claim is written towards the new address.
    {
        Bench b;
        b.gw.now = kNewLan;
        b.rediscoverFails = true;
        CHECK(b.core.followAddress(false).isEmpty());
        CHECK(b.core.addressMoved());
        b.gw.asked.clear();
        const auto refused = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
        CHECK(!refused.ok && refused.skipped.isEmpty());
        CHECK(b.gw.adds == 0 && b.gw.removes == 0 && b.gw.asked.isEmpty());
        CHECK(b.gw.table.isEmpty() && b.record.holes().isEmpty());
        CHECK(!b.core.holds(3478));
        b.rediscoverFails = false;
        CHECK(b.core.followAddress(true).isEmpty());
        CHECK(!b.core.addressMoved());
        const auto written = b.core.claim(RouterPortCore::tunnelRequest(false, {}));
        CHECK(written.ok && written.claim.external == 3478);
        CHECK(b.gw.table.value(udp(3478)).client == kNewLan);
        Hole noted;
        CHECK(b.record.find(3478, false, noted) && noted.lan == kNewLan);
    }
    // Quitting: nothing is followed any more
    {
        Bench b;
        CHECK(b.core.claim(RouterPortCore::tunnelRequest(false, {})).ok);
        b.core.close();
        b.gw.now = kNewLan;
        CHECK(b.core.followAddress(true).isEmpty());
        CHECK_EQ(b.rediscovers, 0);
    }

    SECTION("RouterSweep OLDADDR: what the admin page is told, and nobody else");
    {
        const QLatin1String key(mw::routerholes::kStaleRefusedKey);
        QJsonObject status{{QStringLiteral("active"), true}};
        mw::routerholes::tellAdmin(status, true, true);
        CHECK(status.value(key).isBool() && status.value(key).toBool());
        mw::routerholes::tellAdmin(status, true, false);
        CHECK(status.value(key).isBool() && !status.value(key).toBool());
        // Nothing but that yes or no goes with it
        CHECK_EQ(status.size(), qsizetype(2));
        // Whoever is not the admin gets no such key, whatever the status held
        mw::routerholes::tellAdmin(status, true, true);
        mw::routerholes::tellAdmin(status, false, true);
        CHECK(!status.contains(key) && status.value(QStringLiteral("active")).toBool());
        // A key of its own, beside the one of TCP 443
        CHECK(QLatin1String(mw::routerholes::kStaleRefusedKey) !=
              QLatin1String("router443_stale_removal_refused"));
    }
}
