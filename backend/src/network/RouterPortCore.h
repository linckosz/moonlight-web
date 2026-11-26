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
#include <QPair>
#include <QSet>
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

    /// This machine's address on the gateway-facing interface, as it was when
    /// the gateway was found: what every write names as its client.
    virtual QString lanAddress() const = 0;

    /// The address the system would send from towards the gateway now. It
    /// differs from lanAddress() once a DHCP lease has renumbered this
    /// machine under a running server. Empty when the system does not say.
    virtual QString addressNow() const { return lanAddress(); }

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
        /// The description the router showed of the entry once it was
        /// written: its own words, for a router that puts its own. Empty when
        /// it showed nothing. Not part of what a claim is.
        QString text;

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
        /// The router forwards it to this machine, under a description that is
        /// not this installation's: another edition's, or another
        /// installation's, on this machine. Never its address nor its tag in
        /// the log.
        OtherInstallation,
        /// This installation's entry towards an address this host had before
        /// (formerOurs()), which the router would not take off: it is not
        /// written over, and the number is left. `owner` is that address.
        FormerStays,
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
        /// The address this host had before, when the router still had this
        /// installation's entry for the number towards it: set only once that
        /// entry was taken off, seen gone, and the number written here.
        QString reclaimedFrom;
    };

    /// A hole the router now aims at somebody else, found at renewal.
    struct Lost
    {
        Claim claim;
        QString owner;
        /// At this machine's address still, but another installation's entry
        /// by its description: another edition on this machine wrote it.
        bool here = false;
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
    /// Whether a tunnel hole on `port` may be asked for in this run: one of
    /// the owner's list when it is in force (router_ports_allowed,
    /// AllowedPorts.h), tunnelPortAllowed() otherwise. A list that was refused
    /// asks for nothing at all, which the allocator sees to. What is taken
    /// back goes by tunnelPortAllowed() and the list (RouterHoles' allowed()).
    static bool tunnelPortAsked(bool devInstance, uint16_t port);

    /// The tunnel's request. A remembered port the instance may not hold is
    /// left out — a --dev remembering 3479 from before it had a block.
    static Request tunnelRequest(bool devInstance, const QList<uint16_t>& remembered);

    /// Whether `port` may be the mux's port `which` (kMuxStandard, kMuxBlock)
    /// for this kind of instance: the conferencing list and the foot of the
    /// stream block for the installed host, the top of the block alone for a
    /// --dev, which has no conferencing port at all.
    static bool muxPortAllowed(bool devInstance, int which, uint16_t port);
    /// The same, for what may be asked for in this run (tunnelPortAsked()).
    static bool muxPortAsked(bool devInstance, int which, uint16_t port);

    /// The request for one of the mux's ports, the caller's `hold` and `drop`
    /// left to set. A remembered port the instance may not hold is left out.
    /// For a --dev's conferencing port the request has no candidate, and
    /// claim() answers "none" without a word to the router.
    static Request muxRequest(bool devInstance, int which, const QList<uint16_t>& remembered);

    /// Whether `port` may be the router-side number of a stream slot's own
    /// hole for this kind of instance: one of its stream block, or of its
    /// media pool (RouterPortPools.h).
    static bool mediaPortAllowed(bool devInstance, uint16_t port);
    /// The same, for what may be asked for in this run (tunnelPortAsked()).
    static bool mediaPortAsked(bool devInstance, uint16_t port);

    /// The request for a stream slot's own port, `internalPort`: the number
    /// the slot had on the router last time (`remembered`, 0 for none, left
    /// out when it is no number a slot's hole may have for this kind of
    /// instance), then the port's own number, then the instance's media
    /// pool. With the mux in use, no router-side
    /// number is taken from the mux's stretch of the block: those are the
    /// mux's, whenever it comes to ask for them. And a port the mux itself is
    /// bound to (`muxHoldsPort`) gets no hole at all: the hole would lead to
    /// the mux. The request then has no candidate.
    static Request mediaRequest(bool muxInUse, bool devInstance, int slot, uint16_t internalPort,
                                uint16_t remembered, bool muxHoldsPort);

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

    /// Whether another MoonlightWeb server runs on this machine
    /// (SocketTables.h): Yes, No, or Unknown when the system does not say.
    /// Asked before an untagged stream slot entry is taken back or taken
    /// over. Unset, nothing is known, and no such entry is.
    void setOtherServer(std::function<mw::sockettables::Listens()> otherServer)
    {
        m_OtherServer = std::move(otherServer);
    }

    /// Whether this core walks a DEV identity's ports (PortIdentity.h). A DEV
    /// identity never takes over an untagged stream slot entry.
    void setDevInstance(bool devInstance) { m_DevInstance = devInstance; }

    /// Looks for the gateway again, from the address this machine has now
    /// (followAddress()). True once it answered: lanAddress() then says what
    /// it sees. Unset, a renumbering under a running server is not followed.
    void setRediscover(std::function<bool()> rediscover) { m_Rediscover = std::move(rediscover); }

    /// One port, from the first candidate the router lets this host have.
    /// Candidates already held by this core are passed over silently. An
    /// entry the router already has for a candidate is taken over only when
    /// it is this installation's: at this host's address and under a
    /// description with this record's tag, or, for an entry of a version that
    /// put no tag, under the rule of earlierOurs(). One towards an address
    /// this host had before is taken off first, and seen gone, when it is
    /// still as this installation wrote it there (formerOurs()); one the
    /// router keeps leaves the number. Anyone else's is passed over, and left
    /// as it is.
    Result claim(const Request& request);

    /// This machine's address on the router's LAN is not the one the router
    /// was found from (a DHCP lease renumbered it under a running server):
    /// the gateway is looked for again (setRediscover()). Once it answers
    /// from another address, every held hole is moved there: its entries
    /// towards the address this host had are taken off, when still as this
    /// installation wrote them (formerOurs()), and seen gone, then it is
    /// written towards the address of now. Returns the holes that could not
    /// be: one whose entry the router keeps towards the former address (its
    /// line stays, formerStays() says so), and those the router shows as
    /// another's once rewritten, as renew() does. Nothing when nothing moved.
    /// `again`: look for the gateway even when it did not answer the last
    /// time this same address was seen (a renewal); a claim does not wait for
    /// a discovery that failed already.
    QList<Lost> followAddress(bool again);

    /// Whether this machine's address now is another than the one every
    /// write names (lanAddress()): a renumbering not followed yet, because
    /// the gateway did not answer from the new address. renew() writes
    /// nothing meanwhile: it would aim every hole at an address this machine
    /// no longer has.
    bool addressMoved() const;

    /// The router keeps an entry this installation wrote towards an address
    /// this machine had before, after its removal was asked (at a claim, a
    /// sweep or a move): the person who runs the router has to remove it.
    /// False again once its line is struck. Never its address or its number
    /// outside this core: the admin page has the words.
    bool formerStays() const { return !m_FormerStays.isEmpty(); }

    /// Re-add every held mapping, then read each back. Each is read first:
    /// one the router now aims at another machine, or shows at this host's
    /// address as another installation's, is not written over. Returns those
    /// — they are dropped from the held list, since a hole that leads
    /// elsewhere is worse than none, and their TCP entry is taken off when it
    /// is still this host's. One still running down its old lease after the
    /// write is removed and added afresh: some routers answer a re-add
    /// without renewing anything. Also drives the lease latch: two
    /// consecutive renewal failures switch to permanent mappings and re-add
    /// once more.
    QList<Lost> renew();

    /// The same for one held mapping, now: a stream or a browser about to be
    /// sent to it, after a router reboot or a night's sleep the half-hour
    /// renewal has not caught up with yet. Leaves the failure count alone.
    /// Nothing for a port this core does not hold.
    QList<Lost> renew(uint16_t externalPort);

    /// Remove one held mapping (UDP, and TCP if it was accepted), each only
    /// while the router shows it as this host's entry: one that is another
    /// machine's or another installation's by now is left, and so said. A
    /// router that does not say is asked to remove it as before.
    void release(const Claim& claim);
    void releaseAll();

    /// What a release before the process quits came to.
    struct Released
    {
        /// Claims whose every entry the router acknowledged as taken off.
        int released = 0;
        /// Claims with an entry left on the router: not tried before the
        /// deadline, or not acknowledged. Their lines stay in the record.
        QList<Claim> left;
    };

    /// The process is quitting: from now on nothing is written to the router
    /// (claim(), renew() and sweep() give nothing), then every held mapping
    /// is taken off, the streams' shared ports first, then the tunnel's, then
    /// the slots', each only while the router shows it as this host's, as
    /// with release(). No call to the router is begun once @p expired says
    /// true; one under way is not cut. Removals the router does not
    /// acknowledge stay in the record.
    Released releaseBeforeExit(const std::function<bool()>& expired);

    /// No more writes to the router (releaseBeforeExit() begins with it).
    void close() { m_Closed = true; }
    bool closed() const { return m_Closed; }

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
        /// Of a version that kept no record, or the entry on TCP 443, noted
        /// under another address than this host has now: not looked at. 443
        /// has a rule of its own (Tcp443Guard::formerAddress()).
        OtherAddress,
        /// Noted under an address this machine had before, and still as this
        /// installation wrote it there (formerOurs()): taken off the router,
        /// and seen gone.
        RemovedFormer,
        /// This run holds the port again: its claim wrote the entry anew.
        HeldAgain,
        /// Of a version that kept no record, and something on this machine
        /// listens on the port it leads to, or the system does not say
        /// whether anything does; for a stream slot's, or another
        /// MoonlightWeb server runs here, or the system does not say: left as
        /// it is, judged again at the next start.
        InUse,
    };

    struct SweepResult
    {
        Swept outcome = Swept::Unknown;
        /// NotOurs: "address", "internal port" or "description". InUse:
        /// "listened", or "unknown" when the system does not say; "server"
        /// when another MoonlightWeb server runs here, or "server unknown"
        /// when the system does not say whether one does.
        QString detail;
        /// The line was noted under an address this machine no longer has.
        bool former = false;
    };

    /// What sweepList() left out, in numbers only (for the log).
    struct SweepCount
    {
        /// The record's lines of this kind of instance.
        int lines = 0;
        /// Noted under another address of this machine than the one it has now.
        int elsewhere = 0;
        /// Of those, the ones the list has all the same (formerOurs()).
        int former = 0;
        /// Of ports this run holds.
        int held = 0;
    };

    /// The lines of the record a sweep looks at: those noted under the
    /// address this host has now, of ports this run does not hold; then
    /// those noted under an address this machine had before, all but the
    /// lines of a version that kept no record and the line of TCP 443.
    QList<mw::routerholes::Hole> sweepList(SweepCount* count = nullptr) const;

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
    /// listens on that internal port; and, for a stream slot's, no other
    /// MoonlightWeb server runs on this machine (asked first, and again just
    /// before the removal), since every edition up to 0.4.3 wrote those
    /// words to those ports. The removal is read back. Whatever else the
    /// router says, nothing is removed.
    ///
    /// A line noted under an address this machine had before is taken back
    /// under formerOurs() instead: the entry is towards that address, to the
    /// noted port, under the noted description, this installation's tag
    /// included. One that reads otherwise is another's, and no longer in the
    /// record; one the router keeps stays, and formerStays() says so.
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

    /// Whose the router's entry of one number and protocol is.
    enum class Whose
    {
        /// None, or a router that does not say: nothing to leave alone.
        Free,
        /// This host's: one it may write over, or take off.
        Ours,
        /// Another machine's: `shown.client` says whose.
        Foreign,
        /// At this host's address, another installation's by its description.
        Other,
        /// A candidate's entry towards an address this host had before, still
        /// as this installation wrote it there (formerOurs()): to be taken
        /// off, and seen gone, before the number is written here.
        Former,
    };
    /// What the router shows of `external` in that protocol, and whose it is.
    /// For a held claim (`held`): at this host's address, to the port it
    /// binds, and under no other installation's words (its own text, the
    /// words the router showed at the claim, or words that are not a
    /// MoonlightWeb's at all). For a candidate (`held` null): at this host's
    /// address under this record's tag, or an entry of a version that put no
    /// tag under earlierOurs(); or Former. `shown` gets the entry when there
    /// is one.
    Whose judge(uint16_t external, bool tcp, const Claim* held, IUpnpGateway::Entry& shown);
    /// The same of an entry the router showed (Found, with a client).
    Whose whoseIs(const IUpnpGateway::Entry& shown, uint16_t external, bool tcp, const Claim* held);
    /// Whether a description is one a MoonlightWeb writes, describe() of a
    /// kind, a slot, and no tag or a tag; `tag` gets the tag.
    static bool moonlightText(const QString& description, QString* tag);
    /// Whether a description is one this installation writes: a
    /// MoonlightWeb's, with this record's tag. Never so without a record.
    bool taggedOurs(const QString& description) const;
    /// Whether an entry of a version that put no tag, at this host's address,
    /// is this installation's own to take over: the record holds that very
    /// line as one of an earlier version (`old`), noted under this address;
    /// the router shows it to the noted port under that version's words; and
    /// nothing on this machine listens on that port in that protocol. For a
    /// stream slot's, also: no other MoonlightWeb server runs here, and this
    /// is the installed host's identity, the only one that binds that block
    /// now.
    bool earlierOurs(uint16_t external, bool tcp, const IUpnpGateway::Entry& shown);
    /// Whether an entry the router shows is the one this installation wrote
    /// under an address this machine no longer has, by its record AND by its
    /// words, never by the address alone. All of these must hold: the line is
    /// this record's and not of a version that kept no record (an untagged
    /// line proves nothing at another address), nor the line of TCP 443 (its
    /// own rule: Tcp443Guard::formerAddress()); it was noted under another
    /// address than lanAddress(); the entry reads as one, towards that very
    /// address, to the noted port, under the noted description with this
    /// record's tag. Another installation's carries another tag, and an
    /// edition beside this one keeps another record: neither is ever touched.
    bool formerOurs(const mw::routerholes::Hole& line, const IUpnpGateway::Entry& shown) const;
    /// The same with the record's line of that number and protocol.
    bool formerOurs(uint16_t external, bool tcp, const IUpnpGateway::Entry& shown) const;
    /// Takes off an entry formerOurs() said is this installation's towards a
    /// former address, and reads it back. True when the router has none any
    /// more: the line is struck. False: the line stays, and formerStays()
    /// says so until it is struck.
    bool takeOffFormer(uint16_t external, bool tcp);
    /// sweep() of a line noted under an address this machine had before.
    SweepResult sweepFormer(const mw::routerholes::Hole& line);
    /// Whether another MoonlightWeb server runs here, as setOtherServer() says.
    mw::sockettables::Listens otherServer() const;
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
    /// Takes one held entry off, when the router shows it as this host's
    /// (judge()); `left` says it is still there after a removal that was
    /// asked for. False when nothing was asked of the router: another's
    /// entry, or one already gone. `expired` is asked before each call.
    bool takeOff(const Claim& claim, bool tcp, const std::function<bool()>& expired, bool& left);
    /// Whether this core holds the entry of that port in that protocol.
    bool holdsLine(uint16_t external, bool tcp) const;
    /// One renewal of a held mapping: UDP and TCP written, UDP read back into
    /// `owner`, the pair added afresh if the router kept its old lease. False
    /// when the router refused the UDP write. Nothing is written when the
    /// router shows the entry as another machine's (`owner` says whose) or
    /// another installation's on this one (`here`, `owner` this address).
    bool rewrite(const Claim& claim, QString& owner, bool& here);

    IUpnpGateway& m_Gateway;
    std::function<bool(uint16_t)> m_BindProbe;
    mw::routerholes::IRecord* m_Record = nullptr;
    std::function<mw::sockettables::Listens(uint16_t, bool)> m_Listens;
    std::function<mw::sockettables::Listens()> m_OtherServer;
    bool m_DevInstance = false;
    std::function<bool()> m_Rediscover;
    /// The address this machine had now when the gateway last did not answer
    /// from it (followAddress()): a claim does not look again from there.
    QString m_UnansweredFrom;
    /// The address this machine had now when the gateway, found again from
    /// it, still saw it at lanAddress(): not a move (addressMoved()).
    QString m_SameFrom;
    QList<Claim> m_Held;
    /// Lines whose entry towards a former address the router kept when its
    /// removal was asked: (external port, TCP).
    QSet<QPair<uint16_t, bool>> m_FormerStays;
    uint32_t m_LeaseSec;
    int m_RenewFailures = 0;
    bool m_Closed = false;
};
