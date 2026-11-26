/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The owner's list of the ports the router may forward (settings.json
 * router_ports_allowed, network/AllowedPorts, issue #82): read strictly and
 * refused whole on any doubt, with the rule it breaks; with it, every hole is
 * asked for from the list alone and TCP 443 never; refused, nothing is asked
 * and what was written before is still taken back; without it, every request
 * is what it always was.
 */
#include "test_framework.h"
#include "network/AllowedPorts.h"
#include "network/RouterHoles.h"
#include "network/RouterPortCore.h"
#include "network/RouterPortPools.h"
#include "network/Tcp443Demand.h"
#include "common/Edition.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

using namespace mw::routerports;
using mw::routerholes::Hole;
using mw::routerholes::Kind;

namespace {

/// This server's own ports, as main() passes them: a host started with
/// --http-port 8080 --https-port 8443 and the stock signaling ports.
const QList<Forbidden> kLocal = {
    {8080, 8080, QStringLiteral("this server's HTTP port")},
    {8443, 8443, QStringLiteral("this server's HTTPS port")},
    {50001, 50003, QStringLiteral("this server's signaling ports")},
    {48011, 48058, QStringLiteral("this server's signaling ports")},
};

AllowedPorts parse(const QJsonArray& list)
{
    return parseAllowedPorts(QJsonValue(list), kLocal);
}

/// Refused, and the warning names what breaks the rule.
bool refusedFor(const QJsonValue& value, const QString& said)
{
    const AllowedPorts out = parseAllowedPorts(value, kLocal);
    if (out.state != AllowedPorts::State::Invalid) return false;
    if (!out.ranges.isEmpty() || out.contains(11756)) return false;
    return out.why.contains(QLatin1String(kAllowedKey)) && out.why.contains(said);
}

/// The process's state as a test leaves it: no list.
struct Reset
{
    ~Reset()
    {
        setAllowedPorts({});
        setRememberedForRemoval({});
    }
};

void reads()
{
    SECTION("AllowedPorts: absent is no list; a list reads as written, in its order");
    {
        const AllowedPorts none = parseAllowedPorts(QJsonValue(QJsonValue::Undefined), kLocal);
        CHECK(none.state == AllowedPorts::State::Absent);
        CHECK(!none.defined() && !none.valid() && none.ports().isEmpty());

        // The issue's own case
        const AllowedPorts issue = parse({"11756-11775"});
        CHECK(issue.valid());
        CHECK_EQ(issue.ports().size(), 20);
        CHECK(issue.ports().first() == 11756 && issue.ports().last() == 11775);
        CHECK(issue.contains(11756) && issue.contains(11775));
        CHECK(!issue.contains(11755) && !issue.contains(11776));

        const AllowedPorts mixed = parse({11780, "11756-11758", 20000, "30000-30000"});
        CHECK(mixed.valid());
        const QList<uint16_t> want = {11780, 11756, 11757, 11758, 20000, 30000};
        CHECK(mixed.ports() == want);
        // The top of the port space is a port like any other
        CHECK(parse({65535, "65530-65534"}).valid());
        CHECK(parse({1024}).valid());
    }
}

void refusesShapes()
{
    SECTION(
        "AllowedPorts: P6, only whole numbers and \"a-b\", 64 items, 4096 ports, no port twice");
    {
        CHECK(refusedFor(QJsonValue(11756), "must be a list"));
        CHECK(refusedFor(QJsonValue("11756-11775"), "must be a list"));
        CHECK(refusedFor(QJsonValue(QJsonValue::Null), "must be a list"));
        CHECK(refusedFor(QJsonValue(true), "must be a list"));
        CHECK(refusedFor(QJsonValue(QJsonObject{{"from", 11756}}), "must be a list"));
        CHECK(refusedFor(QJsonValue(QJsonArray{}), "empty"));

        // Items that are neither
        for (const QJsonValue& bad :
             {QJsonValue(11756.5), QJsonValue(0), QJsonValue(-1), QJsonValue(65536),
              QJsonValue("11756"), QJsonValue("11756-"), QJsonValue("-11756"),
              QJsonValue("11756 - 11760"), QJsonValue("11760-11756"), QJsonValue("0-10"),
              QJsonValue("65535-65536"), QJsonValue("1e4-2e4"), QJsonValue("0x2DEC-0x2DF0"),
              QJsonValue("+11756-11760"), QJsonValue("11756-11760-11770"), QJsonValue(true),
              QJsonValue(QJsonValue::Null), QJsonValue(QJsonArray{11756}),
              QJsonValue(QJsonObject{})})
            CHECK(refusedFor(QJsonValue(QJsonArray{20000, bad}), "item 2"));

        // 64 items at most
        QJsonArray sixtyFour, sixtyFive;
        for (int i = 0; i < kAllowedItemsMax; ++i)
            sixtyFour.append(20000 + i);
        sixtyFive = sixtyFour;
        sixtyFive.append(30000);
        CHECK(parse(sixtyFour).valid());
        CHECK(refusedFor(QJsonValue(sixtyFive), "more than 64"));

        // 4096 ports at most
        CHECK(parse({"20000-24095"}).valid());
        CHECK(refusedFor(QJsonValue(QJsonArray{"20000-24096"}), "more than 4096"));
        CHECK(refusedFor(QJsonValue(QJsonArray{"20000-22047", "30000-32048"}), "more than 4096"));

        // A port twice, or ranges that meet
        CHECK(refusedFor(QJsonValue(QJsonArray{20000, 20000}), "overlaps"));
        CHECK(refusedFor(QJsonValue(QJsonArray{"20000-20010", 20005}), "overlaps"));
        CHECK(refusedFor(QJsonValue(QJsonArray{20010, "20000-20010"}), "overlaps"));
        CHECK(refusedFor(QJsonValue(QJsonArray{"20000-20010", "20010-20020"}), "overlaps"));
        CHECK(parse({"20000-20010", "20011-20020"}).valid());
    }
}

void refusesPorts()
{
    SECTION("AllowedPorts: P7, never a port it may not touch, each refusal with its rule");
    {
        // Below 1024: 443 included (PORTLOW)
        for (const QJsonValue v : {QJsonValue(443), QJsonValue(1), QJsonValue(1023),
                                   QJsonValue("1000-1100"), QJsonValue("80-80")})
            CHECK(refusedFor(QJsonValue(QJsonArray{20000, v}), "below 1024"));
        // The tunnel's STUN ports
        for (const QJsonValue v : {QJsonValue(3478), QJsonValue(3481), QJsonValue("3470-3478"),
                                   QJsonValue("3481-3500"), QJsonValue("3000-4000")})
            CHECK(refusedFor(QJsonValue(QJsonArray{v}), "3478-3481"));
        CHECK(parse({3477, 3482}).valid());
        // The streaming servers'
        for (const QJsonValue v : {QJsonValue(47984), QJsonValue(48010), QJsonValue("47900-47984")})
            CHECK(refusedFor(QJsonValue(QJsonArray{v}), "47984-48010"));
        // A DEV identity's, every one of them
        using namespace mw::edition;
        for (const int p :
             {int(kDevMediaBasePort), kDevMediaBasePort + kMediaPortCount - 1,
              int(kDevTunnelPoolBegin), int(kDevTunnelPoolEnd), int(kDevMediaPoolBegin),
              int(kDevMediaPoolEnd), int(kDevHttpPort), int(kDevHttpsPort), int(kDevSignalingPort),
              kDevSignalingPort + 2, 48598, 48645})
            CHECK(refusedFor(QJsonValue(QJsonArray{p}), "DEV identity"));
        // The port the sweep asks about first, which nothing may write
        CHECK(refusedFor(QJsonValue(QJsonArray{int(mw::routerholes::kKnownQuestionPort)}),
                         "asked about first"));
        // This server's own, as main() passes them
        CHECK(refusedFor(QJsonValue(QJsonArray{8080}), "HTTP port"));
        CHECK(refusedFor(QJsonValue(QJsonArray{"8400-8500"}), "HTTPS port"));
        CHECK(refusedFor(QJsonValue(QJsonArray{48011}), "signaling"));
        CHECK(refusedFor(QJsonValue(QJsonArray{"48050-48060"}), "signaling"));
        // One bad item refuses the whole list: nothing of it is kept
        const AllowedPorts whole = parse({"11756-11775", 3478});
        CHECK(whole.state == AllowedPorts::State::Invalid);
        CHECK(whole.ports().isEmpty() && !whole.contains(11756));
        CHECK(whole.why.contains("3478"));
        // The installed host's own pools may be listed: they are its own
        CHECK(parse({"46000-46031", "48550-48573", "46100-46199"}).valid());
    }
}

/// Every candidate a request names, in the order the claim walks them.
QList<uint16_t> candidates(const RouterPortCore::Request& r)
{
    QList<uint16_t> out = r.remembered + r.preferred;
    for (int p = r.poolBegin; r.poolEnd != 0 && p <= r.poolEnd; ++p)
        out.append(static_cast<uint16_t>(p));
    return out;
}

void requestsFromTheList()
{
    SECTION("AllowedPorts: P9, with a list nothing is asked for outside it, remembered or not");
    {
        Reset reset;
        setAllowedPorts(parse({"11756-11775"}));
        const AllowedPorts list = allowedPorts();
        // Numbers the settings remember from before the list, and the
        // record's: none of them is asked for
        const QList<uint16_t> before = {3478, 46000, 48550, 46100, 48562};
        setRememberedForRemoval({3478, 46000, 48550, 46100});
        for (const bool dev : {false, true}) {
            const auto tunnel = RouterPortCore::tunnelRequest(dev, before + QList<uint16_t>{11760});
            const auto standard =
                RouterPortCore::muxRequest(dev, RouterPortCore::kMuxStandard, before);
            const auto block = RouterPortCore::muxRequest(dev, RouterPortCore::kMuxBlock, before);
            for (const auto* r : {&tunnel, &standard, &block}) {
                CHECK(r->hasCandidate());
                for (const uint16_t p : candidates(*r))
                    CHECK(list.contains(p));
            }
            // What is remembered of the list comes first, as ever
            CHECK(tunnel.remembered == QList<uint16_t>{11760});
            CHECK(tunnel.preferred == list.ports());
            for (const uint16_t remembered : {uint16_t(46100), uint16_t(11770), uint16_t(0)}) {
                const auto media =
                    RouterPortCore::mediaRequest(true, dev, 0, 48562, remembered, false);
                for (const uint16_t p : candidates(media))
                    CHECK(list.contains(p));
                CHECK_EQ(media.remembered.contains(11770), remembered == 11770);
                CHECK(media.internalPort == 48562); // the slot binds its own port, as ever
            }
            // A port the mux holds still gets no hole
            CHECK(!RouterPortCore::mediaRequest(true, dev, 0, 48562, 0, true).hasCandidate());
            for (const uint16_t p : before) {
                CHECK(!RouterPortCore::tunnelPortAsked(dev, p));
                CHECK(!RouterPortCore::muxPortAsked(dev, RouterPortCore::kMuxBlock, p));
                CHECK(!RouterPortCore::mediaPortAsked(dev, p));
            }
            CHECK(RouterPortCore::tunnelPortAsked(dev, 11756));
            CHECK(RouterPortCore::muxPortAsked(dev, RouterPortCore::kMuxStandard, 11775));
            CHECK(!RouterPortCore::muxPortAsked(dev, 7, 11775));
        }
    }

    SECTION("AllowedPorts: P9, what may be taken back is wider than what may be asked for");
    {
        Reset reset;
        // An earlier list gave 20001, which the settings still remember; the
        // list in force is another
        setRememberedForRemoval({20001});
        setAllowedPorts(parse({"11756-11775"}));
        Hole t;
        t.kind = Kind::Tunnel;
        t.lan = QStringLiteral("192.168.1.20");
        for (const uint16_t p :
             {uint16_t(11756), uint16_t(20001), uint16_t(3478), uint16_t(46000)}) {
            t.external = t.internal = p;
            CHECK(mw::routerholes::allowed(false, t));
        }
        // Never one nobody gave, never below 1024, never one that does not fit
        // its kind, never an entry of a version that kept no record
        for (const uint16_t p : {uint16_t(20002), uint16_t(443), uint16_t(22)}) {
            t.external = t.internal = p;
            CHECK(!mw::routerholes::allowed(false, t));
        }
        t.external = 11756;
        t.internal = 11757;
        CHECK(!mw::routerholes::allowed(false, t));
        t.internal = 11756;
        t.old = true;
        CHECK(!mw::routerholes::allowed(false, t));
        t.old = false;
        Hole s = t;
        s.kind = Kind::Streams;
        CHECK(mw::routerholes::allowed(false, s));
        Hole m;
        m.kind = Kind::Media;
        m.slot = 0;
        m.external = 11760;
        m.internal = static_cast<uint16_t>(kMediaBasePort + kMuxBlockSpan);
        m.lan = t.lan;
        CHECK(mw::routerholes::allowed(false, m));
        m.internal = 11760; // not a slot's own port
        CHECK(!mw::routerholes::allowed(false, m));
        // 443 is never a list's
        Hole by443;
        by443.kind = Kind::Tcp443;
        by443.tcp = true;
        by443.external = 443;
        by443.internal = 11756;
        by443.lan = t.lan;
        CHECK(!mw::routerholes::allowed(false, by443));
        // Asked for: still the list alone
        CHECK(!RouterPortCore::tunnelPortAsked(false, 20001));
    }
}

void refusedList()
{
    SECTION("AllowedPorts: P8, refused, binding stays as ever and what was given stays removable");
    {
        Reset reset;
        // In force, then refused (a port of it was taken by the server itself)
        setAllowedPorts(parse({"11756-11775"}));
        AllowedPorts refused;
        refused.state = AllowedPorts::State::Invalid;
        refused.why = QStringLiteral("refused");
        setAllowedPorts(refused);
        CHECK(allowedPorts().defined() && !allowedPorts().valid());
        // What it gave is this host's to take back still
        CHECK(removableByList(11756) && removableByList(11775));
        // The requests read as without a list: the shared ports bind where
        // they always did (the router is asked nothing: the allocator refuses
        // every claim, RouterPortAllocator::refusal())
        const auto block = RouterPortCore::muxRequest(false, RouterPortCore::kMuxBlock, {});
        CHECK(block.preferred.first() == kMediaBasePort);
        CHECK(!candidates(block).contains(11756));
        // The admin is told, and nobody else
        QJsonObject status{{QLatin1String(kAllowedRefusedKey), true}};
        tellAdmin(status, false);
        CHECK(!status.contains(QLatin1String(kAllowedRefusedKey)));
        tellAdmin(status, true);
        CHECK(status.value(QLatin1String(kAllowedRefusedKey)) == QJsonValue(true));
        setAllowedPorts(parse({20000}));
        tellAdmin(status, true);
        CHECK(status.value(QLatin1String(kAllowedRefusedKey)) == QJsonValue(false));
    }
}

void noListAsEver()
{
    SECTION("AllowedPorts: P10, without the key every request is what it always was");
    {
        Reset reset;
        CHECK(!allowedPorts().defined());
        // The tunnel: the eight conferencing ports, then its pool
        const auto tunnel = RouterPortCore::tunnelRequest(false, {3479});
        CHECK(tunnel.remembered == QList<uint16_t>{3479});
        QList<uint16_t> eight;
        for (const uint16_t p : kTunnelPreferred)
            eight.append(p);
        CHECK(tunnel.preferred == eight);
        CHECK(tunnel.poolBegin == kTunnelPoolBegin && tunnel.poolEnd == kTunnelPoolEnd);
        const auto devTunnel = RouterPortCore::tunnelRequest(true, {});
        CHECK(devTunnel.preferred.isEmpty());
        CHECK(devTunnel.poolBegin == kDevTunnelPoolBegin && devTunnel.poolEnd == kDevTunnelPoolEnd);
        // The shared ports: the conferencing list, then the foot of the block
        const auto standard = RouterPortCore::muxRequest(false, RouterPortCore::kMuxStandard, {});
        CHECK(standard.preferred == eight);
        const auto block = RouterPortCore::muxRequest(false, RouterPortCore::kMuxBlock, {});
        CHECK_EQ(block.preferred.size(), kMuxBlockSpan);
        CHECK(block.preferred.first() == kMediaBasePort);
        CHECK(block.preferred.last() == kMediaBasePort + kMuxBlockSpan - 1);
        CHECK(!RouterPortCore::muxRequest(true, RouterPortCore::kMuxStandard, {}).hasCandidate());
        // A slot: its own number, then the media pool
        const auto media = RouterPortCore::mediaRequest(true, false, 0, 48562, 46101, false);
        CHECK(media.remembered == QList<uint16_t>{46101});
        CHECK(media.preferred == QList<uint16_t>{48562});
        CHECK(media.poolBegin == kMediaPoolBegin && media.poolEnd == kMediaPoolEnd);
        // What may be asked for is what may be held
        for (const uint16_t p : {uint16_t(3478), uint16_t(46000), uint16_t(11756)}) {
            CHECK_EQ(RouterPortCore::tunnelPortAsked(false, p),
                     RouterPortCore::tunnelPortAllowed(false, p));
            CHECK_EQ(RouterPortCore::mediaPortAsked(false, p),
                     RouterPortCore::mediaPortAllowed(false, p));
        }
        // Nothing outside the lists is taken back on the list's account
        CHECK(!removableByList(11756));
    }
}

void no443WithAList()
{
    SECTION("AllowedPorts: with a list, valid or not, TCP 443 is never allowed (443PORT A)");
    {
        mw::tcp443::Switches s;
        s.guard = s.router = s.sharedPorts = s.tcpPassive = true;
        // ice_tcp_443 absent from the file: the system's default (on where the
        // shared ports are, AppSettings::iceTcp443())
        s.tcp443 = true;
        CHECK(mw::tcp443::allowedBy(s));
        s.portList = true;
        CHECK(!mw::tcp443::allowedBy(s));
        s.portList = false;
        s.tcp443 = false; // ice_tcp_443 false: never
        CHECK(!mw::tcp443::allowedBy(s));
        s.tcp443 = true;
        for (bool* off : {&s.guard, &s.router, &s.sharedPorts, &s.tcpPassive}) {
            *off = false;
            CHECK(!mw::tcp443::allowedBy(s));
            *off = true;
        }
        s.devPorts = true;
        CHECK(!mw::tcp443::allowedBy(s));
    }
}

} // namespace

void run_allowed_ports_tests()
{
    reads();
    refusesShapes();
    refusesPorts();
    requestsFromTheList();
    refusedList();
    noListAsEver();
    no443WithAList();
}
