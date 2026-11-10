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

namespace {
const QString kUdp = QStringLiteral("UDP");
const QString kTcp = QStringLiteral("TCP");

QString descriptionOf(const RouterPortCore::Claim& c)
{
    return c.purpose == RouterPortCore::Purpose::Tunnel
               ? QStringLiteral("MoonlightWeb tunnel")
               : QStringLiteral("MoonlightWeb media slot %1").arg(c.slot);
}

void logKeptLease(uint16_t external, int leaseLeft)
{
    qInfo().noquote() << QStringLiteral("[UPNP] Port %1 still runs down its old lease (%2 s left): "
                                        "the router took the write without renewing it — "
                                        "removing the entry and adding it again")
                             .arg(external)
                             .arg(leaseLeft);
}
} // namespace

RouterPortCore::RouterPortCore(IUpnpGateway& gateway, std::function<bool(uint16_t)> localBindProbe)
    : m_Gateway(gateway)
    , m_BindProbe(std::move(localBindProbe))
    , m_LeaseSec(mw::routerports::kLeaseSec)
{}

bool RouterPortCore::tunnelPortAllowed(bool devInstance, uint16_t port, uint16_t mediaBasePort)
{
    using namespace mw::routerports;
    if (port >= mediaBasePort && port < static_cast<uint32_t>(mediaBasePort) + kMediaPortCount)
        return false;
    if (devInstance) return port >= kDevTunnelPoolBegin && port <= kDevTunnelPoolEnd;
    for (const uint16_t p : kTunnelPreferred)
        if (p == port) return true;
    return port >= kTunnelPoolBegin && port <= kTunnelPoolEnd;
}

RouterPortCore::Request RouterPortCore::tunnelRequest(bool devInstance,
                                                      const QList<uint16_t>& remembered,
                                                      uint16_t mediaBasePort)
{
    using namespace mw::routerports;
    Request request;
    request.purpose = Purpose::Tunnel;
    request.devInstance = devInstance;
    request.mediaBasePort = mediaBasePort;
    request.description = QStringLiteral("MoonlightWeb tunnel");
    for (const uint16_t p : remembered)
        if (tunnelPortAllowed(devInstance, p, mediaBasePort)) request.remembered.append(p);
    if (devInstance) {
        request.poolBegin = kDevTunnelPoolBegin;
        request.poolEnd = kDevTunnelPoolEnd;
        return request;
    }
    for (const uint16_t p : kTunnelPreferred)
        if (tunnelPortAllowed(false, p, mediaBasePort)) request.preferred.append(p);
    request.poolBegin = kTunnelPoolBegin;
    request.poolEnd = kTunnelPoolEnd;
    return request;
}

bool RouterPortCore::holds(uint16_t externalPort) const
{
    for (const Claim& c : m_Held)
        if (c.external == externalPort) return true;
    return false;
}

QString RouterPortCore::ownerOf(uint16_t external)
{
    int leaseLeft = -1;
    return entryOf(external, leaseLeft);
}

QString RouterPortCore::entryOf(uint16_t external, int& leaseLeft)
{
    QString client;
    leaseLeft = -1;
    if (!m_Gateway.owner(external, kUdp, client, leaseLeft)) return {};
    return client;
}

bool RouterPortCore::tcpWritable(uint16_t external)
{
    QString client;
    int leaseLeft = -1;
    if (!m_Gateway.owner(external, kTcp, client, leaseLeft) || client.isEmpty()) return true;
    return client == m_Gateway.lanAddress() ||
           (!m_PreviousLan.isEmpty() && client == m_PreviousLan);
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

bool RouterPortCore::addAfresh(uint16_t external, uint16_t internal, const QString& description,
                               const QString& protocol)
{
    m_Gateway.remove(external, protocol);
    return addWithLatch(external, internal, description, protocol);
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
    for (int p = request.poolBegin; p <= request.poolEnd && p <= 65535; ++p) {
        const auto port = static_cast<uint16_t>(p);
        if (request.purpose == Purpose::Tunnel &&
            !tunnelPortAllowed(request.devInstance, port, request.mediaBasePort))
            continue;
        consider(port);
    }

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
    const uint16_t internal = tunnel ? external : request.internalPort;

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

    if (!addWithLatch(external, internal, request.description, kUdp)) {
        result.skipped.append({external, SkipReason::Refused, {}});
        return false;
    }

    // And the write is read back, because "added successfully" is the router's
    // opinion of the request, not of the table. An entry that answers with
    // another machine's address is a hole that leads somewhere else, and
    // advertising it would aim every connectivity check at that machine.
    int leaseLeft = -1;
    const QString after = entryOf(external, leaseLeft);
    if (!after.isEmpty() && after != lan) {
        result.skipped.append({external, SkipReason::KeptElsewhere, after});
        return false;
    }

    // "Added" can also mean "already there, left as it was": MiniUPnPd up to
    // 1.9 answers a write identical to an entry it holds without touching it,
    // lease included. An entry this host left behind (a crash, a kill) would
    // run out on its old lease within the hour; only a fresh one takes the
    // lease just asked for.
    const bool kept = !after.isEmpty() && keptOldLease(leaseLeft);
    if (kept) {
        logKeptLease(external, leaseLeft);
        if (!addAfresh(external, internal, request.description, kUdp)) {
            result.skipped.append({external, SkipReason::Refused, {}});
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
    claim.tcp = tcpWritable(external) &&
                (kept ? addAfresh(external, internal, request.description, kTcp)
                      : addWithLatch(external, internal, request.description, kTcp));
    m_Held.append(claim);
    result.claim = claim;
    return true;
}

bool RouterPortCore::rewrite(const Claim& c, QString& owner)
{
    const QString desc = descriptionOf(c);
    bool ok = addWithLatch(c.external, c.internal, desc, kUdp);
    // A neighbour may have taken the TCP number since: its entry is not rewritten.
    const bool tcp = c.tcp && tcpWritable(c.external);
    if (tcp) addWithLatch(c.external, c.internal, desc, kTcp);
    int leaseLeft = -1;
    owner = entryOf(c.external, leaseLeft);

    // The renewal the router ignored (see tryOne): the entry would run out on
    // the lease it was made with, an hour after the first stream, and every
    // stream after that would fail its connectivity checks until the next
    // half hour happened to land after the expiry. Only ever on an entry the
    // table says is this host's: a neighbour's is not removed.
    if (ok && owner == m_Gateway.lanAddress() && keptOldLease(leaseLeft)) {
        logKeptLease(c.external, leaseLeft);
        ok = addAfresh(c.external, c.internal, desc, kUdp);
        if (tcp) addAfresh(c.external, c.internal, desc, kTcp);
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
    for (const Lost& l : lost)
        m_Held.removeAll(l.claim);

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
        return {{c, owner}};
    }
    return {};
}

void RouterPortCore::release(const Claim& claim)
{
    if (!m_Held.contains(claim)) return;
    m_Gateway.remove(claim.external, kUdp);
    if (claim.tcp && tcpWritable(claim.external)) m_Gateway.remove(claim.external, kTcp);
    m_Held.removeAll(claim);
}

void RouterPortCore::releaseAll()
{
    const QList<Claim> held = m_Held;
    for (const Claim& c : held)
        release(c);
}
