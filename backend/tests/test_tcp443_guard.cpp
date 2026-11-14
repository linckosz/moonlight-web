/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * TCP 443 on the router (network/Tcp443Guard): when the host asks for it,
 * step by step, what it writes, and what it takes back. No router and no
 * socket here: a table for the one, a script for what dialling would find.
 */
#include "test_framework.h"
#include "network/Tcp443Guard.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPair>
#include <QStringList>

#include <functional>

using namespace mw::tcp443;
using mw::routerholes::Hole;
using mw::routerholes::Kind;

namespace {

const QString kTcp = QStringLiteral("TCP");
const QString kUdp = QStringLiteral("UDP");
const QString kTag = QStringLiteral("a3f9c1");
const QString kText = QStringLiteral("MoonlightWeb TCP 443 a3f9c1");
const QString kPublic = QStringLiteral("1.2.3.4");
const QString kLan = QStringLiteral("192.168.1.20");
const QString kFormerLan = QStringLiteral("192.168.1.77");
const QString kNeighbour = QStringLiteral("192.168.1.34");
const uint16_t kBlock = 48550;

/// The router's table, and the ways a router can be unhelpful.
struct Box : IUpnpGateway
{
    struct Row
    {
        QString client;
        uint16_t internal = 0;
        QString text;
        uint32_t lease = 0;

        bool operator==(const Row& o) const
        {
            return client == o.client && internal == o.internal && text == o.text;
        }
    };
    QString lan = kLan;
    QMap<QPair<uint16_t, QString>, Row> table;
    bool silent = false;        ///< says nothing of any entry
    bool refusesAdd = false;    ///< refuses to write
    bool forgetsAdd = false;    ///< says it wrote, and did not
    bool refusesRemove = false; ///< refuses to remove
    QString writesTowards;      ///< writes the entry towards this machine instead
    int adds = 0, removes = 0, lookups = 0;

    QString lanAddress() const override { return lan; }
    bool add(uint16_t external, uint16_t internal, uint32_t leaseSec, const QString& description,
             const QString& protocol) override
    {
        ++adds;
        if (refusesAdd) return false;
        if (!forgetsAdd)
            table[{external, protocol}] =
                Row{writesTowards.isEmpty() ? lan : writesTowards, internal, description, leaseSec};
        return true;
    }
    Lookup entry(uint16_t external, const QString& protocol, Entry& out) override
    {
        ++lookups;
        if (silent) return Lookup::Unknown;
        const auto it = table.constFind({external, protocol});
        if (it == table.constEnd()) return Lookup::None;
        out.client = it->client;
        out.internalPort = it->internal;
        out.description = it->text;
        out.leaseLeft = static_cast<int>(it->lease);
        return Lookup::Found;
    }
    bool remove(uint16_t external, const QString& protocol) override
    {
        ++removes;
        if (refusesRemove) return false;
        return table.remove({external, protocol}) > 0;
    }

    bool has443() const { return table.contains({kPort, kTcp}); }
    Row row443() const { return table.value({kPort, kTcp}); }
};

/// The record, in memory.
struct Notes : mw::routerholes::IRecord
{
    QList<Hole> lines;

    QString tag() const override { return kTag; }
    QList<Hole> holes() const override { return lines; }
    bool find(uint16_t external, bool tcp, Hole& out) const override
    {
        for (const Hole& line : lines) {
            if (line.external != external || line.tcp != tcp) continue;
            out = line;
            return true;
        }
        return false;
    }
    void note(const Hole& hole) override
    {
        strike(hole.external, hole.tcp);
        lines.append(hole);
    }
    void strike(uint16_t external, bool tcp) override
    {
        for (qsizetype i = lines.size() - 1; i >= 0; --i)
            if (lines[i].external == external && lines[i].tcp == tcp) lines.removeAt(i);
    }
};

/// What dialling finds. By default what the table says: a connection to the
/// router's 443 arrives here when the router forwards it to this machine's
/// block port, and nothing answers there when it forwards it nowhere.
struct Dials : IProbe
{
    explicit Dials(const Box& router)
        : box(router)
    {}
    const Box& box;
    bool loopsBack = true;
    /// Something else than a UPnP entry serves 443: a rule made by hand
    bool handMade = false;
    /// The entry is there and still nothing arrives
    bool deaf = false;
    QStringList log;
    /// The shared port the last frame was to come in on
    uint16_t toldPort = 0;

    bool make(const Dial& dial) override
    {
        if (dial.kind == Dial::Kind::Answers) {
            log << QStringLiteral("silent to %1").arg(dial.port);
            return dial.address == kPublic && (handMade || box.has443());
        }
        log << QStringLiteral("frame to %1").arg(dial.port);
        toldPort = dial.leadsHereOn;
        if (dial.kind != Dial::Kind::ArrivesHere || dial.address != kPublic || !loopsBack)
            return false;
        // Straight to a port of the router's: here when that is the port
        // this host listens on
        if (dial.port != kPort) return dial.port == dial.leadsHereOn;
        // To 443: here when the router forwards it to this machine, towards
        // the port this host listens on
        const Box::Row row = box.row443();
        return !deaf && box.has443() && row.client == box.lan && row.internal == dial.leadsHereOn;
    }
};

struct Rig
{
    Box box;
    Notes notes;
    Dials dials{box};
    Guard guard{box, &notes};

    /// The steps in a row, each dial made at once: what only a test does
    Verdict claim(const Situation& now) { return guard.claim(now, dials); }
};

Situation wanted()
{
    Situation s;
    s.wanted = true;
    s.publicAddress = kPublic;
    s.routerAddress = kPublic;
    s.blockPort = kBlock;
    s.leaseSec = 3600;
    return s;
}

Hole lineUnder(const QString& lan, uint16_t internal = kBlock)
{
    Hole line;
    line.external = kPort;
    line.tcp = true;
    line.internal = internal;
    line.kind = Kind::Tcp443;
    line.slot = -1;
    line.lan = lan;
    return line;
}

Box::Row rowOf(const QString& client, uint16_t internal = kBlock, const QString& text = kText)
{
    return Box::Row{client, internal, text, 3600};
}

/// Nothing a verdict says names an address
bool namesNoAddress(const Verdict& v)
{
    return !v.said.isEmpty() && !v.said.contains(QStringLiteral("192.168")) &&
           !v.said.contains(kPublic) && !v.said.contains(kTag);
}

void addresses()
{
    SECTION("Tcp443Guard: an address the Internet routes to one place");
    for (const char* good :
         {"1.2.3.4", "8.8.8.8", "100.63.255.255", "100.128.0.1", "172.15.0.1", "172.32.0.1",
          "192.167.1.1", "169.253.1.1", "223.255.255.254", "192.0.1.1", "192.0.3.1", "192.88.98.1",
          "192.88.100.1", "198.17.0.1", "198.20.0.1", "198.51.99.1", "198.51.101.1", "203.0.112.1",
          "203.0.114.1"})
        CHECK(publicIpv4(QString::fromLatin1(good)));
    // The ranges kept for documentation, for a relay and for benches: each
    // from its first address to its last
    for (const char* kept : {"192.0.2.0", "192.0.2.255", "198.51.100.0", "198.51.100.255",
                             "203.0.113.0", "203.0.113.255", "192.88.99.0", "192.88.99.255",
                             "198.18.0.0", "198.19.255.255", "192.0.0.0", "192.0.0.255"})
        CHECK(!publicIpv4(QString::fromLatin1(kept)));
    for (const char* bad : {"",
                            "10.0.0.1",
                            "172.16.0.1",
                            "172.31.255.255",
                            "192.168.1.1",
                            "100.64.0.1",
                            "100.127.255.255",
                            "169.254.10.10",
                            "127.0.0.1",
                            "0.1.2.3",
                            "224.0.0.1",
                            "255.255.255.255",
                            "192.0.0.8",
                            "198.18.0.1",
                            "198.19.255.255",
                            "2001:db8::1",
                            "example.org",
                            "1.2.3",
                            "1.2.3.4.5",
                            " 8.8.8.8",
                            "08.8.8.8"})
        CHECK(!publicIpv4(QString::fromLatin1(bad)));
}

void notAsked()
{
    SECTION("Tcp443Guard: not asked for, and the router is told nothing");
    {
        Rig r;
        Situation s = wanted();
        s.wanted = false;
        const Verdict v = r.claim(s);
        CHECK(v.outcome == Outcome::Left && v.step == 0 && namesNoAddress(v));
        CHECK(r.box.lookups == 0 && r.box.adds == 0 && r.dials.log.isEmpty());
    }
    {
        // No shared TCP port for it to lead to
        Rig r;
        Situation s = wanted();
        s.blockPort = 0;
        CHECK(r.claim(s).step == 0);
        CHECK(r.box.lookups == 0 && r.dials.log.isEmpty());
    }
    {
        // A run that keeps no record never writes what it could not take back
        Box box;
        Dials dials(box);
        Guard guard(box, nullptr);
        const Verdict v = guard.claim(wanted(), dials);
        CHECK(v.outcome == Outcome::Left && v.step == 0);
        CHECK(box.lookups == 0 && box.adds == 0 && dials.log.isEmpty());
        CHECK(guard.release() && guard.formerAddress() == Guard::Former::None);
        CHECK(guard.abandon() == Guard::Abandoned::Nothing && !guard.underWay());
    }
}

void stepByStep()
{
    SECTION("Tcp443Guard: each step that says no leaves 443 as it is");
    // 1: a private address, an operator's shared one, none, another than the router's
    for (const QString& address : {QStringLiteral("192.168.1.1"), QStringLiteral("100.72.3.4"),
                                   QString(), QStringLiteral("2001:db8::1")}) {
        Rig r;
        Situation s = wanted();
        s.publicAddress = address;
        s.routerAddress = address;
        const Verdict v = r.claim(s);
        CHECK(v.outcome == Outcome::Left && v.step == 1 && namesNoAddress(v));
        CHECK(r.box.lookups == 0 && r.box.adds == 0 && r.dials.log.isEmpty());
    }
    {
        // A second router in front: the one asked is not the one browsers reach
        Rig r;
        Situation s = wanted();
        s.routerAddress = QStringLiteral("198.51.100.9");
        CHECK(r.claim(s).step == 1);
        s.routerAddress = QStringLiteral("10.0.0.2");
        CHECK(r.claim(s).step == 1);
        CHECK(r.box.lookups == 0);
    }

    // 2: an entry on 443, whoever it leads to; a router that does not say
    const auto taken = [](const Box::Row& there) {
        Rig r;
        r.box.table[{kPort, kTcp}] = there;
        const Verdict v = r.claim(wanted());
        CHECK(v.outcome == Outcome::Left && v.step == 2 && namesNoAddress(v));
        CHECK(r.box.row443() == there);
        CHECK(r.box.adds == 0 && r.box.removes == 0 && r.dials.log.isEmpty());
        CHECK(r.notes.lines.isEmpty());
    };
    taken(rowOf(kNeighbour, 443, QStringLiteral("https")));
    // Towards this very machine, by another program or by hand
    taken(rowOf(kLan, 8443, QStringLiteral("my server")));
    // With this installation's very words, and no line in the record
    taken(rowOf(kLan));
    {
        Rig r;
        r.box.silent = true;
        const Verdict v = r.claim(wanted());
        CHECK(v.outcome == Outcome::Left && v.step == 2);
        CHECK(r.box.adds == 0 && r.dials.log.isEmpty());
    }
    {
        // Noted, and written with another installation's tag: not ours
        Rig r;
        r.notes.note(lineUnder(kLan));
        r.box.table[{kPort, kTcp}] =
            rowOf(kLan, kBlock, QStringLiteral("MoonlightWeb TCP 443 ffffff"));
        CHECK(r.claim(wanted()).step == 2);
        CHECK(r.box.removes == 0 && r.box.has443());
    }

    // 3: a router that does not lead back here cannot be checked from here
    {
        Rig r;
        r.dials.loopsBack = false;
        const Verdict v = r.claim(wanted());
        CHECK(v.outcome == Outcome::Left && v.step == 3 && namesNoAddress(v));
        CHECK(r.dials.log == QStringList{QStringLiteral("frame to 48550")});
        CHECK(r.box.adds == 0 && r.notes.lines.isEmpty());
    }
    // 4: something answers on 443 that the router's table does not show
    {
        Rig r;
        r.dials.handMade = true;
        const Verdict v = r.claim(wanted());
        CHECK(v.outcome == Outcome::Left && v.step == 4 && namesNoAddress(v));
        CHECK(r.box.adds == 0 && r.notes.lines.isEmpty() && !r.box.has443());
    }
}

void written()
{
    SECTION("Tcp443Guard: written, read back, and proved by a connection");
    {
        Rig r;
        const Verdict v = r.claim(wanted());
        CHECK(v.outcome == Outcome::Held && v.step == 5 && namesNoAddress(v));
        CHECK(r.guard.held() && r.guard.leadsTo() == kBlock);
        // One entry, TCP, to the block port, under this installation's words
        CHECK(r.box.row443() == rowOf(kLan));
        CHECK_EQ(r.box.row443().lease, uint32_t(3600));
        CHECK(!r.box.table.contains({kPort, kUdp}) && r.box.table.size() == 1);
        CHECK(r.notes.lines == QList<Hole>{lineUnder(kLan)});
        // In this order: the way back, then 443 in silence, then 443 for proof
        CHECK(r.dials.log ==
              (QStringList{QStringLiteral("frame to 48550"), QStringLiteral("silent to 443"),
                           QStringLiteral("frame to 443")}));
        // Every line the guard notes is one an installed host may have
        CHECK(mw::routerholes::allowed(false, r.notes.lines.first()));
        CHECK(!mw::routerholes::allowed(true, r.notes.lines.first()));
    }

    SECTION("Tcp443Guard: what step 5 wrote and could not prove is taken back");
    {
        // Refused: nothing on the router, nothing in the record
        Rig r;
        r.box.refusesAdd = true;
        const Verdict v = r.claim(wanted());
        CHECK(v.outcome == Outcome::Left && v.step == 5 && namesNoAddress(v));
        CHECK(!r.box.has443() && r.notes.lines.isEmpty() && !r.guard.held());
    }
    {
        // Said yes and wrote nothing
        Rig r;
        r.box.forgetsAdd = true;
        CHECK(r.claim(wanted()).step == 5);
        CHECK(!r.box.has443() && r.notes.lines.isEmpty());
        // No connection was sent to prove what does not read back
        CHECK(r.dials.log.size() == 2);
    }
    {
        // Wrote it towards another machine: not this installation's entry,
        // and so not removed by it; nothing is noted as ours
        Rig r;
        r.box.writesTowards = kNeighbour;
        CHECK(r.claim(wanted()).step == 5);
        CHECK(r.box.removes == 0 && r.box.row443().client == kNeighbour);
        CHECK(r.notes.lines.isEmpty() && r.dials.log.size() == 2);
    }
    {
        // Reads back, and no connection arrives: removed, and the removal read back
        Rig r;
        r.dials.deaf = true;
        const Verdict v = r.claim(wanted());
        CHECK(v.outcome == Outcome::Left && v.step == 5);
        CHECK(r.box.removes == 1 && !r.box.has443() && r.notes.lines.isEmpty());
    }
    {
        // The same on a router that will not remove it: the line stays, for
        // the next start to take the entry back
        Rig r;
        r.dials.deaf = true;
        r.box.refusesRemove = true;
        CHECK(r.claim(wanted()).step == 5);
        CHECK(r.box.has443() && r.notes.lines == QList<Hole>{lineUnder(kLan)});
        CHECK(!r.guard.held());
    }
}

void kept()
{
    SECTION("Tcp443Guard: renewed while it is still this installation's");
    {
        Rig r;
        CHECK(r.claim(wanted()).outcome == Outcome::Held);
        const int dialled = static_cast<int>(r.dials.log.size());
        const Verdict v = r.guard.renew(wanted());
        CHECK(v.outcome == Outcome::Held && v.step == 6 && namesNoAddress(v));
        // Written again, and nothing dialled
        CHECK(r.box.adds == 2 && r.dials.log.size() == dialled);
        // Not held: nothing
        Rig idle;
        CHECK(idle.guard.renew(wanted()).outcome == Outcome::Left);
        CHECK(idle.box.lookups == 0 && idle.box.adds == 0);
    }
    {
        // Another's by the time it is renewed: not written again, and no
        // longer in the record
        Rig r;
        CHECK(r.claim(wanted()).outcome == Outcome::Held);
        r.box.table[{kPort, kTcp}] = rowOf(kNeighbour, 443, QStringLiteral("https"));
        const Verdict v = r.guard.renew(wanted());
        CHECK(v.outcome == Outcome::Lost && v.step == 6 && namesNoAddress(v));
        CHECK(r.box.adds == 1 && r.box.removes == 0 && r.box.row443().client == kNeighbour);
        CHECK(r.notes.lines.isEmpty() && !r.guard.held() && r.guard.leadsTo() == 0);
        // The next attempt is a whole run, which finds 443 taken
        CHECK(r.claim(wanted()).step == 2);
    }
    {
        // Gone from the router (it restarted): lost, and struck
        Rig r;
        CHECK(r.claim(wanted()).outcome == Outcome::Held);
        r.box.table.clear();
        CHECK(r.guard.renew(wanted()).outcome == Outcome::Lost);
        CHECK(r.box.adds == 1 && r.notes.lines.isEmpty());
        // A whole run then holds it again
        CHECK(r.claim(wanted()).outcome == Outcome::Held);
    }
    {
        // A router that says nothing: lost for now, and the line stays
        Rig r;
        CHECK(r.claim(wanted()).outcome == Outcome::Held);
        r.box.silent = true;
        CHECK(r.guard.renew(wanted()).outcome == Outcome::Lost);
        CHECK(r.box.adds == 1 && r.notes.lines.size() == 1 && !r.guard.held());
        // It speaks again: the entry is this installation's, and is kept
        // without a new run of dials
        r.box.silent = false;
        const int dialled = static_cast<int>(r.dials.log.size());
        const Verdict again = r.claim(wanted());
        CHECK(again.outcome == Outcome::Held && again.step == 6);
        CHECK(r.dials.log.size() == dialled && r.box.adds == 2);
    }
    {
        // A router that refuses the renewal
        Rig r;
        CHECK(r.claim(wanted()).outcome == Outcome::Held);
        r.box.refusesAdd = true;
        CHECK(r.guard.renew(wanted()).outcome == Outcome::Lost);
        CHECK(!r.guard.held() && r.notes.lines.size() == 1);
    }

    SECTION("Tcp443Guard: what it stood on changed, and the entry is taken off");
    const auto changed = [](const std::function<void(Situation&)>& how) {
        Rig r;
        CHECK(r.claim(wanted()).outcome == Outcome::Held);
        Situation s = wanted();
        how(s);
        const Verdict v = r.guard.renew(s);
        CHECK(v.outcome == Outcome::Lost && namesNoAddress(v));
        CHECK(!r.box.has443() && r.notes.lines.isEmpty() && !r.guard.held());
        CHECK(r.box.adds == 1 && r.box.removes == 1);
    };
    changed([](Situation& s) { s.wanted = false; });
    changed([](Situation& s) { s.blockPort = uint16_t(kBlock + 1); });
    changed([](Situation& s) { s.blockPort = 0; });
    changed([](Situation& s) { s.publicAddress = QStringLiteral("198.51.100.9"); });
    changed([](Situation& s) { s.routerAddress = QStringLiteral("10.0.0.2"); });

    SECTION(
        "Tcp443Guard: an earlier run's entry is this installation's by the record and the text");
    {
        // Left by a run that was killed: kept, with no dial
        Rig r;
        r.notes.note(lineUnder(kLan));
        r.box.table[{kPort, kTcp}] = rowOf(kLan);
        const Verdict v = r.claim(wanted());
        CHECK(v.outcome == Outcome::Held && v.step == 6);
        CHECK(r.dials.log.isEmpty() && r.box.adds == 1 && r.box.removes == 0);
    }
    {
        // Left towards a port the streams no longer share: taken off, and
        // asked for anew by a whole run
        Rig r;
        r.notes.note(lineUnder(kLan, uint16_t(kBlock + 3)));
        r.box.table[{kPort, kTcp}] = rowOf(kLan, uint16_t(kBlock + 3));
        const Verdict v = r.claim(wanted());
        CHECK(v.outcome == Outcome::Held && v.step == 5);
        CHECK(r.box.removes == 1 && r.box.row443() == rowOf(kLan));
        CHECK(r.notes.lines == QList<Hole>{lineUnder(kLan)});
        CHECK(r.dials.log.size() == 3);
    }
    {
        // The same, on a router that will not let go of it
        Rig r;
        r.notes.note(lineUnder(kLan, uint16_t(kBlock + 3)));
        r.box.table[{kPort, kTcp}] = rowOf(kLan, uint16_t(kBlock + 3));
        r.box.refusesRemove = true;
        CHECK(r.claim(wanted()).step == 2);
        CHECK(r.box.adds == 0 && r.notes.lines.size() == 1);
    }
    {
        // Noted, and the router no longer has it: the line goes, the run goes on
        Rig r;
        r.notes.note(lineUnder(kLan));
        CHECK(r.claim(wanted()).outcome == Outcome::Held);
        CHECK(r.notes.lines == QList<Hole>{lineUnder(kLan)});
    }
}

void released()
{
    SECTION("Tcp443Guard: taken off when the host stops, and the removal read back");
    {
        Rig r;
        CHECK(r.claim(wanted()).outcome == Outcome::Held);
        CHECK(r.guard.release());
        CHECK(!r.box.has443() && r.notes.lines.isEmpty() && !r.guard.held());
        CHECK(r.box.removes == 1);
        // Twice is nothing
        CHECK(r.guard.release() && r.box.removes == 1);
    }
    {
        // A router that will not remove it: said, and the line stays
        Rig r;
        CHECK(r.claim(wanted()).outcome == Outcome::Held);
        r.box.refusesRemove = true;
        CHECK(!r.guard.release());
        CHECK(r.box.has443() && r.notes.lines.size() == 1 && !r.guard.held());
    }
    {
        // A router that says nothing: the same
        Rig r;
        CHECK(r.claim(wanted()).outcome == Outcome::Held);
        r.box.silent = true;
        CHECK(!r.guard.release());
        CHECK(r.box.removes == 0 && r.notes.lines.size() == 1);
    }
    {
        // Another's by now: not removed, and no longer in the record
        Rig r;
        CHECK(r.claim(wanted()).outcome == Outcome::Held);
        r.box.table[{kPort, kTcp}] = rowOf(kNeighbour, 443, QStringLiteral("https"));
        CHECK(r.guard.release());
        CHECK(r.box.removes == 0 && r.box.has443() && r.notes.lines.isEmpty());
    }
    {
        // A line of another kind on that number is not the guard's to touch
        Rig r;
        Hole other = lineUnder(kLan);
        other.kind = Kind::Tunnel;
        r.notes.note(other);
        r.box.table[{kPort, kTcp}] = rowOf(kLan);
        CHECK(r.guard.release());
        CHECK(r.box.removes == 0 && r.notes.lines.size() == 1);
    }
}

void formerAddress()
{
    SECTION("Tcp443Guard: an entry written under an address this machine no longer has");
    using Former = Guard::Former;
    {
        // Still there as it was written: removed, though it leads to another
        // address than this machine's, and the run then goes on
        Rig r;
        r.notes.note(lineUnder(kFormerLan));
        r.box.table[{kPort, kTcp}] = rowOf(kFormerLan);
        CHECK(r.guard.formerAddress() == Former::Removed);
        CHECK(!r.box.has443() && r.notes.lines.isEmpty() && !r.guard.formerRefused());
        CHECK(r.guard.formerAddress() == Former::None);

        Rig whole;
        whole.notes.note(lineUnder(kFormerLan));
        whole.box.table[{kPort, kTcp}] = rowOf(kFormerLan);
        const Verdict v = whole.claim(wanted());
        CHECK(v.outcome == Outcome::Held && v.step == 5);
        CHECK(whole.box.removes == 1 && whole.box.row443() == rowOf(kLan));
        CHECK(whole.notes.lines == QList<Hole>{lineUnder(kLan)});
    }
    {
        // The router will not remove it: 443 is left, it is said, and the
        // line stays until the entry is gone
        Rig r;
        r.notes.note(lineUnder(kFormerLan));
        r.box.table[{kPort, kTcp}] = rowOf(kFormerLan);
        r.box.refusesRemove = true;
        const Verdict v = r.claim(wanted());
        CHECK(v.outcome == Outcome::Left && v.step == 2 && namesNoAddress(v));
        CHECK(r.guard.formerRefused());
        CHECK(r.box.has443() && r.box.adds == 0 && r.dials.log.isEmpty());
        CHECK(r.notes.lines == QList<Hole>{lineUnder(kFormerLan)});
        // Tried again, and still refused
        CHECK(r.guard.formerAddress() == Former::Refused && r.guard.formerRefused());
        CHECK(!r.guard.release());
        // A router that says nothing leaves what was said standing: the
        // entry may be there still
        r.box.silent = true;
        CHECK(r.guard.formerAddress() == Former::Unknown && r.guard.formerRefused());
        CHECK(r.claim(wanted()).step == 2 && r.guard.formerRefused());
        CHECK(r.notes.lines == QList<Hole>{lineUnder(kFormerLan)});
        r.box.silent = false;
        CHECK(r.guard.formerAddress() == Former::Refused && r.guard.formerRefused());
        // Somebody removed it on the router: no longer said, and the run goes on
        r.box.table.clear();
        CHECK(r.guard.formerAddress() == Former::Gone);
        CHECK(!r.guard.formerRefused() && r.notes.lines.isEmpty());
        CHECK(r.claim(wanted()).outcome == Outcome::Held);
    }
    {
        // Another's by now, at that address or elsewhere: not removed, the
        // line goes, and 443 is somebody's
        for (const Box::Row& there :
             {rowOf(kFormerLan, 443), rowOf(kFormerLan, kBlock, QStringLiteral("https")),
              rowOf(kNeighbour), rowOf(kLan, kBlock, QStringLiteral("something else"))}) {
            Rig r;
            r.notes.note(lineUnder(kFormerLan));
            r.box.table[{kPort, kTcp}] = there;
            CHECK(r.guard.formerAddress() == Former::Gone);
            CHECK(r.box.removes == 0 && r.box.row443() == there && r.notes.lines.isEmpty());
            CHECK(r.claim(wanted()).step == 2);
        }
    }
    {
        // A router that says nothing: nothing is done, the line stays
        Rig r;
        r.notes.note(lineUnder(kFormerLan));
        r.box.table[{kPort, kTcp}] = rowOf(kFormerLan);
        r.box.silent = true;
        CHECK(r.guard.formerAddress() == Former::Unknown);
        CHECK(r.claim(wanted()).step == 2);
        CHECK(r.box.removes == 0 && r.notes.lines.size() == 1 && !r.guard.formerRefused());
    }
    {
        // By the record AND the text, never by the address alone: this
        // installation's words at the former address with no line are not ours
        Rig r;
        r.box.table[{kPort, kTcp}] = rowOf(kFormerLan);
        CHECK(r.guard.formerAddress() == Former::None);
        CHECK(r.claim(wanted()).step == 2);
        CHECK(r.box.removes == 0 && r.box.has443());
    }
}

/// A run up to the dial that proves what it wrote: the entry is on the
/// router, read back, noted, and not held yet.
void toProof(Rig& r)
{
    Step step = r.guard.begin(wanted());
    step = r.guard.after(true);
    step = r.guard.after(false);
    CHECK(!step.over() && r.guard.wrote() && r.box.has443() && !r.guard.held());
}

/// The run a step at a time, as the server makes it: what asks the router
/// between two dials, and what each dial is told.
void inSteps()
{
    SECTION("Tcp443Guard: a step at a time, the run is what the steps in a row are");
    const auto same = [](const std::function<void(Rig&)>& how) {
        Rig row, stepped;
        how(row);
        how(stepped);
        const Verdict whole = row.claim(wanted());
        Step step = stepped.guard.begin(wanted());
        qsizetype dials = 0;
        while (!step.over()) {
            // Between two steps a run is under way, and nothing is held
            CHECK(stepped.guard.underWay() && !stepped.guard.held());
            ++dials;
            step = stepped.guard.after(stepped.dials.make(step.dial));
        }
        CHECK(!stepped.guard.underWay() && !stepped.guard.wrote());
        // The same verdict, the same entry, the same record, the same
        // questions to the router, the same dials in the same order
        CHECK(step.verdict.outcome == whole.outcome && step.verdict.step == whole.step &&
              step.verdict.said == whole.said);
        CHECK(stepped.box.table == row.box.table && stepped.notes.lines == row.notes.lines);
        CHECK(stepped.box.adds == row.box.adds && stepped.box.removes == row.box.removes &&
              stepped.box.lookups == row.box.lookups);
        CHECK(stepped.dials.log == row.dials.log && dials == row.dials.log.size());
        CHECK(stepped.guard.held() == row.guard.held() &&
              stepped.guard.leadsTo() == row.guard.leadsTo());
    };
    same([](Rig&) {});
    same([](Rig& r) { r.dials.loopsBack = false; });
    same([](Rig& r) { r.dials.handMade = true; });
    same([](Rig& r) { r.dials.deaf = true; });
    same([](Rig& r) {
        r.dials.deaf = true;
        r.box.refusesRemove = true;
    });
    same([](Rig& r) { r.box.refusesAdd = true; });
    same([](Rig& r) { r.box.forgetsAdd = true; });
    same([](Rig& r) { r.box.writesTowards = kNeighbour; });
    same([](Rig& r) { r.box.silent = true; });
    same([](Rig& r) {
        r.box.table[{kPort, kTcp}] = rowOf(kNeighbour, 443, QStringLiteral("https"));
    });
    same([](Rig& r) {
        r.notes.note(lineUnder(kLan));
        r.box.table[{kPort, kTcp}] = rowOf(kLan);
    });
    same([](Rig& r) {
        r.notes.note(lineUnder(kLan, uint16_t(kBlock + 3)));
        r.box.table[{kPort, kTcp}] = rowOf(kLan, uint16_t(kBlock + 3));
    });
    same([](Rig& r) {
        r.notes.note(lineUnder(kFormerLan));
        r.box.table[{kPort, kTcp}] = rowOf(kFormerLan);
    });

    SECTION("Tcp443Guard: each dial of a run is told whole, and the router is asked in between");
    {
        Rig r;
        Step step = r.guard.begin(wanted());
        // 3: a frame to the block port at the router's own address, which
        // must come in on that port
        CHECK(step.dial.kind == Dial::Kind::ArrivesHere && step.dial.address == kPublic &&
              step.dial.port == kBlock && step.dial.leadsHereOn == kBlock);
        CHECK(r.box.adds == 0 && r.notes.lines.isEmpty() && !r.guard.wrote());
        step = r.guard.after(true);
        // 4: 443 there, with nothing sent
        CHECK(step.dial.kind == Dial::Kind::Answers && step.dial.address == kPublic &&
              step.dial.port == kPort);
        CHECK(r.box.adds == 0 && r.notes.lines.isEmpty() && !r.guard.wrote());
        step = r.guard.after(false);
        // 5: noted, written and read back; then a frame to 443, which must
        // come in on the block port. Not held before that
        CHECK(step.dial.kind == Dial::Kind::ArrivesHere && step.dial.address == kPublic &&
              step.dial.port == kPort && step.dial.leadsHereOn == kBlock);
        CHECK(r.guard.wrote() && !r.guard.held() && r.guard.leadsTo() == 0);
        CHECK(r.box.row443() == rowOf(kLan) && r.notes.lines == QList<Hole>{lineUnder(kLan)});
        step = r.guard.after(true);
        CHECK(step.over() && step.verdict.outcome == Outcome::Held && step.verdict.step == 5);
        CHECK(r.guard.leadsTo() == kBlock && !r.guard.underWay());
        // An answer no dial was asked for: nothing, and the router is left alone
        const int lookups = r.box.lookups;
        for (const bool result : {true, false}) {
            const Step none = r.guard.after(result);
            CHECK(none.over() && none.verdict.outcome == Outcome::Left && none.verdict.step == 0);
        }
        CHECK(r.box.lookups == lookups && r.box.adds == 1 && r.guard.leadsTo() == kBlock);
    }

    SECTION("Tcp443Guard: what the router has on 443 is asked again just before the write");
    // Somebody's by then: with other words, towards this very machine, or
    // with this installation's very words, which no line says are its own
    for (const Box::Row& there : {rowOf(kNeighbour, 443, QStringLiteral("https")),
                                  rowOf(kLan, 8443, QStringLiteral("my server")), rowOf(kLan)}) {
        Rig r;
        Step step = r.guard.begin(wanted());
        step = r.guard.after(true);
        // It came while the dials were made
        r.box.table[{kPort, kTcp}] = there;
        step = r.guard.after(false);
        CHECK(step.over() && step.verdict.outcome == Outcome::Left && step.verdict.step == 2 &&
              namesNoAddress(step.verdict));
        CHECK(r.box.adds == 0 && r.box.removes == 0 && r.notes.lines.isEmpty());
        CHECK(r.box.row443() == there && !r.guard.underWay() && !r.guard.held());
        CHECK(r.dials.log.isEmpty());
    }
    {
        // The router no longer says: nothing is written into the dark
        Rig r;
        Step step = r.guard.begin(wanted());
        step = r.guard.after(true);
        r.box.silent = true;
        step = r.guard.after(false);
        CHECK(step.over() && step.verdict.outcome == Outcome::Left && step.verdict.step == 2);
        CHECK(r.box.adds == 0 && r.notes.lines.isEmpty() && !r.guard.underWay());
    }

    SECTION("Tcp443Guard: a run that is given up leaves nothing behind");
    for (const int made : {1, 2}) {
        // Before the write, with the first dial asked for or the second:
        // nothing to take back, and the router is asked nothing
        Rig r;
        Step step = r.guard.begin(wanted());
        if (made == 2) step = r.guard.after(true);
        CHECK(!step.over() && r.guard.underWay() && !r.guard.wrote());
        const int lookups = r.box.lookups;
        CHECK(r.guard.abandon() == Guard::Abandoned::Nothing);
        CHECK(!r.guard.underWay() && r.box.lookups == lookups && r.box.adds == 0);
        CHECK(r.notes.lines.isEmpty());
        // Its answer, should it come all the same, moves nothing: a "nothing
        // answers on 443" least of all
        CHECK(r.guard.after(false).verdict.step == 0 && r.guard.after(true).verdict.step == 0);
        CHECK(r.box.lookups == lookups && r.box.adds == 0);
        // And a whole run is made after it as after none
        CHECK(r.claim(wanted()).outcome == Outcome::Held);
    }
    {
        // After the write: removed, the removal read back, the line struck
        Rig r;
        toProof(r);
        CHECK(r.guard.abandon() == Guard::Abandoned::Undone);
        CHECK(!r.box.has443() && r.notes.lines.isEmpty() && r.box.removes == 1);
        CHECK(!r.guard.underWay() && !r.guard.wrote() && !r.guard.held());
        // The proof comes all the same: nothing is held by it
        CHECK(r.guard.after(true).verdict.step == 0 && !r.guard.held() && r.guard.leadsTo() == 0);
        // Twice is nothing
        CHECK(r.guard.abandon() == Guard::Abandoned::Nothing && r.box.removes == 1);
    }
    {
        // ...on a router that will not remove it: the entry and the line stay
        Rig r;
        toProof(r);
        r.box.refusesRemove = true;
        CHECK(r.guard.abandon() == Guard::Abandoned::Stays);
        CHECK(r.box.has443() && r.notes.lines == QList<Hole>{lineUnder(kLan)});
        CHECK(!r.guard.underWay() && !r.guard.held());
    }
    {
        // ...and a run begun after it never takes that entry as held: it is
        // ours by every mark, towards today's block port, and no dial has
        // shown that 443 leads here. Its removal is tried again; kept again,
        // the run is over at step 2 and paced as one that dialled; let go,
        // the run goes on as for a free 443, and its proof is made
        Rig r;
        toProof(r);
        r.box.refusesRemove = true;
        CHECK(r.guard.abandon() == Guard::Abandoned::Stays);
        const int adds = r.box.adds;
        const int removes = r.box.removes;
        Step step = r.guard.begin(wanted());
        CHECK(step.over() && step.apart && step.verdict.outcome == Outcome::Left &&
              step.verdict.step == 2 && namesNoAddress(step.verdict));
        CHECK(!r.guard.held() && r.box.adds == adds && r.box.removes == removes + 1);
        CHECK(r.box.has443() && r.notes.lines.size() == 1);
        r.box.refusesRemove = false;
        step = r.guard.begin(wanted());
        CHECK(!step.over() && !step.apart && step.dial.kind == Dial::Kind::ArrivesHere &&
              step.dial.port == kBlock);
        CHECK(!r.box.has443() && r.notes.lines.isEmpty() && !r.guard.held());
        r.guard.abandon();
        CHECK(r.claim(wanted()).outcome == Outcome::Held && r.guard.leadsTo() == kBlock);
        CHECK(r.box.adds == adds + 1);
    }
    {
        // The same when it was not shown at all as the run was given up: the
        // router says what it has at the next run, and it is taken off then
        Rig r;
        toProof(r);
        r.box.silent = true;
        CHECK(r.guard.abandon() == Guard::Abandoned::Stays);
        r.box.silent = false;
        const Step step = r.guard.begin(wanted());
        CHECK(!step.over() && !r.box.has443() && r.notes.lines.isEmpty() && !r.guard.held());
    }
    {
        // An entry of this installation's that is not the one given up (an
        // earlier one, which its run proved) is held as before
        Rig r;
        r.notes.note(lineUnder(kLan));
        r.box.table[{kPort, kTcp}] = rowOf(kLan);
        const Step step = r.guard.begin(wanted());
        CHECK(step.over() && !step.apart && step.verdict.outcome == Outcome::Held &&
              step.verdict.step == 6 && r.guard.held());
    }
    {
        // ...or that says nothing: the same, and nothing is removed blind
        Rig r;
        toProof(r);
        r.box.silent = true;
        CHECK(r.guard.abandon() == Guard::Abandoned::Stays);
        CHECK(r.box.removes == 0 && r.notes.lines.size() == 1);
    }
    {
        // Another's by then: not removed, and no longer noted as ours
        Rig r;
        toProof(r);
        r.box.table[{kPort, kTcp}] = rowOf(kNeighbour, 443, QStringLiteral("https"));
        CHECK(r.guard.abandon() == Guard::Abandoned::Undone);
        CHECK(r.box.removes == 0 && r.box.has443() && r.notes.lines.isEmpty());
    }
    {
        // No run under way: nothing, and the router is asked nothing
        Rig r;
        CHECK(r.guard.abandon() == Guard::Abandoned::Nothing && r.box.lookups == 0);
    }
    {
        // One run at a time: a run begun while another waits for its proof
        // gives that one up first, and its entry is not left behind
        Rig r;
        toProof(r);
        Situation off = wanted();
        off.wanted = false;
        const Step step = r.guard.begin(off);
        CHECK(step.over() && step.verdict.step == 0);
        CHECK(!r.box.has443() && r.notes.lines.isEmpty() && !r.guard.underWay());
    }
    {
        // The host stops with the entry written and not proved yet: taken
        // off as one that is held is, and said when the router keeps it
        Rig r;
        toProof(r);
        CHECK(r.guard.release());
        CHECK(!r.box.has443() && r.notes.lines.isEmpty() && !r.guard.underWay());
        CHECK(r.guard.after(true).verdict.step == 0 && !r.guard.held());
        Rig stuck;
        toProof(stuck);
        stuck.box.refusesRemove = true;
        CHECK(!stuck.guard.release());
        CHECK(stuck.box.has443() && stuck.notes.lines.size() == 1 && !stuck.guard.underWay());
    }
}

/// When the whole run is made again: whenever, after one that asked the
/// router and stopped; an hour after one that dialled; never, after one that
/// wrote and did not keep what it wrote.
void pacing()
{
    SECTION("Tcp443Guard: when the whole run is made again");
    const uint64_t hour = Pace::kApartMs;
    const auto leftAt = [](int step) { return Verdict{Outcome::Left, step, QStringLiteral("no")}; };
    const Verdict held{Outcome::Held, 5, QStringLiteral("yes")};
    const Verdict lost{Outcome::Lost, 6, QStringLiteral("gone")};
    {
        // At the start: due. A run that stopped before it dialled may be made
        // again whenever the holes are looked at
        Pace p;
        CHECK(p.due(0) && !p.over());
        for (const int step : {0, 1, 2}) {
            p.said(leftAt(step), 1000);
            CHECK(p.due(1000) && !p.over());
        }
    }
    for (const int step : {3, 4}) {
        // One that dialled waits its hour, whatever is said meanwhile
        Pace p;
        p.said(leftAt(step), 1000);
        CHECK(!p.due(1000) && !p.due(1000 + hour - 1) && !p.over());
        p.said(leftAt(2), 2000);
        CHECK(!p.due(2000) && p.due(1000 + hour));
    }
    {
        // One that wrote and did not keep what it wrote is the last
        Pace p;
        p.said(leftAt(5), 1000);
        CHECK(p.over() && !p.due(1000) && !p.due(1000 + 100 * hour));
        p.anew();
        CHECK(p.over() && !p.due(1000 + 100 * hour));
    }
    {
        // Held: none is due, it is renewed. Lost: at once, once; lost again
        // within the hour, it waits; an hour on, that is forgotten
        Pace p;
        p.said(held, 1000);
        CHECK(!p.due(1000) && !p.due(1000 + 100 * hour));
        p.said(lost, 5000);
        CHECK(p.due(5000));
        p.said(held, 6000);
        p.said(lost, 7000);
        CHECK(!p.due(7000) && !p.due(7000 + hour - 1) && p.due(7000 + hour));
        p.said(held, 7000 + hour);
        p.said(lost, 7000 + 3 * hour);
        CHECK(p.due(7000 + 3 * hour) && !p.over());
        // A renewal with nothing held says nothing of the runs
        Pace q;
        q.said(leftAt(6), 10);
        CHECK(q.due(10) && !q.over());
    }
    {
        // One that had its dials made and was stopped by a question to the
        // router (asked again before the write) waits as one a dial stopped;
        // one that never dialled does not
        Pace p;
        p.said(leftAt(2), 1000, true);
        CHECK(!p.due(1000) && !p.due(1000 + hour - 1) && p.due(1000 + hour) && !p.over());
        Pace q;
        q.said(leftAt(2), 1000, false);
        CHECK(q.due(1000));
        // What was written and not kept is the last, however it was found
        Pace last;
        last.said(leftAt(5), 1000, true);
        CHECK(last.over() && !last.due(1000 + 100 * hour));
    }
    {
        // What it stood on changed by this host's own doing: as at the start
        Pace p;
        p.said(leftAt(3), 1000);
        p.anew();
        CHECK(p.due(1000));
        p.said(held, 2000);
        p.anew();
        CHECK(p.due(2000));
    }
    {
        // Given up because the address browsers reach this host at changed:
        // not this host's doing, and paced as a lost entry is. At once, once;
        // so again within the hour, it waits the hour; an hour on, at once
        Pace p;
        p.movedAway(1000);
        CHECK(p.due(1000) && !p.over());
        p.movedAway(2000);
        CHECK(!p.due(2000) && !p.due(2000 + hour - 1) && p.due(2000 + hour));
        p.movedAway(2000 + 2 * hour);
        CHECK(p.due(2000 + 2 * hour));
        // ...and it is not a lost entry's count: one lost after it is at once
        Pace q;
        q.movedAway(1000);
        q.said(lost, 2000);
        CHECK(q.due(2000));
        // Nothing is over by it
        Pace last;
        last.said(leftAt(5), 1000);
        last.movedAway(2000);
        CHECK(last.over() && !last.due(2000 + 100 * hour));
    }
    {
        // With the guard, looked at every half hour as the holes are: a
        // router that does not lead back is dialled once an hour, once each
        Rig r;
        r.dials.loopsBack = false;
        Pace p;
        int runs = 0;
        for (uint64_t tick = 0; tick < 5; ++tick) {
            const uint64_t now = tick * (hour / 2);
            if (!p.due(now)) continue;
            p.said(r.claim(wanted()), now);
            ++runs;
        }
        CHECK_EQ(runs, 3);
        CHECK_EQ(r.dials.log.size(), qsizetype(3));
        CHECK(r.box.adds == 0);
    }
    {
        // ...and one that writes the entry towards another machine is asked
        // once, and never again by this run of the server
        Rig r;
        r.box.writesTowards = kNeighbour;
        Pace p;
        int runs = 0;
        for (uint64_t tick = 0; tick < 5; ++tick) {
            if (!p.due(tick * hour)) continue;
            const Verdict v = r.claim(wanted());
            CHECK(v.outcome == Outcome::Left && v.step == 5 && namesNoAddress(v));
            p.said(v, tick * hour);
            ++runs;
        }
        CHECK_EQ(runs, 1);
        CHECK(p.over() && r.box.adds == 1);
    }
}

/// What the admin page is told of an entry the router will not remove: a yes
/// or a no, for the admin alone.
void adminNotice()
{
    SECTION("Tcp443Guard: what the admin page is told, and nobody else");
    const QLatin1String key(kStaleRefusedKey);
    QJsonObject status{{QStringLiteral("active"), true}};
    tellAdmin(status, true, true);
    CHECK(status.value(key).isBool() && status.value(key).toBool());
    tellAdmin(status, true, false);
    CHECK(status.value(key).isBool() && !status.value(key).toBool());
    // Nothing but that yes or no goes with it
    CHECK_EQ(status.size(), qsizetype(2));
    // Whoever is not the admin gets no such key, whatever is so and whatever
    // the status held before
    QJsonObject other{{QStringLiteral("active"), true}};
    tellAdmin(other, false, true);
    CHECK(!other.contains(key) && other.size() == 1);
    tellAdmin(status, true, true);
    tellAdmin(status, false, true);
    CHECK(!status.contains(key) && status.value(QStringLiteral("active")).toBool());

    // From the guard: yes when the router refuses the removal, no once the
    // entry is gone
    Rig r;
    r.notes.note(lineUnder(kFormerLan));
    r.box.table[{kPort, kTcp}] = rowOf(kFormerLan);
    r.box.refusesRemove = true;
    CHECK(r.guard.formerAddress() == Guard::Former::Refused);
    QJsonObject told;
    tellAdmin(told, true, r.guard.formerRefused());
    CHECK(told.value(key).isBool() && told.value(key).toBool());
    r.box.refusesRemove = false;
    CHECK(r.guard.formerAddress() == Guard::Former::Removed);
    tellAdmin(told, true, r.guard.formerRefused());
    CHECK(told.value(key).isBool() && !told.value(key).toBool());
    // And what is said holds no address, no port and no text of the router's
    const QString said = QString::fromUtf8(QJsonDocument(told).toJson(QJsonDocument::Compact));
    CHECK_EQ(said, QStringLiteral("{\"router443_stale_removal_refused\":false}"));
}

/// What stands for the thread the dials are made on: it keeps the dial it is
/// asked for until the test has it answered, and counts what the keeper must
/// never do to it.
struct Switchboard : IDialler
{
    struct Asked
    {
        uint32_t run = 0;
        Dial dial;
    };
    QList<Asked> asked; ///< every dial it was asked for, in order
    bool has = false;   ///< one is with it, not answered yet
    int cancels = 0;
    int overlaps = 0;     ///< asked for one while it had another: never
    bool refuses = false; ///< takes no dial at all

    bool ask(uint32_t run, const Dial& dial) override
    {
        if (refuses) return false;
        if (has) ++overlaps;
        has = true;
        asked.append({run, dial});
        return true;
    }
    void cancel() override { ++cancels; }
};

/// A router, a record, what dialling finds, and the keeper over them, with
/// what it asked the router of its own address and what it said.
struct Watch
{
    Box box;
    Notes notes;
    Dials dials{box};
    Switchboard board;
    int asked = 0;
    /// What the router says its own outer address is
    QString outer = kPublic;
    Keeper keeper{box, &notes, board, [this]() {
                      ++asked;
                      return outer;
                  }};
    QStringList warnings, infos;
    /// Each dial is answered as soon as it is asked for, with what `dials`
    /// finds: a look then ends its run, as when nothing else comes meanwhile.
    /// Off, the test answers each dial itself, when it chooses (answer()).
    bool straight = true;

    void take(const QList<Keeper::Line>& lines)
    {
        for (const Keeper::Line& line : lines)
            (line.warning ? warnings : infos) << line.text;
    }
    /// One look, with what the caller knows: no router address of its own
    void look(bool ask, bool wantedNow = true, uint16_t blockPort = kBlock, uint64_t nowMs = 0)
    {
        Situation now;
        now.wanted = wantedNow;
        now.publicAddress = kPublic;
        now.blockPort = blockPort;
        now.leaseSec = 3600;
        lookAt(now, ask, nowMs);
    }
    /// One look at a situation given whole
    void lookAt(const Situation& now, bool ask, uint64_t nowMs = 0)
    {
        take(keeper.look(now, ask, nowMs));
        while (straight && board.has)
            answerTruly(nowMs);
    }
    /// The dial the dialler has is answered: the one it was asked for last
    void answer(bool result, uint64_t nowMs = 0)
    {
        board.has = false;
        take(keeper.dialled(board.asked.last().run, result, nowMs));
    }
    /// ...with what dialling would find
    void answerTruly(uint64_t nowMs = 0) { answer(dials.make(board.asked.last().dial), nowMs); }
    bool saysNoAddress() const
    {
        for (const QString& line : warnings + infos)
            if (line.contains(QStringLiteral("192.168")) || line.contains(kPublic) ||
                line.contains(kTag))
                return false;
        return true;
    }
};

/// The guard as it is run, look after look: what is due each time.
void keeping()
{
    const uint64_t hour = Pace::kApartMs;
    SECTION("Tcp443Guard: looked at again and again, it does what is due and no more");
    {
        // Switched off, and nothing held: the router is asked nothing at all
        Watch w;
        w.look(false);
        w.look(false, false, 0);
        CHECK(w.box.lookups == 0 && w.box.adds == 0 && w.asked == 0 && w.dials.log.isEmpty());
        CHECK(w.infos.isEmpty() && w.warnings.isEmpty() && w.keeper.leadsTo() == 0);
        // Asked, with nothing to ask for: not wanted, or no port for it to
        // lead to. No run, no dial, no line.
        w.look(true, false);
        w.look(true, true, 0);
        CHECK(w.box.adds == 0 && w.asked == 0 && w.dials.log.isEmpty() && w.infos.isEmpty());
    }
    {
        // A run when one is due: held, said once, and the probe was told
        // which port a connection that leads here comes in on
        Watch w;
        w.look(true);
        CHECK(w.keeper.leadsTo() == kBlock && w.box.row443() == rowOf(kLan));
        CHECK(w.dials.toldPort == kBlock && w.asked == 1);
        CHECK(w.infos.size() == 1 && w.infos.first().contains(QStringLiteral("48550")));
        CHECK(w.warnings.isEmpty());
        // Looked at again: renewed, with no dial and nothing to say
        const qsizetype dialled = w.dials.log.size();
        w.look(true, true, kBlock, 1000);
        CHECK(w.box.adds == 2 && w.dials.log.size() == dialled && w.infos.size() == 1);
        CHECK(w.keeper.leadsTo() == kBlock);

        // The streams' shared port moved: taken off, by this host's own
        // doing, and asked for towards the new one in the same look
        w.look(true, true, 48551, 2000);
        CHECK(w.keeper.leadsTo() == 48551 && w.box.row443() == rowOf(kLan, 48551));
        CHECK(w.warnings.isEmpty() && w.infos.size() == 3);
        CHECK(w.dials.toldPort == 48551);
        // It lost its hole: taken off, and nothing is asked for
        w.look(true, true, 0, 3000);
        CHECK(w.keeper.leadsTo() == 0 && !w.box.has443() && w.notes.lines.isEmpty());
        CHECK(w.warnings.isEmpty() && w.infos.size() == 4);
        // Back: asked for at once, an hour has not to go by
        w.look(true, true, kBlock, 4000);
        CHECK(w.keeper.leadsTo() == kBlock && w.infos.size() == 5);

        // UPnP or Internet Access switched off: taken back, as every hole
        // is, and the router is asked nothing beyond that
        const int askedBefore = w.asked;
        w.look(false, false, kBlock, 5000);
        CHECK(w.keeper.leadsTo() == 0 && !w.box.has443() && w.notes.lines.isEmpty());
        CHECK(w.asked == askedBefore && w.warnings.isEmpty() && w.infos.size() == 6);
        const int lookups = w.box.lookups;
        w.look(false, false, kBlock, 6000);
        CHECK(w.box.lookups == lookups);
        CHECK(w.saysNoAddress());
    }
    {
        // Switched off, by a caller whose situation is whole otherwise (still
        // wanted, the same port, the router's own address named): taken off
        // all the same, and nothing is written again
        Watch w;
        w.look(true);
        const int adds = w.box.adds;
        w.lookAt(wanted(), false, 1000);
        CHECK(w.keeper.leadsTo() == 0 && !w.box.has443() && w.notes.lines.isEmpty());
        CHECK(w.box.adds == adds && w.box.removes == 1 && w.warnings.isEmpty());
        CHECK(w.infos.size() == 2 && w.saysNoAddress());
    }
    {
        // Switched off, and the router will not remove the entry: a warning
        // that it is still there, never that it is taken off; the line stays
        // in the record for the next start, and nothing more is asked
        Watch w;
        w.look(true);
        w.box.refusesRemove = true;
        const int adds = w.box.adds;
        w.look(false, false, kBlock, 1000);
        CHECK(w.warnings.size() == 1 && w.infos.size() == 1);
        CHECK(w.warnings.first().contains(QStringLiteral("could not be taken off")) &&
              w.warnings.first().contains(QStringLiteral("step 6")) &&
              !w.warnings.first().contains(QStringLiteral("is taken off")));
        CHECK(w.box.has443() && w.notes.lines.size() == 1 && w.box.adds == adds);
        CHECK(w.keeper.leadsTo() == 0 && w.saysNoAddress());
        const int lookups = w.box.lookups;
        w.look(false, false, kBlock, 2000);
        CHECK(w.box.lookups == lookups && w.warnings.size() == 1);
    }
    {
        // A router that does not lead back: dialled once, said once, and
        // dialled again an hour later and no sooner, with nothing more said
        Watch w;
        w.dials.loopsBack = false;
        w.look(true, true, kBlock, 0);
        CHECK(w.dials.log.size() == 1 && w.infos.size() == 1 && w.warnings.isEmpty());
        CHECK(w.infos.first().contains(QStringLiteral("step 3")));
        w.look(true, true, kBlock, hour - 1);
        CHECK(w.dials.log.size() == 1 && w.infos.size() == 1);
        w.look(true, true, kBlock, hour);
        CHECK(w.dials.log.size() == 2 && w.infos.size() == 1);
        // It leads back now: held
        w.dials.loopsBack = true;
        w.look(true, true, kBlock, 2 * hour);
        CHECK(w.keeper.leadsTo() == kBlock && w.infos.size() == 2);
    }
    {
        // Somebody's own 443: asked about at every look, said once, never
        // dialled; another reason is said when it comes
        Watch w;
        w.box.table[{kPort, kTcp}] = rowOf(kNeighbour, 443, QStringLiteral("https"));
        for (uint64_t at = 0; at < 3; ++at)
            w.look(true, true, kBlock, at * 1000);
        CHECK(w.dials.log.isEmpty() && w.box.adds == 0 && w.infos.size() == 1);
        CHECK(w.infos.first().contains(QStringLiteral("step 2")));
        w.box.table.clear();
        w.dials.handMade = true;
        w.look(true, true, kBlock, 4000);
        CHECK(w.infos.size() == 2 && w.infos.last().contains(QStringLiteral("step 4")));
        CHECK(w.saysNoAddress());
    }
    {
        // A router that writes the entry towards another machine: a warning,
        // and no run again by this run of the server
        Watch w;
        w.box.writesTowards = kNeighbour;
        w.look(true);
        CHECK(w.keeper.leadsTo() == 0 && w.warnings.size() == 1 && w.infos.isEmpty());
        CHECK(w.warnings.first().contains(QStringLiteral("step 5")) &&
              w.warnings.first().contains(QStringLiteral("until the server restarts")));
        const int adds = w.box.adds;
        const qsizetype dialled = w.dials.log.size();
        w.box.writesTowards.clear();
        for (uint64_t at = 1; at < 5; ++at)
            w.look(true, true, kBlock, at * hour);
        CHECK(w.box.adds == adds && w.dials.log.size() == dialled && w.warnings.size() == 1);
        CHECK(w.saysNoAddress());
    }
    {
        // Held, and the router has forgotten it (it was restarted): a
        // warning, and asked for again in the same look, once
        Watch w;
        w.look(true);
        w.box.table.clear();
        w.look(true, true, kBlock, 1000);
        CHECK(w.warnings.size() == 1 && w.keeper.leadsTo() == kBlock && w.box.has443());
        // Forgotten again within the hour: said, and not asked for before
        // the hour is over
        w.box.table.clear();
        const qsizetype dialled = w.dials.log.size();
        w.look(true, true, kBlock, 2000);
        CHECK(w.warnings.size() == 2 && w.keeper.leadsTo() == 0 && !w.box.has443());
        w.look(true, true, kBlock, 2000 + hour - 1);
        CHECK(w.dials.log.size() == dialled && !w.box.has443());
        w.look(true, true, kBlock, 2000 + hour);
        CHECK(w.keeper.leadsTo() == kBlock);
    }
    {
        // Held, and somebody else's at the next look: a warning, nothing
        // written towards anything, and 443 is left as theirs
        Watch w;
        w.look(true);
        w.box.table[{kPort, kTcp}] = rowOf(kNeighbour);
        const int adds = w.box.adds;
        w.look(true, true, kBlock, 1000);
        CHECK(w.warnings.size() == 1 && w.keeper.leadsTo() == 0);
        CHECK(w.box.adds == adds && w.box.removes == 0 && w.box.row443() == rowOf(kNeighbour));
        CHECK(w.notes.lines.isEmpty());
    }
    {
        // An entry under an address this machine no longer has, which the
        // router will not remove: looked for whatever the switches say, said
        // once however often it is looked at, and never with the address
        Watch w;
        w.notes.note(lineUnder(kFormerLan));
        w.box.table[{kPort, kTcp}] = rowOf(kFormerLan);
        w.box.refusesRemove = true;
        for (uint64_t at = 0; at < 3; ++at)
            w.look(true, false, 0, at * 1000);
        CHECK(w.warnings.size() == 1 && w.keeper.formerRefused());
        CHECK(w.box.removes == 3 && w.saysNoAddress());
        // Not looked for while the router is not to be asked
        const int lookups = w.box.lookups;
        w.look(false, false, 0, 4000);
        CHECK(w.box.lookups == lookups && w.keeper.formerRefused());
        // Removed at last, by the router's own leave: no longer so, and with
        // everything else as it should be 443 is asked for
        w.box.refusesRemove = false;
        w.look(true, true, kBlock, 5000);
        CHECK(!w.keeper.formerRefused() && w.keeper.leadsTo() == kBlock);
        CHECK(w.box.row443() == rowOf(kLan) && w.warnings.size() == 1);
    }
    {
        // The server stops: taken off, read back, and nothing to say of it
        Watch w;
        w.look(true);
        CHECK(w.keeper.release().isEmpty());
        CHECK(!w.box.has443() && w.notes.lines.isEmpty() && w.keeper.leadsTo() == 0);
        // Its dials were asked for one at a time, and none was cancelled
        CHECK(w.board.asked.size() == 3 && w.board.overlaps == 0 && w.board.cancels == 0);
    }
}

/// A keeper whose run has its entry written, read back and noted, and waits
/// for the dial that proves it.
void toProof(Watch& w)
{
    w.straight = false;
    w.look(true, true, kBlock, 0);
    w.answer(true, 100);
    w.answer(false, 200);
    CHECK(w.keeper.running() && w.board.asked.size() == 3 && w.board.has);
    CHECK(w.box.has443() && w.notes.lines.size() == 1 && w.keeper.leadsTo() == 0);
}

/// The keeper while its dials are made elsewhere: one run, one dial, and an
/// answer that moves the run it was asked for and no other.
void dialling()
{
    const uint64_t hour = Pace::kApartMs;
    SECTION("Tcp443Guard: a run goes on a dial at a time, and its thread is free in between");
    {
        Watch w;
        w.straight = false;
        w.look(true);
        // Begun: the router was asked, nothing is written, and one dial is
        // with the dialler
        CHECK(w.keeper.running() && w.board.asked.size() == 1 && w.board.has);
        CHECK(w.board.asked.last().dial.kind == Dial::Kind::ArrivesHere &&
              w.board.asked.last().dial.port == kBlock);
        CHECK(w.box.adds == 0 && w.infos.isEmpty() && w.warnings.isEmpty());
        CHECK(w.keeper.leadsTo() == 0);
        // Looked at again with nothing changed: one run at a time. Nothing
        // is begun, the router is asked nothing, nothing is said
        const int lookups = w.box.lookups;
        const int asked = w.asked;
        w.look(true, true, kBlock, 500);
        w.look(true, true, kBlock, 900);
        CHECK(w.board.asked.size() == 1 && w.box.lookups == lookups && w.asked == asked);
        CHECK(w.infos.isEmpty() && w.board.cancels == 0);
        // Each answer brings the next dial of the same run, and only then
        const uint32_t run = w.board.asked.first().run;
        w.answer(true, 1000);
        CHECK(w.board.asked.size() == 2 && w.board.asked.last().run == run);
        CHECK(w.board.asked.last().dial.kind == Dial::Kind::Answers && w.box.adds == 0);
        w.answer(false, 2000);
        CHECK(w.board.asked.size() == 3 && w.board.asked.last().run == run);
        // Written, and not held before it is proved; nothing said yet
        CHECK(w.box.row443() == rowOf(kLan) && w.keeper.leadsTo() == 0 && w.infos.isEmpty());
        w.answer(true, 3000);
        CHECK(!w.keeper.running() && w.keeper.leadsTo() == kBlock && w.infos.size() == 1);
        CHECK(w.board.overlaps == 0 && w.board.cancels == 0 && !w.board.has);
        // Held: renewed at the next look, with no dial
        w.look(true, true, kBlock, 4000);
        CHECK(w.board.asked.size() == 3 && w.box.adds == 2 && w.infos.size() == 1);
        CHECK(w.saysNoAddress());
    }

    SECTION("Tcp443Guard: an answer that is not the dial under way is nothing");
    {
        Watch w;
        w.straight = false;
        w.look(true);
        const uint32_t run = w.board.asked.last().run;
        const int lookups = w.box.lookups;
        for (const uint32_t other : {uint32_t(0), run + 1, run + 77}) {
            w.take(w.keeper.dialled(other, true, 100));
            w.take(w.keeper.dialled(other, false, 100));
        }
        CHECK(w.keeper.running() && w.board.asked.size() == 1 && w.box.lookups == lookups);
        CHECK(w.infos.isEmpty() && w.warnings.isEmpty());
        // Its own answer, and that answer once more: the second is nothing
        w.answer(false, 200);
        CHECK(!w.keeper.running() && w.infos.size() == 1);
        w.take(w.keeper.dialled(run, true, 300));
        CHECK(!w.keeper.running() && w.board.asked.size() == 1 && w.infos.size() == 1);
        CHECK(w.box.adds == 0);
    }

    SECTION("Tcp443Guard: switched off while the first dial is made");
    {
        Watch w;
        w.straight = false;
        w.look(true, true, kBlock, 0);
        const uint32_t first = w.board.asked.last().run;
        const int lookups = w.box.lookups;
        // Off: the run is given up and its dial cancelled. Nothing was
        // written, the router is asked nothing, and nothing is said
        w.look(false, false, kBlock, 1000);
        CHECK(!w.keeper.running() && w.board.cancels == 1 && w.board.asked.size() == 1);
        CHECK(w.box.lookups == lookups && w.box.adds == 0 && w.notes.lines.isEmpty());
        CHECK(w.infos.isEmpty() && w.warnings.isEmpty());
        // On again before the cancelled dial is back: a run is begun, and
        // its dial waits. Never two with the dialler
        w.look(true, true, kBlock, 2000);
        CHECK(w.keeper.running() && w.board.asked.size() == 1 && w.board.has);
        CHECK(w.board.overlaps == 0);
        // The cancelled dial is back, with its no. Dropped: it does not end
        // the new run at its step 3, is not said, and does not make that run
        // wait an hour. Only now is the new run's dial made
        w.answer(false, 2100);
        CHECK(w.keeper.running() && w.board.asked.size() == 2 && w.board.has);
        CHECK(w.board.asked.last().run != first &&
              w.board.asked.last().dial.kind == Dial::Kind::ArrivesHere);
        CHECK(w.infos.isEmpty() && w.warnings.isEmpty());
        CHECK(w.board.overlaps == 0 && w.board.cancels == 1);
        // That run goes on to its end
        w.answerTruly(2200);
        w.answerTruly(2300);
        w.answerTruly(2400);
        CHECK(w.keeper.leadsTo() == kBlock && w.infos.size() == 1 && w.board.overlaps == 0);
        CHECK(w.board.asked.size() == 4 && w.board.cancels == 1);
    }
    for (const bool late : {false, true}) {
        // The same, and the cancelled dial is back before anything is
        // switched on again: dropped, with a yes as with a no. The pace
        // heard nothing of it: the next look begins a run at once
        Watch w;
        w.straight = false;
        w.look(true, true, kBlock, 0);
        w.look(false, false, kBlock, 1000);
        w.answer(late, 1100);
        CHECK(!w.keeper.running() && w.board.asked.size() == 1 && !w.board.has);
        CHECK(w.box.adds == 0 && w.infos.isEmpty() && w.warnings.isEmpty());
        w.look(true, true, kBlock, 1200);
        CHECK(w.keeper.running() && w.board.asked.size() == 2 && w.board.cancels == 1);
    }

    SECTION("Tcp443Guard: given up after its entry was written, a run takes it off");
    {
        Watch w;
        toProof(w);
        w.look(false, false, kBlock, 1000);
        CHECK(!w.keeper.running() && w.board.cancels == 1);
        CHECK(!w.box.has443() && w.notes.lines.isEmpty() && w.box.removes == 1);
        // Said as of an entry that was held and is taken off, and no warning
        CHECK(w.infos.size() == 1 && w.infos.first().contains(QStringLiteral("is taken off")));
        CHECK(w.warnings.isEmpty());
        // The proof comes all the same: dropped, and nothing is held by it
        w.answer(true, 1100);
        CHECK(w.keeper.leadsTo() == 0 && !w.box.has443() && w.box.adds == 1);
        CHECK(w.infos.size() == 1 && w.saysNoAddress());
    }
    {
        // On a router that will not remove it: a warning that it is still
        // there, never that it is taken off; the line stays in the record
        Watch w;
        toProof(w);
        w.box.refusesRemove = true;
        w.look(false, false, kBlock, 1000);
        CHECK(w.warnings.size() == 1 && w.infos.isEmpty() && !w.keeper.running());
        CHECK(w.warnings.first().contains(QStringLiteral("could not be taken off")) &&
              w.warnings.first().contains(QStringLiteral("step 5")) &&
              !w.warnings.first().contains(QStringLiteral("is taken off")));
        CHECK(w.box.has443() && w.notes.lines.size() == 1 && w.keeper.leadsTo() == 0);
        w.answer(true, 1100);
        CHECK(w.keeper.leadsTo() == 0 && w.warnings.size() == 1 && w.saysNoAddress());
    }

    SECTION("Tcp443Guard: an entry this run wrote and could not prove is never taken as held");
    {
        // Given up while it waited for its proof, on a router that keeps it.
        // Looked at again: the entry is ours by every mark, towards today's
        // block port, and still it is not held, renewed or written again.
        // Its removal is tried again, kept again: left at step 2, and the
        // next try waits the hour
        Watch w;
        toProof(w);
        w.box.refusesRemove = true;
        w.look(false, false, kBlock, 1000);
        w.answer(true, 1100);
        CHECK(w.box.has443() && w.notes.lines.size() == 1 && w.warnings.size() == 1);
        w.look(true, true, kBlock, 2000);
        CHECK(!w.keeper.running() && w.keeper.leadsTo() == 0 && w.box.has443());
        CHECK(w.box.adds == 1 && w.box.removes == 2 && w.board.asked.size() == 3);
        CHECK(w.infos.size() == 1 && w.infos.last().contains(QStringLiteral("could not prove")) &&
              w.infos.last().contains(QStringLiteral("step 2")));
        // Within the hour the router is asked nothing at all, and nothing is
        // dialled
        const int lookups = w.box.lookups;
        const int removes = w.box.removes;
        const int asked = w.asked;
        w.look(true, true, kBlock, 2000 + hour / 2);
        w.look(true, true, kBlock, 2000 + hour - 1);
        CHECK(w.box.lookups == lookups && w.box.removes == removes && w.asked == asked);
        CHECK(w.board.asked.size() == 3 && w.keeper.leadsTo() == 0 && w.box.adds == 1);
        // An hour on the router lets it go: taken off, and a whole run is
        // made, with its three dials, as for a free 443
        w.box.refusesRemove = false;
        w.look(true, true, kBlock, 2000 + hour);
        CHECK(w.box.removes == removes + 1 && !w.box.has443() && w.notes.lines.isEmpty());
        CHECK(w.keeper.running() && w.board.asked.size() == 4);
        CHECK(w.board.asked.last().dial.kind == Dial::Kind::ArrivesHere &&
              w.board.asked.last().dial.port == kBlock);
        w.answerTruly(2000 + hour + 100);
        w.answerTruly(2000 + hour + 200);
        w.answerTruly(2000 + hour + 300);
        CHECK(w.board.asked.size() == 6 && w.box.adds == 2 && w.keeper.leadsTo() == kBlock);
        CHECK(w.box.row443() == rowOf(kLan) && w.notes.lines.size() == 1);
        CHECK(w.board.overlaps == 0 && w.saysNoAddress());
    }
    {
        // The router lets it go at the very next look: taken off, and the
        // run that is due at once is a whole one, with its three dials
        Watch w;
        toProof(w);
        w.box.refusesRemove = true;
        w.look(false, false, kBlock, 1000);
        w.answer(true, 1100);
        w.box.refusesRemove = false;
        w.look(true, true, kBlock, 2000);
        CHECK(w.keeper.running() && w.board.asked.size() == 4 && w.keeper.leadsTo() == 0);
        CHECK(!w.box.has443() && w.notes.lines.isEmpty() && w.box.adds == 1);
        w.answerTruly(2100);
        w.answerTruly(2200);
        w.answerTruly(2300);
        CHECK(w.board.asked.size() == 6 && w.box.adds == 2 && w.keeper.leadsTo() == kBlock);
    }
    {
        // The same with the dialler refusing the proof's dial: the run is
        // given up, the entry the router keeps is not taken as held later
        Watch w;
        w.straight = false;
        w.look(true, true, kBlock, 0);
        w.answer(true, 100);
        w.box.refusesRemove = true;
        w.board.refuses = true;
        w.answer(false, 200);
        CHECK(!w.keeper.running() && w.box.has443() && w.notes.lines.size() == 1);
        CHECK(w.box.adds == 1 && w.keeper.leadsTo() == 0);
        // No run is begun again by this run of the server: nothing is renewed
        w.board.refuses = false;
        w.look(true, true, kBlock, 3 * hour);
        CHECK(w.keeper.leadsTo() == 0 && w.box.adds == 1 && w.board.asked.size() == 2);
    }
    {
        // One that was held and proved, then lost to a router that said
        // nothing, is taken as held again: it had its proof
        Watch w;
        w.look(true, true, kBlock, 0);
        CHECK(w.keeper.leadsTo() == kBlock && w.box.adds == 1);
        w.box.silent = true;
        w.look(true, true, kBlock, 1000);
        CHECK(w.keeper.leadsTo() == 0 && w.notes.lines.size() == 1);
        w.box.silent = false;
        w.look(true, true, kBlock, 2000);
        CHECK(w.keeper.leadsTo() == kBlock && w.board.asked.size() == 3 && w.box.adds == 2);
    }

    SECTION("Tcp443Guard: what a run stands on changes while it is under way");
    {
        // The streams' shared port moved while 443 was dialled in silence:
        // the run is given up, and one is begun towards the new port, whose
        // first dial waits for the old one to be back
        Watch w;
        w.straight = false;
        w.look(true, true, kBlock, 0);
        w.answer(true, 100);
        w.look(true, true, 48551, 1000);
        CHECK(w.board.cancels == 1 && w.keeper.running() && w.board.asked.size() == 2);
        // The old dial's "nothing answers on 443" writes nothing towards the
        // port that was left
        w.answer(false, 1100);
        CHECK(w.box.adds == 0 && w.notes.lines.isEmpty() && w.board.asked.size() == 3);
        CHECK(w.board.asked.last().dial.kind == Dial::Kind::ArrivesHere &&
              w.board.asked.last().dial.port == 48551 &&
              w.board.asked.last().dial.leadsHereOn == 48551);
        w.answerTruly(1200);
        w.answerTruly(1300);
        w.answerTruly(1400);
        CHECK(w.keeper.leadsTo() == 48551 && w.box.row443() == rowOf(kLan, 48551));
        CHECK(w.infos.size() == 1 && w.warnings.isEmpty() && w.board.overlaps == 0);
    }
    {
        // It lost its hole, with the entry written: taken off, and nothing
        // is begun
        Watch w;
        toProof(w);
        w.look(true, true, 0, 1000);
        CHECK(!w.keeper.running() && !w.box.has443() && w.notes.lines.isEmpty());
        CHECK(w.board.cancels == 1 && w.board.asked.size() == 3 && w.infos.size() == 1);
        w.answer(true, 1100);
        CHECK(w.board.asked.size() == 3 && w.keeper.leadsTo() == 0);
    }
    {
        // The address browsers reach this host at is another: given up, and
        // what stands now is looked at (here, not the router's own: step 1)
        Watch w;
        w.straight = false;
        w.look(true, true, kBlock, 0);
        Situation moved = wanted();
        moved.routerAddress.clear();
        moved.publicAddress = QStringLiteral("5.6.7.8");
        w.lookAt(moved, true, 1000);
        CHECK(w.board.cancels == 1 && !w.keeper.running());
        CHECK(w.infos.size() == 1 && w.infos.first().contains(QStringLiteral("step 1")));
        w.answer(true, 1100);
        CHECK(w.board.asked.size() == 1 && w.box.adds == 0 && w.infos.size() == 1);
    }
    {
        // ...and it flips back and forth. That is not this host's doing: the
        // first give-up may begin a run at once, at the new address; a second
        // one within the hour begins nothing until the hour is over
        Watch w;
        w.straight = false;
        w.look(true, true, kBlock, 0);
        Situation moved = wanted();
        moved.routerAddress.clear();
        moved.publicAddress = QStringLiteral("5.6.7.8");
        w.outer = moved.publicAddress;
        w.lookAt(moved, true, 1000);
        CHECK(w.board.cancels == 1 && w.keeper.running() && w.board.asked.size() == 1);
        // Its first dial is made once the cancelled one is back
        w.answer(false, 1100);
        CHECK(w.board.asked.size() == 2 &&
              w.board.asked.last().dial.address == moved.publicAddress);
        // Back again, within the hour
        w.outer = kPublic;
        w.look(true, true, kBlock, 2000);
        CHECK(w.board.cancels == 2 && !w.keeper.running());
        w.answer(false, 2100);
        const int lookups = w.box.lookups;
        const int asked = w.asked;
        w.look(true, true, kBlock, 2000 + hour / 2);
        w.look(true, true, kBlock, 2000 + hour - 1);
        CHECK(!w.keeper.running() && w.board.asked.size() == 2);
        CHECK(w.box.lookups == lookups && w.asked == asked && w.box.adds == 0);
        CHECK(w.infos.isEmpty() && w.warnings.isEmpty() && w.board.overlaps == 0);
        // The hour is over: a run is begun
        w.look(true, true, kBlock, 2000 + hour);
        CHECK(w.keeper.running() && w.board.asked.size() == 3);
        CHECK(w.board.asked.last().dial.address == kPublic);
    }
    {
        // A switch flipped back and forth is this host's own doing: each
        // give-up may begin a run at once
        Watch w;
        w.straight = false;
        w.look(true, true, kBlock, 0);
        for (uint64_t at = 1000; at <= 4000; at += 1000) {
            w.look(true, false, kBlock, at);
            w.answer(false, at + 100);
            w.look(true, true, kBlock, at + 200);
            CHECK(w.keeper.running());
        }
        CHECK(w.board.asked.size() == 5 && w.board.cancels == 4 && w.board.overlaps == 0);
    }
    {
        // 443 is no longer wanted, and the router may still be asked: given
        // up, and nothing is begun
        Watch w;
        w.straight = false;
        w.look(true, true, kBlock, 0);
        w.look(true, false, kBlock, 1000);
        CHECK(w.board.cancels == 1 && !w.keeper.running() && w.infos.isEmpty());
        w.answer(true, 1100);
        CHECK(w.board.asked.size() == 1 && w.box.adds == 0);
    }

    SECTION("Tcp443Guard: a run is paced from the time it ends at");
    {
        Watch w;
        w.straight = false;
        w.look(true, true, kBlock, 0);
        // Known at 5000 that the router does not lead back
        w.answer(false, 5000);
        CHECK(w.infos.size() == 1 && w.infos.first().contains(QStringLiteral("step 3")));
        w.look(true, true, kBlock, hour + 4999);
        CHECK(w.board.asked.size() == 1);
        w.look(true, true, kBlock, hour + 5000);
        CHECK(w.board.asked.size() == 2 && w.keeper.running());
    }
    {
        // One that had both dials made, and was stopped by what the router
        // said when asked again before the write, waits its hour as well
        Watch w;
        w.straight = false;
        w.look(true, true, kBlock, 0);
        w.answer(true, 100);
        w.box.table[{kPort, kTcp}] = rowOf(kNeighbour, 443, QStringLiteral("https"));
        w.answer(false, 200);
        CHECK(!w.keeper.running() && w.box.adds == 0 && w.notes.lines.isEmpty());
        CHECK(w.infos.size() == 1 && w.infos.first().contains(QStringLiteral("step 2")));
        w.box.table.clear();
        w.look(true, true, kBlock, 200 + hour - 1);
        CHECK(w.board.asked.size() == 2 && !w.keeper.running());
        w.look(true, true, kBlock, 200 + hour);
        CHECK(w.board.asked.size() == 3 && w.keeper.running());
    }

    SECTION("Tcp443Guard: the server stops while a run is under way");
    {
        // While the first dial is made: cancelled, and the router is asked
        // nothing
        Watch w;
        w.straight = false;
        w.look(true);
        const int lookups = w.box.lookups;
        w.take(w.keeper.release());
        CHECK(!w.keeper.running() && w.board.cancels == 1 && w.box.lookups == lookups);
        CHECK(w.infos.isEmpty() && w.warnings.isEmpty());
        w.answer(true, 100);
        CHECK(w.board.asked.size() == 1 && w.box.adds == 0);
    }
    {
        // With its entry written: taken off, the removal read back, and
        // nothing to say of it
        Watch w;
        toProof(w);
        w.take(w.keeper.release());
        CHECK(!w.box.has443() && w.notes.lines.isEmpty() && w.board.cancels == 1);
        CHECK(w.infos.isEmpty() && w.warnings.isEmpty() && !w.keeper.running());
        w.answer(true, 1000);
        CHECK(w.keeper.leadsTo() == 0 && !w.box.has443() && w.box.adds == 1);
    }
    {
        // ...which the router keeps: a warning, and the line stays for the
        // next start to take the entry back
        Watch w;
        toProof(w);
        w.box.refusesRemove = true;
        w.take(w.keeper.release());
        CHECK(w.warnings.size() == 1 && w.infos.isEmpty());
        CHECK(w.warnings.first().contains(QStringLiteral("could not be taken off")) &&
              w.warnings.first().contains(QStringLiteral("step 5")));
        CHECK(w.box.has443() && w.notes.lines.size() == 1 && w.saysNoAddress());
    }
    {
        // Held, and the router keeps it: said as well
        Watch w;
        w.look(true);
        w.box.refusesRemove = true;
        w.take(w.keeper.release());
        CHECK(w.warnings.size() == 1 && w.warnings.first().contains(QStringLiteral("step 6")));
        CHECK(w.box.has443() && w.notes.lines.size() == 1 && w.keeper.leadsTo() == 0);
    }
    {
        // Nothing held and nothing under way: the router is asked nothing,
        // whatever the record holds
        Watch w;
        w.notes.note(lineUnder(kFormerLan));
        w.box.table[{kPort, kTcp}] = rowOf(kFormerLan);
        w.take(w.keeper.release());
        CHECK(w.box.lookups == 0 && w.box.removes == 0 && w.warnings.isEmpty());
        CHECK(w.board.cancels == 0);
    }

    SECTION("Tcp443Guard: no dial can be made");
    {
        Watch w;
        w.straight = false;
        w.board.refuses = true;
        w.look(true, true, kBlock, 0);
        CHECK(!w.keeper.running() && w.warnings.size() == 1 && w.infos.isEmpty());
        CHECK(w.warnings.first().contains(QStringLiteral("could not dial")));
        CHECK(w.box.adds == 0 && w.notes.lines.isEmpty());
        // No run is begun again by this run of the server, whatever becomes
        // of the dialler: the router is asked nothing more for it
        w.board.refuses = false;
        const int lookups = w.box.lookups;
        for (uint64_t at = 1; at < 4; ++at)
            w.look(true, true, kBlock, at * hour);
        CHECK(w.board.asked.isEmpty() && w.warnings.size() == 1 && w.box.lookups == lookups);
        CHECK(w.saysNoAddress());
    }
    {
        // Refused in the middle of a run, with the entry just written: the
        // run is not read as one whose proof said no, and the entry is taken
        // off
        Watch w;
        w.straight = false;
        w.look(true, true, kBlock, 0);
        w.answer(true, 100);
        w.board.refuses = true;
        w.answer(false, 200);
        CHECK(!w.keeper.running() && !w.box.has443() && w.notes.lines.isEmpty());
        CHECK(w.box.adds == 1 && w.box.removes == 1 && w.keeper.leadsTo() == 0);
        CHECK(w.warnings.size() == 1 &&
              w.warnings.first().contains(QStringLiteral("could not dial")));
    }
}

/// The looks that are asked for while the router is busy with what an earlier
/// run left: the latest one is the one made, once.
void waiting()
{
    SECTION("Tcp443Guard: of the looks that waited, the one asked for last is the one made");
    // As the allocator tells it: switched on, then a switch went off
    Situation on = wanted();
    on.routerAddress.clear();
    Situation off = on;
    off.wanted = false;
    {
        Waiting latest;
        Situation now;
        bool ask = true;
        CHECK(!latest.waits() && !latest.take(now, ask));
        // On, then off, while both wait: only the switch-off is made, and
        // nothing at all is asked of the router or dialled
        latest.put(on, true);
        latest.put(off, false);
        CHECK(latest.waits());
        CHECK(latest.take(now, ask) && !ask && !now.wanted);
        Watch w;
        w.lookAt(now, ask);
        CHECK(w.box.adds == 0 && w.box.lookups == 0 && w.asked == 0 && w.dials.log.isEmpty());
        CHECK(w.keeper.leadsTo() == 0 && !w.box.has443());
        // The one it replaced is never made
        CHECK(!latest.waits() && !latest.take(now, ask));
    }
    {
        // The same two with 443 held: the entry is taken off, and nothing is
        // written towards it again
        Watch w;
        w.look(true);
        CHECK(w.keeper.leadsTo() == kBlock);
        const int adds = w.box.adds;
        Waiting latest;
        Situation now;
        bool ask = true;
        latest.put(on, true);
        latest.put(off, false);
        CHECK(latest.take(now, ask));
        w.lookAt(now, ask, 1000);
        CHECK(w.box.adds == adds && w.box.removes == 1);
        CHECK(w.keeper.leadsTo() == 0 && !w.box.has443() && w.notes.lines.isEmpty());
        CHECK(!latest.take(now, ask));
    }
    {
        // Off, then on: the last word is on, and 443 is asked for, once
        Waiting latest;
        Situation now;
        bool ask = false;
        latest.put(off, false);
        latest.put(on, true);
        CHECK(latest.take(now, ask) && ask && now.wanted && now.blockPort == kBlock);
        Watch w;
        w.lookAt(now, ask);
        CHECK(w.keeper.leadsTo() == kBlock && w.box.adds == 1 && w.box.row443() == rowOf(kLan));
        CHECK(!latest.take(now, ask));
    }
}

} // namespace

void run_tcp443_guard_tests()
{
    addresses();
    notAsked();
    stepByStep();
    written();
    kept();
    released();
    formerAddress();
    inSteps();
    pacing();
    adminNotice();
    keeping();
    dialling();
    waiting();
}
