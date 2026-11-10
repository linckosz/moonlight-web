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

#pragma once

#include "RouterPortPools.h"

#include <QList>
#include <QString>

#include <cstdint>
#include <functional>

/// The router, as the allocation policy sees it. UPNPClient behind it in
/// production; a table in the unit tests, which is the point of the seam —
/// the policy is where every LAN-sharing bug lives, and it has to be
/// exercised without a gateway on the desk.
class IUpnpGateway
{
public:
    virtual ~IUpnpGateway() = default;

    /// This machine's address on the gateway-facing interface.
    virtual QString lanAddress() const = 0;

    /// AddPortMapping. `leaseSec` 0 = permanent.
    virtual bool add(uint16_t externalPort, uint16_t internalPort, uint32_t leaseSec,
                     const QString& description, const QString& protocol) = 0;

    /// GetSpecificPortMappingEntry: true and `client` filled when the router
    /// has an entry for that external port. False when it has none OR cannot
    /// answer the question — and the two are reported alike on purpose: an IGD
    /// that does not implement the query must not make the caller throw away
    /// mappings that are working. `leaseLeft`: the seconds the router says the
    /// entry has left, 0 for a permanent one, -1 when it does not say.
    virtual bool owner(uint16_t externalPort, const QString& protocol, QString& client,
                       int& leaseLeft) = 0;

    virtual bool remove(uint16_t externalPort, const QString& protocol) = 0;
};

/// The allocation policy: one router, several MoonlightWeb hosts, and the
/// rule that nobody takes what a neighbour holds.
///
/// Synchronous and thread-agnostic. RouterPortAllocator runs it on a thread
/// of its own, since every call here is a SOAP round trip to the router.
class RouterPortCore
{
public:
    enum class Purpose
    {
        Tunnel,
        Media
    };

    /// One hole in the router, as this host holds it.
    struct Claim
    {
        Purpose purpose = Purpose::Tunnel;
        /// Stream slot for a media claim; -1 for the tunnel.
        int slot = -1;
        /// What the router forwards, and what a browser is told.
        uint16_t external = 0;
        /// What this machine listens on. Equal to `external` for the tunnel.
        uint16_t internal = 0;
        /// Whether the TCP mapping alongside the UDP one was accepted.
        bool tcp = false;

        bool operator==(const Claim& o) const
        {
            return purpose == o.purpose && slot == o.slot && external == o.external &&
                   internal == o.internal;
        }
    };

    /// Why a candidate was passed over. Every one of these is a log line, and
    /// the log is what a person diagnosing a dead tunnel reads first.
    enum class SkipReason
    {
        /// Another program on this machine listens there.
        LocalBusy,
        /// The router forwards it to another machine on this LAN.
        Foreign,
        /// The router refused both the leased and the permanent mapping.
        Refused,
        /// The write was accepted, yet the table still points elsewhere.
        KeptElsewhere,
    };

    struct Skipped
    {
        uint16_t port = 0;
        SkipReason reason = SkipReason::Foreign;
        /// The other machine, for Foreign and KeptElsewhere.
        QString owner;
    };

    struct Request
    {
        Purpose purpose = Purpose::Tunnel;
        int slot = -1;
        /// Media only: the port the stream binds. The tunnel binds the
        /// external port itself, so this is ignored there.
        uint16_t internalPort = 0;
        /// Tried first, in order: what this host held last time.
        QList<uint16_t> remembered;
        /// Then these, in order.
        QList<uint16_t> preferred;
        /// Then this range, ascending. Empty when begin > end.
        uint16_t poolBegin = 0;
        uint16_t poolEnd = 0;
        bool devInstance = false;
        uint16_t mediaBasePort = mw::routerports::kMediaBasePort;
        /// What the router's admin page shows for the entry.
        QString description;
    };

    struct Result
    {
        bool ok = false;
        Claim claim;
        QList<Skipped> skipped;
        /// Set when a remembered entry pointed at this host's PREVIOUS LAN
        /// address and was taken back rather than treated as a neighbour's.
        QString reclaimedFrom;
    };

    /// A hole the router now aims at somebody else, found at renewal.
    struct Lost
    {
        Claim claim;
        QString owner;
    };

    /// `localBindProbe(port)`: whether this machine can listen there right
    /// now. Asked before a tunnel mapping is made, so a port some other
    /// program holds is skipped rather than mapped to a hole that leads
    /// nowhere — a router entry pointing at a closed port is worse than no
    /// entry, because ICE would advertise it.
    RouterPortCore(IUpnpGateway& gateway, std::function<bool(uint16_t)> localBindProbe);

    /// Whether a tunnel hole on `port` is one this kind of instance may hold:
    /// the preferred list and the pool for the installed host, the --dev block
    /// for a --dev (RouterPortPools.h). Any port in [mediaBasePort, mediaBasePort + kMediaPortCount)
    /// is disallowed.
    static bool tunnelPortAllowed(bool devInstance, uint16_t port,
                                  uint16_t mediaBasePort = mw::routerports::kMediaBasePort);

    /// The tunnel's request. A remembered port the instance may not hold is
    /// left out — a --dev remembering 3479 from before it had a block.
    static Request tunnelRequest(bool devInstance, const QList<uint16_t>& remembered,
                                 uint16_t mediaBasePort = mw::routerports::kMediaBasePort);

    /// The address this host had when its remembered ports were obtained.
    /// An entry the router still points there is ours to overwrite.
    void setPreviousLanAddress(const QString& ip) { m_PreviousLan = ip; }

    /// One port, from the first candidate the router lets this host have.
    /// Candidates already held by this core are passed over silently.
    Result claim(const Request& request);

    /// Re-add every held mapping, then read each back. Returns the ones the
    /// table now aims elsewhere — those are dropped from the held list, since
    /// a hole that leads to another machine is worse than none. One still
    /// running down its old lease after the write is removed and added
    /// afresh: some routers answer a re-add without renewing anything. Also
    /// drives the lease latch: two consecutive renewal failures switch to
    /// permanent mappings and re-add once more.
    QList<Lost> renew();

    /// The same for one held mapping, now: a stream or a browser about to be
    /// sent to it, after a router reboot or a night's sleep the half-hour
    /// renewal has not caught up with yet. Leaves the failure count alone.
    /// Nothing for a port this core does not hold.
    QList<Lost> renew(uint16_t externalPort);

    /// Remove one held mapping (UDP, and TCP if it was accepted).
    void release(const Claim& claim);
    void releaseAll();

    QList<Claim> held() const { return m_Held; }
    bool holds(uint16_t externalPort) const;
    uint32_t leaseSec() const { return m_LeaseSec; }
    int renewFailures() const { return m_RenewFailures; }

private:
    bool tryOne(uint16_t external, const Request& request, Result& result);
    /// Empty when the router has no entry, or cannot say.
    QString ownerOf(uint16_t external);
    /// The same, with the lease the entry has left (see IUpnpGateway::owner).
    QString entryOf(uint16_t external, int& leaseLeft);
    /// Whether the TCP entry of `external` may be written or removed: none, one
    /// this host holds (or held before a DHCP renumbering), or a router that
    /// cannot say. A neighbour's TCP forward on the same number is not ours,
    /// whoever holds the UDP one.
    bool tcpWritable(uint16_t external);
    /// Whether an entry read back just after a write still counts down the
    /// lease it had before it: the router took the write and kept the entry.
    bool keptOldLease(int leaseLeft) const;
    bool addWithLatch(uint16_t external, uint16_t internal, const QString& description,
                      const QString& protocol);
    /// Remove, then add: the one write such a router acts on.
    bool addAfresh(uint16_t external, uint16_t internal, const QString& description,
                   const QString& protocol);
    /// One renewal of a held mapping: UDP and TCP written, UDP read back into
    /// `owner`, the pair added afresh if the router kept its old lease. False
    /// when the router refused the UDP write.
    bool rewrite(const Claim& claim, QString& owner);

    IUpnpGateway& m_Gateway;
    std::function<bool(uint16_t)> m_BindProbe;
    QString m_PreviousLan;
    QList<Claim> m_Held;
    uint32_t m_LeaseSec;
    int m_RenewFailures = 0;
};
