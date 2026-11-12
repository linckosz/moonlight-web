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

#include "RouterHoles.h"
#include "SocketTables.h"

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

    /// What the router's table says of one external port.
    struct Entry
    {
        /// The machine it forwards to, as the router writes it.
        QString client;
        /// 0 when the router gave none that reads as a port.
        uint16_t internalPort = 0;
        QString description;
        /// The seconds the entry has left, 0 for a permanent one, -1 when the
        /// router does not say.
        int leaseLeft = -1;
    };

    enum class Lookup
    {
        /// The router has no entry for that port, and said exactly that.
        None,
        Found,
        /// Any other answer, and none at all.
        Unknown,
    };

    /// GetSpecificPortMappingEntry. To whoever asks whether a number is
    /// taken, None and Unknown are one answer, on purpose: an IGD that does
    /// not implement the query must not make the caller throw away mappings
    /// that are working. To whoever is about to remove an entry they are
    /// not: only Found is acted on, and only None says it is gone.
    virtual Lookup entry(uint16_t externalPort, const QString& protocol, Entry& out) = 0;

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
        Media,
        /// A port every stream shares (ice_udp_mux). Added last: the numbers
        /// of the two above travel in portLost().
        Mux
    };

    /// Which of the mux's ports a claim is, in Claim::slot and Request::slot.
    /// The conferencing port, from the tunnel's preferred list.
    static constexpr int kMuxStandard = 0;
    /// The port at the head of the stream block (RouterPortPools.h).
    static constexpr int kMuxBlock = 1;

    /// One hole in the router, as this host holds it.
    struct Claim
    {
        Purpose purpose = Purpose::Tunnel;
        /// Stream slot for a media claim; -1 for the tunnel; kMuxStandard or
        /// kMuxBlock for the mux.
        int slot = -1;
        /// What the router forwards, and what a browser is told.
        uint16_t external = 0;
        /// What this machine listens on. Equal to `external` for the tunnel
        /// and the mux.
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

    /// What became of a port the mux was asked to hold (Request::hold).
    enum class Hold
    {
        /// Not bound: another program on this machine has it.
        Failed,
        /// Bound for this claim. The one answer after which a claim that
        /// fails hands the port back.
        NewlyBound,
        /// The mux had it before this claim — one made again after the router
        /// lost the hole. Whatever the router says, it stays the mux's.
        AlreadyHeld,
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
        /// Whether the TCP hole on the same number is asked for alongside the
        /// UDP one. Only where this host listens in TCP on that port: a hole
        /// to a port nobody listens on leads to whichever program binds the
        /// number next. Off, the TCP entry is neither written, renewed nor
        /// removed.
        bool tcp = true;
        /// Mux only. The mux keeps its sockets for as long as the process
        /// runs, so "can this machine listen there" is answered by doing it:
        /// `hold(port)` binds the port and keeps it. Asked only once the
        /// router has nothing against the number, in place of the bind probe.
        /// Unset, the probe answers as it does for the tunnel.
        std::function<Hold(uint16_t)> hold;
        /// The router would not give the hole after all: the port `hold` bound
        /// for this claim is not wanted. Never called for a port the mux held
        /// before the claim.
        std::function<void(uint16_t)> drop;

        /// Whether the request names a port at all. One that names none is
        /// answered "none" by claim(), without a word to the router.
        bool hasCandidate() const
        {
            return !remembered.isEmpty() || !preferred.isEmpty() ||
                   (poolEnd != 0 && poolBegin <= poolEnd);
        }
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
    /// for a --dev (RouterPortPools.h).
    static bool tunnelPortAllowed(bool devInstance, uint16_t port);

    /// The tunnel's request. A remembered port the instance may not hold is
    /// left out — a --dev remembering 3479 from before it had a block.
    static Request tunnelRequest(bool devInstance, const QList<uint16_t>& remembered);

    /// Whether `port` may be the mux's port `which` (kMuxStandard, kMuxBlock)
    /// for this kind of instance: the conferencing list and the foot of the
    /// stream block for the installed host, the top of the block alone for a
    /// --dev, which has no conferencing port at all.
    static bool muxPortAllowed(bool devInstance, int which, uint16_t port);

    /// The request for one of the mux's ports, the caller's `hold` and `drop`
    /// left to set. A remembered port the instance may not hold is left out.
    /// For a --dev's conferencing port the request has no candidate, and
    /// claim() answers "none" without a word to the router.
    static Request muxRequest(bool devInstance, int which, const QList<uint16_t>& remembered);

    /// Whether `port` may be the router-side number of a stream slot's own
    /// hole: one of the stream block, or of the media pool.
    static bool mediaPortAllowed(uint16_t port);

    /// The request for a stream slot's own port, `internalPort`: the number
    /// the slot had on the router last time (`remembered`, 0 for none, left
    /// out when it is no number a slot's hole may have), then the port's own
    /// number, then the media pool. With the mux in use, no router-side
    /// number is taken from the mux's stretch of the block: those are the
    /// mux's, whenever it comes to ask for them. And a port the mux itself is
    /// bound to (`muxHoldsPort`) gets no hole at all: the hole would lead to
    /// the mux. The request then has no candidate.
    static Request mediaRequest(bool muxInUse, bool devInstance, int slot, uint16_t internalPort,
                                uint16_t remembered, bool muxHoldsPort);

    /// The address this host had when its remembered ports were obtained.
    /// An entry the router still points there is ours to overwrite.
    void setPreviousLanAddress(const QString& ip) { m_PreviousLan = ip; }

    /// The record of what this host writes (RouterHoles.h): every entry is
    /// noted in it before the router is asked, its description ends with the
    /// record's tag, and sweep() goes by it. Unset, nothing is noted, the
    /// descriptions are the product's words alone, and nothing is swept.
    void setRecord(mw::routerholes::IRecord* record) { m_Record = record; }

    /// Whether something on this machine listens on a port (SocketTables.h).
    /// Asked before an entry of a version that kept no record is removed.
    /// Unset, nothing is known, and no such entry is removed.
    void setListens(std::function<mw::sockettables::Listens(uint16_t port, bool tcp)> listens)
    {
        m_Listens = std::move(listens);
    }

    /// One port, from the first candidate the router lets this host have.
    /// Candidates already held by this core are passed over silently.
    Result claim(const Request& request);

    /// Re-add every held mapping, then read each back. Returns the ones the
    /// table now aims elsewhere — those are dropped from the held list, since
    /// a hole that leads to another machine is worse than none, and their
    /// TCP entry is taken off when it is still this host's. One still
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

    /// What became of one line of the record at the sweep.
    enum class Swept
    {
        /// Taken off the router, and seen gone.
        Removed,
        /// The router has no such entry any more: nothing to take off.
        Gone,
        /// The router's entry is not the one this host wrote (`detail` says
        /// by what): left as it is, and no longer in the record.
        NotOurs,
        /// The router did not say what is there, or not in a way that reads
        /// as an entry: left as it is, asked again at the next start.
        Unknown,
        /// The router refused the removal, or the entry was still there
        /// after it: asked again at the next start.
        Stayed,
        /// Noted under another address than this host has now: not looked at.
        OtherAddress,
        /// This run holds the port again: its claim wrote the entry anew.
        HeldAgain,
        /// Of a version that kept no record, and something on this machine
        /// listens on the port it leads to, or the system does not say
        /// whether anything does: left as it is, judged again at the next
        /// start.
        InUse,
    };

    struct SweepResult
    {
        Swept outcome = Swept::Unknown;
        /// NotOurs: "address", "internal port" or "description". InUse:
        /// "listened", or "unknown" when the system does not say.
        QString detail;
    };

    /// The lines of the record a sweep looks at: those noted under the
    /// address this host has now, of ports this run does not hold.
    QList<mw::routerholes::Hole> sweepList() const;

    /// Asks the router the one question whose answer is known: a port no
    /// MoonlightWeb ever asks for (kKnownQuestionPort), in each protocol
    /// `holes` has. True when it answers "no such entry" and nothing else:
    /// only then is what it says of the other ports believed.
    bool sweepBelieved(const QList<mw::routerholes::Hole>& holes);

    /// Takes back one entry an earlier run of this host left on the router,
    /// if it is still there and still provably this host's. All of these
    /// must hold: the router answers with an entry that reads as one; its
    /// internal client is the address this host has now, the one the line
    /// was noted under; its internal port is the noted one; its description
    /// is the noted one to the letter, this installation's tag included. For
    /// a line of a version that put no tag, also: nothing on this machine
    /// listens on that internal port. The removal is read back. Whatever
    /// else the router says, nothing is removed.
    SweepResult sweep(const mw::routerholes::Hole& hole);

    QList<Claim> held() const { return m_Held; }
    bool holds(uint16_t externalPort) const;
    uint32_t leaseSec() const { return m_LeaseSec; }
    int renewFailures() const { return m_RenewFailures; }

private:
    /// One write to the router's table.
    struct Write
    {
        uint16_t external = 0;
        uint16_t internal = 0;
        Purpose purpose = Purpose::Tunnel;
        int slot = -1;
        bool tcp = false;
    };

    bool tryOne(uint16_t external, const Request& request, Result& result);
    /// Empty when the router has no entry, or cannot say.
    QString ownerOf(uint16_t external);
    /// Whether the TCP entry of `external` may be written or removed: none, one
    /// this host holds (or held before a DHCP renumbering), or a router that
    /// cannot say. A neighbour's TCP forward on the same number is not ours,
    /// whoever holds the UDP one. `seen` gets the entry, when there is one.
    bool tcpWritable(uint16_t external, IUpnpGateway::Entry* seen = nullptr);
    /// Whether an entry read back just after a write still counts down the
    /// lease it had before it: the router took the write and kept the entry.
    bool keptOldLease(int leaseLeft) const;
    /// Whether an entry of this host shows another description than the one
    /// `w` writes. Never so without a record: the description then has
    /// nothing to prove.
    bool keptOldText(const Write& w, const IUpnpGateway::Entry& shown) const;
    /// What the router's page shows for such an entry: the product's words,
    /// then the record's tag when there is a record.
    QString textOf(Purpose purpose, int slot) const;
    /// The write itself, noted in the record before the router is asked; one
    /// the router refuses leaves the record as it was. `afresh`: remove,
    /// then add — the one write a router that kept the old lease, or the old
    /// description, acts on.
    bool write(const Write& w, bool afresh = false);
    bool addWithLatch(uint16_t external, uint16_t internal, const QString& description,
                      const QString& protocol);
    void strike(uint16_t external, bool tcp);
    /// A held hole went to another machine. Nothing renews its TCP entry any
    /// more: it is taken off at once, when the router shows it as the entry
    /// this host wrote: at its address, to its port, under its description.
    void dropTcpOf(const Claim& lost);
    /// Whether this core holds the entry of that port in that protocol.
    bool holdsLine(uint16_t external, bool tcp) const;
    /// One renewal of a held mapping: UDP and TCP written, UDP read back into
    /// `owner`, the pair added afresh if the router kept its old lease. False
    /// when the router refused the UDP write.
    bool rewrite(const Claim& claim, QString& owner);

    IUpnpGateway& m_Gateway;
    std::function<bool(uint16_t)> m_BindProbe;
    mw::routerholes::IRecord* m_Record = nullptr;
    std::function<mw::sockettables::Listens(uint16_t, bool)> m_Listens;
    QString m_PreviousLan;
    QList<Claim> m_Held;
    uint32_t m_LeaseSec;
    int m_RenewFailures = 0;
};
