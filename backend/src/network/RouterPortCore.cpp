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

namespace {
const QString kUdp = QStringLiteral("UDP");
const QString kTcp = QStringLiteral("TCP");
} // namespace

RouterPortCore::RouterPortCore(IUpnpGateway& gateway, std::function<bool(uint16_t)> localBindProbe)
    : m_Gateway(gateway)
    , m_BindProbe(std::move(localBindProbe))
    , m_LeaseSec(mw::routerports::kLeaseSec)
{}

bool RouterPortCore::holds(uint16_t externalPort) const
{
    for (const Claim& c : m_Held)
        if (c.external == externalPort) return true;
    return false;
}

QString RouterPortCore::ownerOf(uint16_t external)
{
    QString client;
    if (!m_Gateway.owner(external, kUdp, client)) return {};
    return client;
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
    const QString after = ownerOf(external);
    if (!after.isEmpty() && after != lan) {
        result.skipped.append({external, SkipReason::KeptElsewhere, after});
        return false;
    }

    Claim claim;
    claim.purpose = request.purpose;
    claim.slot = request.slot;
    claim.external = external;
    claim.internal = internal;
    // Best effort, and only useful where UDP is blocked outright: the
    // connections enable ICE-TCP, but a browser only ever opens those
    // outbound, so reaching this machine needs the inbound hole too.
    claim.tcp = addWithLatch(external, internal, request.description, kTcp);
    m_Held.append(claim);
    result.claim = claim;
    return true;
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
        const QString desc = c.purpose == Purpose::Tunnel
                                 ? QStringLiteral("MoonlightWeb tunnel")
                                 : QStringLiteral("MoonlightWeb media slot %1").arg(c.slot);
        if (!addWithLatch(c.external, c.internal, desc, kUdp)) ok = false;
        if (c.tcp) addWithLatch(c.external, c.internal, desc, kTcp);
        const QString owner = ownerOf(c.external);
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
    // mappings when the process ends.
    if (++m_RenewFailures >= 2 && m_LeaseSec > 0) {
        m_LeaseSec = 0;
        for (const Claim& c : std::as_const(m_Held)) {
            const QString desc = c.purpose == Purpose::Tunnel
                                     ? QStringLiteral("MoonlightWeb tunnel")
                                     : QStringLiteral("MoonlightWeb media slot %1").arg(c.slot);
            m_Gateway.add(c.external, c.internal, 0, desc, kUdp);
            if (c.tcp) m_Gateway.add(c.external, c.internal, 0, desc, kTcp);
        }
    }
    return lost;
}

void RouterPortCore::release(const Claim& claim)
{
    if (!m_Held.contains(claim)) return;
    m_Gateway.remove(claim.external, kUdp);
    if (claim.tcp) m_Gateway.remove(claim.external, kTcp);
    m_Held.removeAll(claim);
}

void RouterPortCore::releaseAll()
{
    const QList<Claim> held = m_Held;
    for (const Claim& c : held)
        release(c);
}
