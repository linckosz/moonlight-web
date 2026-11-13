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
        const Box::Row row = box.row443();
        return !deaf && box.has443() && row.client == box.lan && row.internal == kBlock;
    }
    bool answers(const QString& address, uint16_t port) override
    {
        log << QStringLiteral("silent to %1").arg(port);
        return address == kPublic && (handMade || box.has443());
    }
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
}
