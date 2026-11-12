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
           (port >= kDevTunnelPoolBegin && port <= kDevTunnelPoolEnd) ||
           (port >= kMediaPoolBegin && port <= kMediaPoolEnd) || inMediaBlock(port);
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
    // The shared ports are read in UDP alone: no TCP hole to a port nothing
    // listens on.
    request.tcp = false;
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
    const int first = kMediaBasePort;
    const int last = kMediaBasePort + kMediaPortCount - 1;
    for (int i = 0; i < kMuxBlockSpan; ++i)
        request.preferred.append(static_cast<uint16_t>(devInstance ? last - i : first + i));
    return request;
}

bool RouterPortCore::mediaPortAllowed(uint16_t port)
{
    using namespace mw::routerports;
    return inMediaBlock(port) || (port >= kMediaPoolBegin && port <= kMediaPoolEnd);
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
    const auto free = [&](uint16_t port) {
        return mediaPortAllowed(port) && !(muxInUse && inMuxBlockStretch(devInstance, port));
    };
    if (free(remembered)) request.remembered.append(remembered);
    if (free(internalPort)) request.preferred.append(internalPort);
    request.poolBegin = kMediaPoolBegin;
    request.poolEnd = kMediaPoolEnd;
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

QString RouterPortCore::ownerOf(uint16_t external)
{
    IUpnpGateway::Entry shown;
    if (m_Gateway.entry(external, kUdp, shown) != Lookup::Found) return {};
    return shown.client;
}

bool RouterPortCore::tcpWritable(uint16_t external, IUpnpGateway::Entry* seen)
{
    IUpnpGateway::Entry shown;
    if (m_Gateway.entry(external, kTcp, shown) != Lookup::Found || shown.client.isEmpty())
        return true;
    if (seen) *seen = shown;
    return shown.client == m_Gateway.lanAddress() ||
           (!m_PreviousLan.isEmpty() && shown.client == m_PreviousLan);
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
    return ok;
}

void RouterPortCore::strike(uint16_t external, bool tcp)
{
    if (m_Record) m_Record->strike(external, tcp);
}

RouterPortCore::Result RouterPortCore::claim(const Request& request)
{
    Result result;

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
    // One exception: an entry that points at the address THIS host had before
    // a DHCP renumbering. Nobody else is behind it, and leaving it alone would
    // strand the port until the lease runs out — forever, if it was permanent.
    const QString lan = m_Gateway.lanAddress();
    const QString before = ownerOf(external);
    if (!before.isEmpty() && before != lan) {
        if (!m_PreviousLan.isEmpty() && before == m_PreviousLan) {
            result.reclaimedFrom = before;
        } else {
            result.skipped.append({external, SkipReason::Foreign, before});
            return false;
        }
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
    }

    Claim claim;
    claim.purpose = request.purpose;
    claim.slot = request.slot;
    claim.external = external;
    claim.internal = internal;
    // Best effort, and only useful where UDP is blocked outright: the
    // connections enable ICE-TCP, but a browser only ever opens those
    // outbound, so reaching this machine needs the inbound hole too. A
    // neighbour's TCP forward on the same number is left alone, like its UDP
    // one above: the router would repoint it, or remove it and add ours.
    if (request.tcp) {
        Write tcp = udp;
        tcp.tcp = true;
        IUpnpGateway::Entry there;
        if (tcpWritable(external, &there)) {
            // An entry of this host's with another text is written afresh too
            const bool tcpOldText = !there.client.isEmpty() && keptOldText(tcp, there);
            claim.tcp = write(tcp, kept || tcpOldText);
        }
    }
    m_Held.append(claim);
    result.claim = claim;
    return true;
}

bool RouterPortCore::rewrite(const Claim& c, QString& owner)
{
    const Write udp{c.external, c.internal, c.purpose, c.slot, false};
    Write tcpWrite = udp;
    tcpWrite.tcp = true;
    bool ok = write(udp);
    // A neighbour may have taken the TCP number since: its entry is not rewritten.
    const bool tcp = c.tcp && tcpWritable(c.external);
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
    bool ok = true;

    // Checked after the write, once per half hour, because a machine that comes
    // online later takes these entries silently: nothing in the renewal's own
    // answer says the table stopped pointing here.
    const QString lan = m_Gateway.lanAddress();
    const QList<Claim> held = m_Held;
    for (const Claim& c : held) {
        QString owner;
        if (!rewrite(c, owner)) ok = false;
        if (!owner.isEmpty() && owner != lan) lost.append({c, owner});
    }
    for (const Lost& l : lost) {
        m_Held.removeAll(l.claim);
        // Another machine's now: the record has nothing to say of it.
        strike(l.claim.external, false);
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
            rewrite(c, owner);
        }
    }
    return lost;
}

QList<RouterPortCore::Lost> RouterPortCore::renew(uint16_t externalPort)
{
    for (const Claim& held : std::as_const(m_Held)) {
        if (held.external != externalPort) continue;
        const Claim c = held;
        QString owner;
        rewrite(c, owner);
        if (owner.isEmpty() || owner == m_Gateway.lanAddress()) return {};
        m_Held.removeAll(c);
        strike(c.external, false);
        return {{c, owner}};
    }
    return {};
}

void RouterPortCore::release(const Claim& claim)
{
    if (!m_Held.contains(claim)) return;
    // A removal the router does not acknowledge stays in the record: the
    // next start looks at what is left of it.
    if (m_Gateway.remove(claim.external, kUdp)) strike(claim.external, false);
    if (claim.tcp && tcpWritable(claim.external) && m_Gateway.remove(claim.external, kTcp))
        strike(claim.external, true);
    m_Held.removeAll(claim);
}

void RouterPortCore::releaseAll()
{
    const QList<Claim> held = m_Held;
    for (const Claim& c : held)
        release(c);
}

QList<Hole> RouterPortCore::sweepList() const
{
    QList<Hole> out;
    if (!m_Record) return out;
    const QString lan = m_Gateway.lanAddress();
    for (const Hole& line : m_Record->holes())
        if (line.lan == lan && !holdsLine(line.external, line.tcp)) out.append(line);
    return out;
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
    if (!m_Record) return {Swept::Unknown, {}};
    // A claim served since the list was made may have written the entry anew
    // (it is held), or found it another machine's (its line is gone).
    if (holdsLine(hole.external, hole.tcp)) return {Swept::HeldAgain, {}};
    Hole line;
    if (!m_Record->find(hole.external, hole.tcp, line)) return {Swept::Gone, {}};

    const QString lan = m_Gateway.lanAddress();
    if (line.lan != lan) return {Swept::OtherAddress, {}};
    // The words alone prove nothing of a line that should carry a tag
    const QString tag = line.old ? QString() : m_Record->tag();
    if (!line.old && !mw::routerholes::validTag(tag)) return {Swept::Unknown, {}};

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

    if (!m_Gateway.remove(line.external, protocol)) return {Swept::Stayed, {}};
    IUpnpGateway::Entry after;
    if (m_Gateway.entry(line.external, protocol, after) != Lookup::None) return {Swept::Stayed, {}};
    strike(line.external, line.tcp);
    return {Swept::Removed, {}};
}
