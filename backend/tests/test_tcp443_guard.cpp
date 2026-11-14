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

    bool arrivesHere(const QString& address, uint16_t port) override
    {
        log << QStringLiteral("frame to %1").arg(port);
        if (address != kPublic || !loopsBack) return false;
        if (port != kPort) return true;
        // Where this host listens: the block port, or the one it was told
        const Box::Row row = box.row443();
        return !deaf && box.has443() && row.client == box.lan &&
               row.internal == (toldPort != 0 ? toldPort : kBlock);
    }
    bool answers(const QString& address, uint16_t port) override
    {
        log << QStringLiteral("silent to %1").arg(port);
        return address == kPublic && (handMade || box.has443());
    }
    /// The shared port it was last told a connection comes in on
    uint16_t toldPort = 0;
    void leadsHereOn(uint16_t port) override { toldPort = port; }
};

struct Rig
{
    Box box;
    Notes notes;
    Dials dials{box};
    Guard guard{box, &notes, dials};
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
        const Verdict v = r.guard.claim(s);
        CHECK(v.outcome == Outcome::Left && v.step == 0 && namesNoAddress(v));
        CHECK(r.box.lookups == 0 && r.box.adds == 0 && r.dials.log.isEmpty());
    }
    {
        // No shared TCP port for it to lead to
        Rig r;
        Situation s = wanted();
        s.blockPort = 0;
        CHECK(r.guard.claim(s).step == 0);
        CHECK(r.box.lookups == 0 && r.dials.log.isEmpty());
    }
    {
        // A run that keeps no record never writes what it could not take back
        Box box;
        Dials dials(box);
        Guard guard(box, nullptr, dials);
        const Verdict v = guard.claim(wanted());
        CHECK(v.outcome == Outcome::Left && v.step == 0);
        CHECK(box.lookups == 0 && box.adds == 0 && dials.log.isEmpty());
        CHECK(guard.release() && guard.formerAddress() == Guard::Former::None);
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
        const Verdict v = r.guard.claim(s);
        CHECK(v.outcome == Outcome::Left && v.step == 1 && namesNoAddress(v));
        CHECK(r.box.lookups == 0 && r.box.adds == 0 && r.dials.log.isEmpty());
    }
    {
        // A second router in front: the one asked is not the one browsers reach
        Rig r;
        Situation s = wanted();
        s.routerAddress = QStringLiteral("198.51.100.9");
        CHECK(r.guard.claim(s).step == 1);
        s.routerAddress = QStringLiteral("10.0.0.2");
        CHECK(r.guard.claim(s).step == 1);
        CHECK(r.box.lookups == 0);
    }

    // 2: an entry on 443, whoever it leads to; a router that does not say
    const auto taken = [](const Box::Row& there) {
        Rig r;
        r.box.table[{kPort, kTcp}] = there;
        const Verdict v = r.guard.claim(wanted());
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
        const Verdict v = r.guard.claim(wanted());
        CHECK(v.outcome == Outcome::Left && v.step == 2);
        CHECK(r.box.adds == 0 && r.dials.log.isEmpty());
    }
    {
        // Noted, and written with another installation's tag: not ours
        Rig r;
        r.notes.note(lineUnder(kLan));
        r.box.table[{kPort, kTcp}] =
            rowOf(kLan, kBlock, QStringLiteral("MoonlightWeb TCP 443 ffffff"));
        CHECK(r.guard.claim(wanted()).step == 2);
        CHECK(r.box.removes == 0 && r.box.has443());
    }

    // 3: a router that does not lead back here cannot be checked from here
    {
        Rig r;
        r.dials.loopsBack = false;
        const Verdict v = r.guard.claim(wanted());
        CHECK(v.outcome == Outcome::Left && v.step == 3 && namesNoAddress(v));
        CHECK(r.dials.log == QStringList{QStringLiteral("frame to 48550")});
        CHECK(r.box.adds == 0 && r.notes.lines.isEmpty());
    }
    // 4: something answers on 443 that the router's table does not show
    {
        Rig r;
        r.dials.handMade = true;
        const Verdict v = r.guard.claim(wanted());
        CHECK(v.outcome == Outcome::Left && v.step == 4 && namesNoAddress(v));
        CHECK(r.box.adds == 0 && r.notes.lines.isEmpty() && !r.box.has443());
    }
}

void written()
{
    SECTION("Tcp443Guard: written, read back, and proved by a connection");
    {
        Rig r;
        const Verdict v = r.guard.claim(wanted());
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
        const Verdict v = r.guard.claim(wanted());
        CHECK(v.outcome == Outcome::Left && v.step == 5 && namesNoAddress(v));
        CHECK(!r.box.has443() && r.notes.lines.isEmpty() && !r.guard.held());
    }
    {
        // Said yes and wrote nothing
        Rig r;
        r.box.forgetsAdd = true;
        CHECK(r.guard.claim(wanted()).step == 5);
        CHECK(!r.box.has443() && r.notes.lines.isEmpty());
        // No connection was sent to prove what does not read back
        CHECK(r.dials.log.size() == 2);
    }
    {
        // Wrote it towards another machine: not this installation's entry,
        // and so not removed by it; nothing is noted as ours
        Rig r;
        r.box.writesTowards = kNeighbour;
        CHECK(r.guard.claim(wanted()).step == 5);
        CHECK(r.box.removes == 0 && r.box.row443().client == kNeighbour);
        CHECK(r.notes.lines.isEmpty() && r.dials.log.size() == 2);
    }
    {
        // Reads back, and no connection arrives: removed, and the removal read back
        Rig r;
        r.dials.deaf = true;
        const Verdict v = r.guard.claim(wanted());
        CHECK(v.outcome == Outcome::Left && v.step == 5);
        CHECK(r.box.removes == 1 && !r.box.has443() && r.notes.lines.isEmpty());
    }
    {
        // The same on a router that will not remove it: the line stays, for
        // the next start to take the entry back
        Rig r;
        r.dials.deaf = true;
        r.box.refusesRemove = true;
        CHECK(r.guard.claim(wanted()).step == 5);
        CHECK(r.box.has443() && r.notes.lines == QList<Hole>{lineUnder(kLan)});
        CHECK(!r.guard.held());
    }
}

void kept()
{
    SECTION("Tcp443Guard: renewed while it is still this installation's");
    {
        Rig r;
        CHECK(r.guard.claim(wanted()).outcome == Outcome::Held);
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
        CHECK(r.guard.claim(wanted()).outcome == Outcome::Held);
        r.box.table[{kPort, kTcp}] = rowOf(kNeighbour, 443, QStringLiteral("https"));
        const Verdict v = r.guard.renew(wanted());
        CHECK(v.outcome == Outcome::Lost && v.step == 6 && namesNoAddress(v));
        CHECK(r.box.adds == 1 && r.box.removes == 0 && r.box.row443().client == kNeighbour);
        CHECK(r.notes.lines.isEmpty() && !r.guard.held() && r.guard.leadsTo() == 0);
        // The next attempt is a whole run, which finds 443 taken
        CHECK(r.guard.claim(wanted()).step == 2);
    }
    {
        // Gone from the router (it restarted): lost, and struck
        Rig r;
        CHECK(r.guard.claim(wanted()).outcome == Outcome::Held);
        r.box.table.clear();
        CHECK(r.guard.renew(wanted()).outcome == Outcome::Lost);
        CHECK(r.box.adds == 1 && r.notes.lines.isEmpty());
        // A whole run then holds it again
        CHECK(r.guard.claim(wanted()).outcome == Outcome::Held);
    }
    {
        // A router that says nothing: lost for now, and the line stays
        Rig r;
        CHECK(r.guard.claim(wanted()).outcome == Outcome::Held);
        r.box.silent = true;
        CHECK(r.guard.renew(wanted()).outcome == Outcome::Lost);
        CHECK(r.box.adds == 1 && r.notes.lines.size() == 1 && !r.guard.held());
        // It speaks again: the entry is this installation's, and is kept
        // without a new run of dials
        r.box.silent = false;
        const int dialled = static_cast<int>(r.dials.log.size());
        const Verdict again = r.guard.claim(wanted());
        CHECK(again.outcome == Outcome::Held && again.step == 6);
        CHECK(r.dials.log.size() == dialled && r.box.adds == 2);
    }
    {
        // A router that refuses the renewal
        Rig r;
        CHECK(r.guard.claim(wanted()).outcome == Outcome::Held);
        r.box.refusesAdd = true;
        CHECK(r.guard.renew(wanted()).outcome == Outcome::Lost);
        CHECK(!r.guard.held() && r.notes.lines.size() == 1);
    }

    SECTION("Tcp443Guard: what it stood on changed, and the entry is taken off");
    const auto changed = [](const std::function<void(Situation&)>& how) {
        Rig r;
        CHECK(r.guard.claim(wanted()).outcome == Outcome::Held);
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
        const Verdict v = r.guard.claim(wanted());
        CHECK(v.outcome == Outcome::Held && v.step == 6);
        CHECK(r.dials.log.isEmpty() && r.box.adds == 1 && r.box.removes == 0);
    }
    {
        // Left towards a port the streams no longer share: taken off, and
        // asked for anew by a whole run
        Rig r;
        r.notes.note(lineUnder(kLan, uint16_t(kBlock + 3)));
        r.box.table[{kPort, kTcp}] = rowOf(kLan, uint16_t(kBlock + 3));
        const Verdict v = r.guard.claim(wanted());
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
        CHECK(r.guard.claim(wanted()).step == 2);
        CHECK(r.box.adds == 0 && r.notes.lines.size() == 1);
    }
    {
        // Noted, and the router no longer has it: the line goes, the run goes on
        Rig r;
        r.notes.note(lineUnder(kLan));
        CHECK(r.guard.claim(wanted()).outcome == Outcome::Held);
        CHECK(r.notes.lines == QList<Hole>{lineUnder(kLan)});
    }
}

void released()
{
    SECTION("Tcp443Guard: taken off when the host stops, and the removal read back");
    {
        Rig r;
        CHECK(r.guard.claim(wanted()).outcome == Outcome::Held);
        CHECK(r.guard.release());
        CHECK(!r.box.has443() && r.notes.lines.isEmpty() && !r.guard.held());
        CHECK(r.box.removes == 1);
        // Twice is nothing
        CHECK(r.guard.release() && r.box.removes == 1);
    }
    {
        // A router that will not remove it: said, and the line stays
        Rig r;
        CHECK(r.guard.claim(wanted()).outcome == Outcome::Held);
        r.box.refusesRemove = true;
        CHECK(!r.guard.release());
        CHECK(r.box.has443() && r.notes.lines.size() == 1 && !r.guard.held());
    }
    {
        // A router that says nothing: the same
        Rig r;
        CHECK(r.guard.claim(wanted()).outcome == Outcome::Held);
        r.box.silent = true;
        CHECK(!r.guard.release());
        CHECK(r.box.removes == 0 && r.notes.lines.size() == 1);
    }
    {
        // Another's by now: not removed, and no longer in the record
        Rig r;
        CHECK(r.guard.claim(wanted()).outcome == Outcome::Held);
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
        const Verdict v = whole.guard.claim(wanted());
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
        const Verdict v = r.guard.claim(wanted());
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
        CHECK(r.guard.claim(wanted()).step == 2 && r.guard.formerRefused());
        CHECK(r.notes.lines == QList<Hole>{lineUnder(kFormerLan)});
        r.box.silent = false;
        CHECK(r.guard.formerAddress() == Former::Refused && r.guard.formerRefused());
        // Somebody removed it on the router: no longer said, and the run goes on
        r.box.table.clear();
        CHECK(r.guard.formerAddress() == Former::Gone);
        CHECK(!r.guard.formerRefused() && r.notes.lines.isEmpty());
        CHECK(r.guard.claim(wanted()).outcome == Outcome::Held);
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
            CHECK(r.guard.claim(wanted()).step == 2);
        }
    }
    {
        // A router that says nothing: nothing is done, the line stays
        Rig r;
        r.notes.note(lineUnder(kFormerLan));
        r.box.table[{kPort, kTcp}] = rowOf(kFormerLan);
        r.box.silent = true;
        CHECK(r.guard.formerAddress() == Former::Unknown);
        CHECK(r.guard.claim(wanted()).step == 2);
        CHECK(r.box.removes == 0 && r.notes.lines.size() == 1 && !r.guard.formerRefused());
    }
    {
        // By the record AND the text, never by the address alone: this
        // installation's words at the former address with no line are not ours
        Rig r;
        r.box.table[{kPort, kTcp}] = rowOf(kFormerLan);
        CHECK(r.guard.formerAddress() == Former::None);
        CHECK(r.guard.claim(wanted()).step == 2);
        CHECK(r.box.removes == 0 && r.box.has443());
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
        // With the guard, looked at every half hour as the holes are: a
        // router that does not lead back is dialled once an hour, once each
        Rig r;
        r.dials.loopsBack = false;
        Pace p;
        int runs = 0;
        for (uint64_t tick = 0; tick < 5; ++tick) {
            const uint64_t now = tick * (hour / 2);
            if (!p.due(now)) continue;
            p.said(r.guard.claim(wanted()), now);
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
            const Verdict v = r.guard.claim(wanted());
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

/// A router, a record, what dialling finds, and the keeper over them, with
/// what it asked the router of its own address and what it said.
struct Watch
{
    Box box;
    Notes notes;
    Dials dials{box};
    int asked = 0;
    Keeper keeper{box, &notes, dials, [this]() {
                      ++asked;
                      return kPublic;
                  }};
    QStringList warnings, infos;

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
        for (const Keeper::Line& line : keeper.look(now, ask, nowMs))
            (line.warning ? warnings : infos) << line.text;
    }
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
        // The server stops: taken off, and read back
        Watch w;
        w.look(true);
        CHECK(w.keeper.release());
        CHECK(!w.box.has443() && w.notes.lines.isEmpty() && w.keeper.leadsTo() == 0);
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
    pacing();
    adminNotice();
    keeping();
    waiting();
}
