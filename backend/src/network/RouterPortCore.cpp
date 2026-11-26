/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "RouterPortCore.h"
#include "RouterPortPools.h"

#include <QDebug>

#include <optional>

namespace {
const QString kUdp = QStringLiteral("UDP");
const QString kTcp = QStringLiteral("TCP");

using mw::routerholes::Hole;
using mw::routerholes::Kind;
using mw::sockettables::Listens;
using Lookup = IUpnpGateway::Lookup;

Kind kindOf(RouterPortCore::Purpose purpose)
{
    switch (purpose) {
    case RouterPortCore::Purpose::Tunnel: return Kind::Tunnel;
    case RouterPortCore::Purpose::Mux: return Kind::Streams;
    case RouterPortCore::Purpose::Media: break;
    }
    return Kind::Media;
}

/// Whether any MoonlightWeb, of any kind, ever asks a router for `port`.
constexpr bool inAnyList(int port)
{
    using namespace mw::routerports;
    for (const uint16_t p : kTunnelPreferred)
        if (p == port) return true;
    return (port >= kTunnelPoolBegin && port <= kTunnelPoolEnd) ||
           (port >= kDevTunnelPoolBegin && port <= kDevTunnelPoolEnd) || inMediaPool(false, port) ||
           inMediaPool(true, port) || inMediaBlock(false, port) || inMediaBlock(true, port);
}
// The question whose answer is known is only known if nobody writes there.
static_assert(!inAnyList(mw::routerholes::kKnownQuestionPort),
              "the port the sweep asks about first is one a MoonlightWeb may hold");

/// What a router may say of an entry: an address in figures, a port, and a
/// description that is text.
bool readsAsEntry(const IUpnpGateway::Entry& e)
{
    if (!mw::routerholes::dottedIpv4(e.client) || e.internalPort == 0) return false;
    if (e.description.size() > 128) return false;
    for (const QChar c : e.description)
        if (c.unicode() < 0x20 || c.unicode() == 0x7F) return false;
    return true;
}

void logKeptLease(uint16_t external, int leaseLeft)
{
    qInfo().noquote() << QStringLiteral("[UPNP] Port %1 still runs down its old lease (%2 s left): "
                                        "the router took the write without renewing it — "
                                        "removing the entry and adding it again")
                             .arg(external)
                             .arg(leaseLeft);
}

void logKeptText(uint16_t external)
{
    qInfo().noquote() << QStringLiteral("[UPNP] Port %1 still shows an earlier description: the "
                                        "router took the write without changing it — removing "
                                        "the entry and adding it again")
                             .arg(external);
}
} // namespace

RouterPortCore::RouterPortCore(IUpnpGateway& gateway, std::function<bool(uint16_t)> localBindProbe)
    : m_Gateway(gateway)
    , m_BindProbe(std::move(localBindProbe))
    , m_LeaseSec(mw::routerports::kLeaseSec)
{}

bool RouterPortCore::tunnelPortAllowed(bool devInstance, uint16_t port)
{
    using namespace mw::routerports;
    if (devInstance) return port >= kDevTunnelPoolBegin && port <= kDevTunnelPoolEnd;
    for (const uint16_t p : kTunnelPreferred)
        if (p == port) return true;
    return port >= kTunnelPoolBegin && port <= kTunnelPoolEnd;
}

RouterPortCore::Request RouterPortCore::tunnelRequest(bool devInstance,
                                                      const QList<uint16_t>& remembered)
{
    using namespace mw::routerports;
    Request request;
    request.purpose = Purpose::Tunnel;
    for (const uint16_t p : remembered)
        if (tunnelPortAllowed(devInstance, p)) request.remembered.append(p);
    if (devInstance) {
        request.poolBegin = kDevTunnelPoolBegin;
        request.poolEnd = kDevTunnelPoolEnd;
        return request;
    }
    for (const uint16_t p : kTunnelPreferred)
        request.preferred.append(p);
    request.poolBegin = kTunnelPoolBegin;
    request.poolEnd = kTunnelPoolEnd;
    return request;
}

bool RouterPortCore::muxPortAllowed(bool devInstance, int which, uint16_t port)
{
    using namespace mw::routerports;
    if (which == kMuxStandard) {
        if (devInstance) return false;
        for (const uint16_t p : kTunnelPreferred)
            if (p == port) return true;
        return false;
    }
    return which == kMuxBlock && inMuxBlockStretch(devInstance, port);
}

RouterPortCore::Request RouterPortCore::muxRequest(bool devInstance, int which,
                                                   const QList<uint16_t>& remembered)
{
    using namespace mw::routerports;
    Request request;
    request.purpose = Purpose::Mux;
    request.slot = which;
    // The server listens on a shared port in TCP too, for a stream that has
    // to connect that way (IceMuxTcp): its TCP hole comes alongside. Whoever
    // asks takes it off where nothing will listen (ice_tcp_passive off).
    request.tcp = true;
    for (const uint16_t p : remembered)
        if (muxPortAllowed(devInstance, which, p)) request.remembered.append(p);

    if (which == kMuxStandard) {
        // No pool behind the eight: a host that finds them all taken streams
        // on its block port alone, which is where every stream was before.
        if (!devInstance)
            for (const uint16_t p : kTunnelPreferred)
                request.preferred.append(p);
        return request;
    }
    if (which != kMuxBlock) return request;

    // Listed rather than given as a pool: a --dev walks its stretch downwards.
    const int first = mediaBasePort(devInstance);
    const int last = first + kMediaPortCount - 1;
    for (int i = 0; i < kMuxBlockSpan; ++i)
        request.preferred.append(static_cast<uint16_t>(devInstance ? last - i : first + i));
    return request;
}

bool RouterPortCore::mediaPortAllowed(bool devInstance, uint16_t port)
{
    using namespace mw::routerports;
    return inMediaBlock(devInstance, port) || inMediaPool(devInstance, port);
}

RouterPortCore::Request RouterPortCore::mediaRequest(bool muxInUse, bool devInstance, int slot,
                                                     uint16_t internalPort, uint16_t remembered,
                                                     bool muxHoldsPort)
{
    using namespace mw::routerports;
    Request request;
    request.purpose = Purpose::Media;
    request.slot = slot;
    request.internalPort = internalPort;
    if (muxHoldsPort) return request;
    // A number of the other identity's block or pool is never asked for: a
    // DEV that remembers what it had before it had a block of its own
    const auto free = [&](uint16_t port) {
        return mediaPortAllowed(devInstance, port) &&
               !(muxInUse && inMuxBlockStretch(devInstance, port));
    };
    if (free(remembered)) request.remembered.append(remembered);
    if (free(internalPort)) request.preferred.append(internalPort);
    request.poolBegin = devInstance ? kDevMediaPoolBegin : kMediaPoolBegin;
    request.poolEnd = devInstance ? kDevMediaPoolEnd : kMediaPoolEnd;
    return request;
}

bool RouterPortCore::holds(uint16_t externalPort) const
{
    for (const Claim& c : m_Held)
        if (c.external == externalPort) return true;
    return false;
}

bool RouterPortCore::holdsLine(uint16_t external, bool tcp) const
{
    for (const Claim& c : m_Held)
        if (c.external == external && (!tcp || c.tcp)) return true;
    return false;
}

bool RouterPortCore::moonlightText(const QString& description, QString* tag)
{
    using mw::routerholes::describe;
    // The words, then the tag when there is one
    QString words = description;
    QString itsTag;
    const qsizetype space = description.lastIndexOf(QLatin1Char(' '));
    if (space > 0 && mw::routerholes::validTag(description.mid(space + 1))) {
        words = description.left(space);
        itsTag = description.mid(space + 1);
    }
    bool known = false;
    for (const Kind kind : {Kind::Tunnel, Kind::Streams, Kind::Tcp443})
        known = known || words == describe(kind, -1, QString());
    for (int slot = 0; !known && slot < mw::routerports::kMediaPortCount; ++slot)
        known = words == describe(Kind::Media, slot, QString());
    if (known && tag) *tag = itsTag;
    return known;
}

bool RouterPortCore::taggedOurs(const QString& description) const
{
    if (!m_Record || !mw::routerholes::validTag(m_Record->tag())) return false;
    QString tag;
    return moonlightText(description, &tag) && tag == m_Record->tag();
}

Listens RouterPortCore::otherServer() const
{
    return m_OtherServer ? m_OtherServer() : Listens::Unknown;
}

bool RouterPortCore::earlierOurs(uint16_t external, bool tcp, const IUpnpGateway::Entry& shown)
{
    if (!m_Record) return false;
    Hole line;
    if (!m_Record->find(external, tcp, line) || !line.old) return false;
    if (line.lan != m_Gateway.lanAddress() || shown.client != line.lan) return false;
    if (shown.internalPort != line.internal) return false;
    if (shown.description != mw::routerholes::describe(line.kind, line.slot, QString()))
        return false;
    // Every edition up to 0.4.3 wrote these words to these ports. Only the
    // installed host binds that block now, and only while no other server
    // here may be the one that wrote them: a release of then that streams
    // over UDP alone listens on nothing in TCP, its TCP entry in use all the
    // same.
    const bool slotEntry = line.kind == Kind::Media;
    if (slotEntry && m_DevInstance) return false;
    const Listens listens = m_Listens ? m_Listens(line.internal, tcp) : Listens::Unknown;
    if (listens != Listens::No) return false;
    return !slotEntry || otherServer() == Listens::No;
}

RouterPortCore::Whose RouterPortCore::whoseIs(const IUpnpGateway::Entry& shown, uint16_t external,
                                              bool tcp, const Claim* held)
{
    const QString lan = m_Gateway.lanAddress();
    if (held) {
        if (shown.client != lan) return Whose::Foreign;
        // To another port of this machine than the one this host binds
        if (shown.internalPort != 0 && shown.internalPort != held->internal) return Whose::Other;
        const QString& text = shown.description;
        if (text == textOf(held->purpose, held->slot) ||
            (!held->text.isEmpty() && text == held->text))
            return Whose::Ours;
        // Words that are a MoonlightWeb's, and not this installation's: an
        // edition beside it wrote them. Any other words are the router's own,
        // as some routers put, or what they show after a reboot.
        return moonlightText(text, nullptr) ? Whose::Other : Whose::Ours;
    }
    // Towards another address: another machine's, unless the record AND the
    // words say this installation wrote it there, under an address this host
    // had before a DHCP renumbering. That address may be another machine's by
    // now, so such an entry is never written over: it is taken off first.
    if (shown.client != lan)
        return formerOurs(external, tcp, shown) ? Whose::Former : Whose::Foreign;
    if (taggedOurs(shown.description)) return Whose::Ours;
    if (earlierOurs(external, tcp, shown)) return Whose::Ours;
    return Whose::Other;
}

bool RouterPortCore::formerOurs(const Hole& line, const IUpnpGateway::Entry& shown) const
{
    if (!m_Record || line.old || line.kind == Kind::Tcp443) return false;
    const QString tag = m_Record->tag();
    if (!mw::routerholes::validTag(tag)) return false;
    if (!mw::routerholes::dottedIpv4(line.lan) || line.lan == m_Gateway.lanAddress()) return false;
    if (!readsAsEntry(shown)) return false;
    return shown.client == line.lan && shown.internalPort == line.internal &&
           shown.description == mw::routerholes::describe(line.kind, line.slot, tag);
}

bool RouterPortCore::formerOurs(uint16_t external, bool tcp, const IUpnpGateway::Entry& shown) const
{
    Hole line;
    return m_Record && m_Record->find(external, tcp, line) && formerOurs(line, shown);
}

bool RouterPortCore::takeOffFormer(uint16_t external, bool tcp)
{
    const QString& protocol = tcp ? kTcp : kUdp;
    // Only "no such entry" read back says it is gone: a router that says it
    // removed it, and still has it, keeps it
    IUpnpGateway::Entry after;
    if (m_Gateway.remove(external, protocol) &&
        m_Gateway.entry(external, protocol, after) == Lookup::None) {
        strike(external, tcp);
        return true;
    }
    m_FormerStays.insert(qMakePair(external, tcp));
    return false;
}

bool RouterPortCore::addressMoved() const
{
    const QString now = m_Gateway.addressNow();
    // Found again from there, the gateway saw this machine where it saw it:
    // what it goes by is the address every write names
    return mw::routerholes::dottedIpv4(now) && now != m_Gateway.lanAddress() && now != m_SameFrom;
}

QList<RouterPortCore::Lost> RouterPortCore::followAddress(bool again)
{
    QList<Lost> lost;
    if (m_Closed || !m_Rediscover || !addressMoved()) return lost;
    const QString was = m_Gateway.lanAddress();
    const QString now = m_Gateway.addressNow();
    // Looked for from there already, in vain: a claim does not wait for that
    // again, a renewal does
    if (!again && now == m_UnansweredFrom) return lost;
    if (!m_Rediscover()) {
        m_UnansweredFrom = now;
        return lost;
    }
    m_UnansweredFrom.clear();
    // The router sees this machine where it saw it: nothing moved after all,
    // and it is not looked for again from that address
    if (m_Gateway.lanAddress() == was) {
        m_SameFrom = now;
        return lost;
    }
    m_SameFrom.clear();

    // Every held hole: what it has towards the address this host had is taken
    // off, then it is written towards the one it has now. Its line was noted
    // under the former address, which is what formerOurs() goes by.
    const QList<Claim> held = m_Held;
    for (const Claim& c : held) {
        bool kept = false;
        bool tcpKept = false;
        for (const bool isTcp : {false, true}) {
            if (isTcp && !c.tcp) continue;
            IUpnpGateway::Entry shown;
            if (m_Gateway.entry(c.external, isTcp ? kTcp : kUdp, shown) == Lookup::Found &&
                formerOurs(c.external, isTcp, shown) && !takeOffFormer(c.external, isTcp))
                (isTcp ? tcpKept : kept) = true;
        }
        // Its TCP entry stays towards the former address: no longer this
        // hole's, its line left for the next start
        if (tcpKept && !kept)
            for (Claim& h : m_Held)
                if (h == c) h.tcp = false;
        // The router keeps its entry towards the former address: the number
        // is not this host's to write here. Its line stays for the next start.
        if (kept) {
            m_Held.removeAll(c);
            lost.append({c, was, false});
            continue;
        }
        QString owner;
        bool here = false;
        rewrite(c, owner, here);
        if (!here && (owner.isEmpty() || owner == m_Gateway.lanAddress())) continue;
        m_Held.removeAll(c);
        strike(c.external, false);
        dropTcpOf(c);
        lost.append({c, owner, here});
    }
    return lost;
}

RouterPortCore::SweepResult RouterPortCore::sweepFormer(const Hole& line)
{
    SweepResult out;
    out.former = true;
    // Without this record's tag the words prove nothing
    if (!mw::routerholes::validTag(m_Record->tag())) {
        out.outcome = Swept::Unknown;
        return out;
    }
    const QString& protocol = line.tcp ? kTcp : kUdp;
    IUpnpGateway::Entry shown;
    switch (m_Gateway.entry(line.external, protocol, shown)) {
    case Lookup::None:
        strike(line.external, line.tcp);
        out.outcome = Swept::Gone;
        return out;
    case Lookup::Unknown: out.outcome = Swept::Unknown; return out;
    case Lookup::Found: break;
    }
    if (!readsAsEntry(shown)) {
        out.outcome = Swept::Unknown;
        return out;
    }
    // Another machine's, another installation's, or what another program
    // made of that number since: not this host's to touch, and no longer in
    // its record
    if (!formerOurs(line, shown)) {
        strike(line.external, line.tcp);
        out.outcome = Swept::NotOurs;
        if (shown.client != line.lan)
            out.detail = QStringLiteral("address");
        else if (shown.internalPort != line.internal)
            out.detail = QStringLiteral("internal port");
        else
            out.detail = QStringLiteral("description");
        return out;
    }
    out.outcome = takeOffFormer(line.external, line.tcp) ? Swept::RemovedFormer : Swept::Stayed;
    return out;
}

RouterPortCore::Whose RouterPortCore::judge(uint16_t external, bool tcp, const Claim* held,
                                            IUpnpGateway::Entry& shown)
{
    if (m_Gateway.entry(external, tcp ? kTcp : kUdp, shown) != Lookup::Found ||
        shown.client.isEmpty())
        return Whose::Free;
    return whoseIs(shown, external, tcp, held);
}

bool RouterPortCore::keptOldLease(int leaseLeft) const
{
    // 0 is a permanent entry, -1 a router that does not say: nothing to judge.
    if (leaseLeft <= 0) return false;
    const auto left = static_cast<uint32_t>(leaseLeft);
    // Asked for a permanent entry, still counting down a lease of the size
    // this host asks for: the old entry. A longer countdown is the router's
    // own idea of permanent (IGD v2 turns 0 into a week), not ours.
    if (m_LeaseSec == 0) return left <= mw::routerports::kLeaseSec;
    // Renewed, an entry reads back at about the full lease. Left as it was,
    // it has run down since it was made, to half or less by the second
    // renewal; the quarter in between keeps a router that rounds clear.
    return left < m_LeaseSec - m_LeaseSec / 4;
}

bool RouterPortCore::keptOldText(const Write& w, const IUpnpGateway::Entry& shown) const
{
    return m_Record && shown.description != textOf(w.purpose, w.slot);
}

QString RouterPortCore::textOf(Purpose purpose, int slot) const
{
    return mw::routerholes::describe(kindOf(purpose), slot, m_Record ? m_Record->tag() : QString());
}

bool RouterPortCore::addWithLatch(uint16_t external, uint16_t internal, const QString& description,
                                  const QString& protocol)
{
    if (m_LeaseSec > 0) {
        if (m_Gateway.add(external, internal, m_LeaseSec, description, protocol)) return true;
        // Plenty of IGDs — v1 especially — reject a non-zero lease outright,
        // and would otherwise cost the user UPnP entirely. Only conclude that
        // from a permanent retry actually succeeding: a plain "port busy"
        // failure fails either way and must not drop everyone's lease.
        if (!m_Gateway.add(external, internal, 0, description, protocol)) return false;
        m_LeaseSec = 0;
        return true;
    }
    return m_Gateway.add(external, internal, 0, description, protocol);
}

bool RouterPortCore::write(const Write& w, bool afresh)
{
    // Noted first: an entry the router has and the record has not could
    // never be taken back. A line of that port there already — an earlier
    // run's, another purpose's — comes back if the router refuses this one.
    std::optional<Hole> before;
    if (m_Record) {
        Hole was;
        if (m_Record->find(w.external, w.tcp, was)) before = was;
        Hole line;
        line.external = w.external;
        line.tcp = w.tcp;
        line.internal = w.internal;
        line.kind = kindOf(w.purpose);
        line.slot = w.purpose == Purpose::Media ? w.slot : -1;
        line.lan = m_Gateway.lanAddress();
        m_Record->note(line);
    }
    const QString& protocol = w.tcp ? kTcp : kUdp;
    if (afresh) m_Gateway.remove(w.external, protocol);
    const bool ok = addWithLatch(w.external, w.internal, textOf(w.purpose, w.slot), protocol);
    if (!ok && m_Record) {
        if (before)
            m_Record->note(*before);
        else
            m_Record->strike(w.external, w.tcp);
    }
    // The line is this write's now, whatever was noted of that number before
    if (ok) m_FormerStays.remove(qMakePair(w.external, w.tcp));
    return ok;
}

void RouterPortCore::strike(uint16_t external, bool tcp)
{
    if (m_Record) m_Record->strike(external, tcp);
    m_FormerStays.remove(qMakePair(external, tcp));
}

void RouterPortCore::dropTcpOf(const Claim& lost)
{
    if (!lost.tcp) return;
    // One the router does not show, or shows as a neighbour's, is not asked
    // to go: its line stays for the next start, which goes by the record.
    IUpnpGateway::Entry there;
    if (m_Gateway.entry(lost.external, kTcp, there) != Lookup::Found) return;
    if (there.client != m_Gateway.lanAddress()) return;
    // Nor one at this host's address that is not the entry it wrote, to the
    // letter: another edition on this machine may hold that number.
    if (there.internalPort != lost.internal || there.description != textOf(lost.purpose, lost.slot))
        return;
    if (m_Gateway.remove(lost.external, kTcp)) strike(lost.external, true);
}

bool RouterPortCore::takeOff(const Claim& claim, bool tcp, const std::function<bool()>& expired,
                             bool& left)
{
    const QString& protocol = tcp ? kTcp : kUdp;
    const auto late = [&expired]() { return expired && expired(); };
    if (late()) {
        left = true;
        return false;
    }
    IUpnpGateway::Entry shown;
    const Lookup seen = m_Gateway.entry(claim.external, protocol, shown);
    // Gone already: a router that rebooted, a lease that ran out
    if (seen == Lookup::None) {
        strike(claim.external, tcp);
        return false;
    }
    // Another machine's, or another installation's on this machine, by now:
    // not this host's to take off, and no longer in its record. A router that
    // does not say is asked to remove what this host wrote, as ever.
    if (seen == Lookup::Found && !shown.client.isEmpty()) {
        const Whose whose = whoseIs(shown, claim.external, tcp, &claim);
        if (whose == Whose::Foreign || whose == Whose::Other) {
            strike(claim.external, tcp);
            qInfo().noquote() << QStringLiteral("[UPNP] %1 %2 is %3's entry on the router by now: "
                                                "left as it is")
                                     .arg(protocol)
                                     .arg(claim.external)
                                     .arg(whose == Whose::Foreign
                                              ? QStringLiteral("another machine")
                                              : QStringLiteral("another installation on this "
                                                               "machine"));
            return false;
        }
    }
    if (late()) {
        left = true;
        return false;
    }
    // A removal the router does not acknowledge stays in the record: the
    // next start looks at what is left of it.
    if (m_Gateway.remove(claim.external, protocol))
        strike(claim.external, tcp);
    else
        left = true;
    return true;
}

RouterPortCore::Result RouterPortCore::claim(const Request& request)
{
    Result result;
    // Quitting: nothing new on the router. Nor while this machine has an
    // address the router was not found again from (followAddress()): every
    // write names the address it had, which may be another machine's by now.
    if (m_Closed || addressMoved()) return result;

    // Remembered first, so a restart lands where it was; then the preferred
    // list; then the pool. Each list in its own order, nothing tried twice.
    QList<uint16_t> candidates;
    const auto consider = [&](uint16_t port) {
        if (port == 0 || holds(port) || candidates.contains(port)) return;
        candidates.append(port);
    };
    for (const uint16_t p : request.remembered)
        consider(p);
    for (const uint16_t p : request.preferred)
        consider(p);
    for (int p = request.poolBegin; p <= request.poolEnd && p <= 65535; ++p)
        consider(static_cast<uint16_t>(p));

    for (const uint16_t external : candidates) {
        if (tryOne(external, request, result)) {
            result.ok = true;
            return result;
        }
    }
    return result;
}

bool RouterPortCore::tryOne(uint16_t external, const Request& request, Result& result)
{
    const bool tunnel = request.purpose == Purpose::Tunnel;
    const bool mux = request.purpose == Purpose::Mux;
    // The tunnel and the mux listen on the very port the router forwards.
    const uint16_t internal = (tunnel || mux) ? external : request.internalPort;

    if (tunnel && m_BindProbe && !m_BindProbe(external)) {
        result.skipped.append({external, SkipReason::LocalBusy, {}});
        return false;
    }

    // Someone else's entry is left alone.
    //
    // The router would let us overwrite it — that is the whole bug this guards
    // against — and taking it would break the other machine's tunnel exactly
    // the way ours was broken, until it renews and breaks ours back. There are
    // other ports; a fight over this one has no winner.
    //
    // So is another installation's on this machine: one address to the
    // router, so its entries point here too, and only their description says
    // whose they are. An entry at this address is this installation's when it
    // carries its tag, or is one a version that put no tag wrote here and
    // nothing uses any more (earlierOurs). Anything else stays.
    //
    // One exception to the address: an entry this installation wrote towards
    // the address THIS host had before a DHCP renumbering (formerOurs()).
    // Leaving it would strand the port until the lease runs out — forever, if
    // it was permanent, or on a router that keeps no lease. Writing over it
    // is not enough either: a router that keeps one entry per number answers
    // a write towards another client with a conflict (the Freebox: 718), and
    // keeps the old one. So it is taken off first, and seen gone; one the
    // router keeps leaves the number, as another's entry does.
    const QString lan = m_Gateway.lanAddress();
    IUpnpGateway::Entry seen;
    QString reclaimed;
    switch (judge(external, false, nullptr, seen)) {
    case Whose::Free:
    case Whose::Ours: break;
    case Whose::Former:
        if (!takeOffFormer(external, false)) {
            result.skipped.append({external, SkipReason::FormerStays, seen.client});
            return false;
        }
        reclaimed = seen.client;
        break;
    case Whose::Foreign:
        result.skipped.append({external, SkipReason::Foreign, seen.client});
        return false;
    case Whose::Other:
        result.skipped.append({external, SkipReason::OtherInstallation, {}});
        return false;
    }

    // The mux binds for good, so it binds late: only a number the router has
    // nothing against is worth a socket. From here on, a port that does not
    // become a hole is handed back — unless the mux had it before this claim:
    // that socket may be serving streams, and is not this claim's to close.
    bool boundForThis = false;
    if (mux) {
        bool free = true;
        if (request.hold) {
            const Hold held = request.hold(external);
            free = held != Hold::Failed;
            boundForThis = held == Hold::NewlyBound;
        } else if (m_BindProbe) {
            free = m_BindProbe(external);
        }
        if (!free) {
            result.skipped.append({external, SkipReason::LocalBusy, {}});
            return false;
        }
    }
    const auto handBack = [&]() {
        if (boundForThis && request.drop) request.drop(external);
    };

    const Write udp{external, internal, request.purpose, request.slot, false};
    if (!write(udp)) {
        result.skipped.append({external, SkipReason::Refused, {}});
        handBack();
        return false;
    }

    // And the write is read back, because "added successfully" is the router's
    // opinion of the request, not of the table. An entry that answers with
    // another machine's address is a hole that leads somewhere else, and
    // advertising it would aim every connectivity check at that machine.
    IUpnpGateway::Entry shown;
    const bool found = m_Gateway.entry(external, kUdp, shown) == Lookup::Found;
    const QString after = found ? shown.client : QString();
    if (!after.isEmpty() && after != lan) {
        // Not this host's entry after all: the record has nothing to say of it.
        strike(external, false);
        result.skipped.append({external, SkipReason::KeptElsewhere, after});
        handBack();
        return false;
    }

    // "Added" can also mean "already there, left as it was": MiniUPnPd up to
    // 1.9 answers a write identical to an entry it holds without touching it,
    // lease included. An entry this host left behind (a crash, a kill) would
    // run out on its old lease within the hour; only a fresh one takes the
    // lease just asked for. The same goes for its description, which is what
    // a later start knows the entry by: one that still shows another text is
    // written afresh, once, here and never at a renewal.
    const bool oldLease = !after.isEmpty() && keptOldLease(shown.leaseLeft);
    const bool oldText = !after.isEmpty() && keptOldText(udp, shown);
    const bool kept = oldLease || oldText;
    QString text = after.isEmpty() ? QString() : shown.description;
    if (kept) {
        if (oldLease)
            logKeptLease(external, shown.leaseLeft);
        else
            logKeptText(external);
        if (!write(udp, true)) {
            result.skipped.append({external, SkipReason::Refused, {}});
            handBack();
            return false;
        }
        // What it shows now: a router that puts words of its own puts them
        // again, and the renewals know the entry by them
        IUpnpGateway::Entry again;
        text = m_Gateway.entry(external, kUdp, again) == Lookup::Found && again.client == lan
                   ? again.description
                   : QString();
    }

    Claim claim;
    claim.purpose = request.purpose;
    claim.slot = request.slot;
    claim.external = external;
    claim.internal = internal;
    claim.text = text;
    // Best effort, and only useful where UDP is blocked outright: the
    // connections enable ICE-TCP, but a browser only ever opens those
    // outbound, so reaching this machine needs the inbound hole too. A
    // neighbour's TCP forward on the same number is left alone, like its UDP
    // one above: the router would repoint it, or remove it and add ours. So
    // is another installation's on this machine, by the same rule.
    if (request.tcp) {
        Write tcp = udp;
        tcp.tcp = true;
        IUpnpGateway::Entry there;
        Whose whose = judge(external, true, nullptr, there);
        // Its TCP entry towards the former address goes the same way. One the
        // router keeps is left, with the TCP hole.
        if (whose == Whose::Former) {
            whose = takeOffFormer(external, true) ? Whose::Free : Whose::Foreign;
            there = {};
        }
        if (whose == Whose::Free || whose == Whose::Ours) {
            // An entry of this host's with another text is written afresh too
            const bool tcpOldText = !there.client.isEmpty() && keptOldText(tcp, there);
            claim.tcp = write(tcp, kept || tcpOldText);
        }
    }
    m_Held.append(claim);
    result.claim = claim;
    // Said only now: the entry towards the former address is gone, and the
    // number is written here
    result.reclaimedFrom = reclaimed;
    return true;
}

bool RouterPortCore::rewrite(const Claim& c, QString& owner, bool& here)
{
    here = false;
    // Read first: what the router aims at another machine, or shows as
    // another installation's on this one, is not written over. The router
    // would take the write, and the other host's next renewal would take it
    // back, each breaking the other's streams in turn.
    IUpnpGateway::Entry before;
    switch (judge(c.external, false, &c, before)) {
    case Whose::Free:
    case Whose::Ours: break;
    case Whose::Other: here = true; [[fallthrough]];
    // (Former is a candidate's: a held claim's entry elsewhere is Foreign)
    case Whose::Former:
    case Whose::Foreign: owner = before.client; return true;
    }

    const Write udp{c.external, c.internal, c.purpose, c.slot, false};
    Write tcpWrite = udp;
    tcpWrite.tcp = true;
    bool ok = write(udp);
    // A neighbour may have taken the TCP number since, or another
    // installation here: its entry is not rewritten.
    IUpnpGateway::Entry tcpShown;
    const Whose tcpWhose = c.tcp ? judge(c.external, true, &c, tcpShown) : Whose::Foreign;
    const bool tcp = tcpWhose == Whose::Free || tcpWhose == Whose::Ours;
    if (tcp) write(tcpWrite);
    IUpnpGateway::Entry shown;
    owner = m_Gateway.entry(c.external, kUdp, shown) == Lookup::Found ? shown.client : QString();

    // The renewal the router ignored (see tryOne): the entry would run out on
    // the lease it was made with, an hour after the first stream, and every
    // stream after that would fail its connectivity checks until the next
    // half hour happened to land after the expiry. Only ever on an entry the
    // table says is this host's: a neighbour's is not removed.
    if (ok && owner == m_Gateway.lanAddress() && keptOldLease(shown.leaseLeft)) {
        logKeptLease(c.external, shown.leaseLeft);
        ok = write(udp, true);
        if (tcp) write(tcpWrite, true);
    }
    return ok;
}

QList<RouterPortCore::Lost> RouterPortCore::renew()
{
    QList<Lost> lost;
    // Nothing is written towards an address this machine no longer has: the
    // renumbering is followed first (followAddress()), and until the gateway
    // answers from the new address the holes are left as they are.
    if (m_Closed || addressMoved()) return lost;
    bool ok = true;

    // Checked after the write, once per half hour, because a machine that comes
    // online later takes these entries silently: nothing in the renewal's own
    // answer says the table stopped pointing here.
    const QString lan = m_Gateway.lanAddress();
    const QList<Claim> held = m_Held;
    for (const Claim& c : held) {
        QString owner;
        bool here = false;
        if (!rewrite(c, owner, here)) ok = false;
        if (here || (!owner.isEmpty() && owner != lan)) lost.append({c, owner, here});
    }
    for (const Lost& l : lost) {
        m_Held.removeAll(l.claim);
        // Another machine's now: the record has nothing to say of it.
        strike(l.claim.external, false);
        dropTcpOf(l.claim);
    }

    if (ok) {
        m_RenewFailures = 0;
        return lost;
    }

    // A lease expiring under a live connection takes it down. Two misses is
    // enough to stop betting on the next one; releaseAll() still removes the
    // mappings when the process ends. Written like any renewal, so a router
    // that keeps the leased entry over a permanent write gets it afresh.
    if (++m_RenewFailures >= 2 && m_LeaseSec > 0) {
        m_LeaseSec = 0;
        for (const Claim& c : std::as_const(m_Held)) {
            QString owner;
            bool here = false;
            rewrite(c, owner, here);
        }
    }
    return lost;
}

QList<RouterPortCore::Lost> RouterPortCore::renew(uint16_t externalPort)
{
    if (m_Closed || addressMoved()) return {};
    for (const Claim& held : std::as_const(m_Held)) {
        if (held.external != externalPort) continue;
        const Claim c = held;
        QString owner;
        bool here = false;
        rewrite(c, owner, here);
        if (!here && (owner.isEmpty() || owner == m_Gateway.lanAddress())) return {};
        m_Held.removeAll(c);
        strike(c.external, false);
        dropTcpOf(c);
        return {{c, owner, here}};
    }
    return {};
}

void RouterPortCore::release(const Claim& claim)
{
    const qsizetype at = m_Held.indexOf(claim);
    if (at < 0) return;
    // As this core holds it: its TCP entry may have been left since, towards
    // an address this host had before (followAddress())
    const Claim held = m_Held.at(at);
    // Each entry only while it is this host's (takeOff)
    bool left = false;
    takeOff(held, false, {}, left);
    if (held.tcp) takeOff(held, true, {}, left);
    m_Held.removeAll(held);
}

void RouterPortCore::releaseAll()
{
    const QList<Claim> held = m_Held;
    for (const Claim& c : held)
        release(c);
}

RouterPortCore::Released RouterPortCore::releaseBeforeExit(const std::function<bool()>& expired)
{
    close();
    Released out;
    // The streams' shared ports, then the tunnel's, then the slots': what
    // every stream stands on first.
    QList<Claim> order;
    for (const Purpose p : {Purpose::Mux, Purpose::Tunnel, Purpose::Media})
        for (const Claim& c : std::as_const(m_Held))
            if (c.purpose == p) order.append(c);
    for (const Claim& c : std::as_const(order)) {
        // Not held any more either way: what is left of it is the record's.
        m_Held.removeAll(c);
        // As release(), with the deadline asked before each call: a removal
        // the router does not acknowledge stays in the record. An entry that
        // is not this host's any more is not ours to take off, and not
        // counted as left.
        bool left = false;
        takeOff(c, false, expired, left);
        if (c.tcp) takeOff(c, true, expired, left);
        if (left)
            out.left.append(c);
        else
            ++out.released;
    }
    return out;
}

QList<Hole> RouterPortCore::sweepList(SweepCount* count) const
{
    QList<Hole> out;
    QList<Hole> former;
    SweepCount counted;
    if (m_Record) {
        const QString lan = m_Gateway.lanAddress();
        for (const Hole& line : m_Record->holes()) {
            ++counted.lines;
            if (line.lan != lan) {
                ++counted.elsewhere;
                // Looked at by formerOurs(), after the others: an untagged
                // line proves nothing at another address, and 443 has a rule
                // of its own
                if (!line.old && line.kind != Kind::Tcp443) {
                    ++counted.former;
                    former.append(line);
                }
            } else if (holdsLine(line.external, line.tcp)) {
                ++counted.held;
            } else {
                out.append(line);
            }
        }
    }
    if (count) *count = counted;
    return out + former;
}

bool RouterPortCore::sweepBelieved(const QList<Hole>& holes)
{
    bool udp = false;
    bool tcp = false;
    for (const Hole& line : holes)
        (line.tcp ? tcp : udp) = true;
    // Nobody writes there, so "no such entry" is the one true answer. A
    // router that says anything else — an entry, an error, nothing — does
    // not answer what it is asked, and is not believed about the rest.
    IUpnpGateway::Entry unused;
    if (udp && m_Gateway.entry(mw::routerholes::kKnownQuestionPort, kUdp, unused) != Lookup::None)
        return false;
    if (tcp && m_Gateway.entry(mw::routerholes::kKnownQuestionPort, kTcp, unused) != Lookup::None)
        return false;
    return true;
}

RouterPortCore::SweepResult RouterPortCore::sweep(const Hole& hole)
{
    if (!m_Record || m_Closed) return {Swept::Unknown, {}};
    // A claim served since the list was made may have written the entry anew
    // (it is held), or found it another machine's (its line is gone).
    if (holdsLine(hole.external, hole.tcp)) return {Swept::HeldAgain, {}};
    Hole line;
    if (!m_Record->find(hole.external, hole.tcp, line)) return {Swept::Gone, {}};

    const QString lan = m_Gateway.lanAddress();
    if (line.lan != lan) {
        if (line.old || line.kind == Kind::Tcp443) return {Swept::OtherAddress, {}};
        return sweepFormer(line);
    }
    // The words alone prove nothing of a line that should carry a tag
    const QString tag = line.old ? QString() : m_Record->tag();
    if (!line.old && !mw::routerholes::validTag(tag)) return {Swept::Unknown, {}};

    // A stream slot's entry of a version that put no tag: every edition then
    // wrote those words to those ports, a DEV as well as a release, so the
    // entry may be another edition's on this machine, and in use while
    // nothing listens on its port (a release of then streaming over UDP
    // alone, for its TCP entry). Only while no other MoonlightWeb server runs
    // here is nobody behind it. Asked first, and again just before the removal.
    const bool slotOfThen = line.old && line.kind == Kind::Media;
    const auto serverHere = [this]() -> std::optional<SweepResult> {
        switch (otherServer()) {
        case Listens::No: return std::nullopt;
        case Listens::Yes: return SweepResult{Swept::InUse, QStringLiteral("server")};
        case Listens::Unknown: break;
        }
        return SweepResult{Swept::InUse, QStringLiteral("server unknown")};
    };
    if (slotOfThen)
        if (const auto busy = serverHere()) return *busy;

    const QString& protocol = line.tcp ? kTcp : kUdp;
    IUpnpGateway::Entry shown;
    switch (m_Gateway.entry(line.external, protocol, shown)) {
    case Lookup::None: strike(line.external, line.tcp); return {Swept::Gone, {}};
    case Lookup::Unknown: return {Swept::Unknown, {}};
    case Lookup::Found: break;
    }
    if (!readsAsEntry(shown)) return {Swept::Unknown, {}};

    // Whose entry it is. Another machine's, another program's, another
    // installation's: not this host's to touch, and no longer in its record.
    QString differs;
    if (shown.client != lan)
        differs = QStringLiteral("address");
    else if (shown.internalPort != line.internal)
        differs = QStringLiteral("internal port");
    else if (shown.description != mw::routerholes::describe(line.kind, line.slot, tag))
        differs = QStringLiteral("description");
    if (!differs.isEmpty()) {
        strike(line.external, line.tcp);
        return {Swept::NotOurs, differs};
    }

    // No tag says which installation wrote an entry of before the record:
    // another edition on this machine writes the same words. One whose port
    // something listens on may be that edition's, and in use.
    if (line.old) {
        const Listens listens = m_Listens ? m_Listens(line.internal, line.tcp) : Listens::Unknown;
        if (listens != Listens::No)
            return {Swept::InUse, listens == Listens::Yes ? QStringLiteral("listened")
                                                          : QStringLiteral("unknown")};
    }
    if (slotOfThen)
        if (const auto busy = serverHere()) return *busy;

    if (!m_Gateway.remove(line.external, protocol)) return {Swept::Stayed, {}};
    IUpnpGateway::Entry after;
    if (m_Gateway.entry(line.external, protocol, after) != Lookup::None) return {Swept::Stayed, {}};
    strike(line.external, line.tcp);
    return {Swept::Removed, {}};
}
