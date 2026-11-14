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
#include "RouterPortCore.h"

#include <QJsonObject>
#include <QList>
#include <QString>

#include <cstdint>
#include <functional>

/// TCP 443 on the router, for a browser on a network that lets out nothing
/// else (settings.json ice_tcp_443): one entry, TCP only, from the router's
/// 443 to the port every stream shares, where the server listens in TCP
/// (network/IceMuxTcp). Nothing of this host binds 443.
///
/// 443 is the port people forward by hand, and the one a router keeps for
/// itself. So the host asks for it only when it can show, alone, that nobody
/// has it and that it then leads here; on any doubt it leaves 443 as it is.
/// What decides is here, over three seams: the router, the record of what
/// this installation wrote there, and a probe that dials. docs/design/ice-mux.md
namespace mw::tcp443 {

inline constexpr uint16_t kPort = 443;

/// What the guard learns by dialling, which no router can be asked: one dial,
/// told whole. Whoever makes it needs nothing else, and keeps nothing from
/// one dial to the next.
struct Dial
{
    enum class Kind
    {
        None,
        /// `address:port` is dialled and sent one frame, which names a
        /// fragment made for this one try: yes when that very frame came in
        /// at this host's own TCP listener, on the shared port `leadsHereOn`,
        /// within the time a try has.
        ArrivesHere,
        /// `address:port` is dialled and sent nothing: yes when something
        /// there completes the handshake within that time.
        Answers,
    };
    Kind kind = Kind::None;
    QString address;
    uint16_t port = 0;
    /// The shared port a connection that leads here comes in on: the block
    /// port, which step 3 dials and to which 443 leads at step 5. Nothing to
    /// Answers.
    uint16_t leadsHereOn = 0;
};

/// What makes a dial and waits for what comes of it: for as long as a try
/// has, each time. So never on the thread the router is asked from, where
/// every other request would wait behind it (IDialler).
class IProbe
{
public:
    virtual ~IProbe() = default;
    virtual bool make(const Dial& dial) = 0;
};

/// What has the dials made on a thread that does nothing else, one at a time
/// (Tcp443Probe's Dialler). What came of a dial is told later, with the
/// number it was asked under, to Keeper::dialled() on the keeper's thread.
class IDialler
{
public:
    virtual ~IDialler() = default;
    /// That dial is to be made, for the run `run`. Asked only once the dial
    /// asked before it has been answered. False when it will not be made:
    /// nothing is then told of it.
    virtual bool ask(uint32_t run, const Dial& dial) = 0;
    /// The dial that was asked and is not answered yet is no longer wanted:
    /// it ends soon, and is answered all the same, with a no.
    virtual void cancel() = 0;
};

/// An IPv4 address in figures that the Internet routes to one place: not
/// private, not an operator's shared range (100.64/10), not link-local,
/// loopback, multicast or reserved, and not one of the ranges kept for
/// documentation or for a relay.
bool publicIpv4(const QString& address);

/// What the guard is run on.
struct Situation
{
    /// Every switch it takes is on (ice_udp_mux, ice_tcp_passive,
    /// ice_tcp_443, UPnP, Internet Access), and the run is not a DEV
    /// identity's.
    bool wanted = false;
    /// What browsers reach this host at.
    QString publicAddress;
    /// What the router says its own outer address is.
    QString routerAddress;
    /// The shared block port in service, when the router forwards it in TCP:
    /// where 443 would lead. 0 otherwise.
    uint16_t blockPort = 0;
    /// The lease the other holes are asked with, 0 for a permanent entry.
    uint32_t leaseSec = 0;
};

enum class Outcome
{
    /// The router forwards TCP 443 to the block port of this machine: read
    /// back, and, when it was just written, proved by a connection.
    Held,
    /// 443 is left as it is.
    Left,
    /// It was held and no longer is. Nothing is written again by this call:
    /// the next attempt is a whole run.
    Lost,
};

struct Verdict
{
    Outcome outcome = Outcome::Left;
    /// The step that said so, 1 to 6; 0 when the guard was not to run at all.
    int step = 0;
    /// The step in words, for the log. Never an address, another's port, or
    /// the text of another's entry.
    QString said;
};

/// Where a run stands after one of its steps: over, with what it found; or
/// waiting for a dial, whose answer Guard::after() takes.
struct Step
{
    Dial dial;
    Verdict verdict;
    /// Over, and paced as a run that dialled (Pace): the next one waits
    /// kApartMs, whatever step said it.
    bool apart = false;
    bool over() const { return dial.kind == Dial::Kind::None; }
};

class Guard
{
public:
    /// `record` null: a run that keeps no record, and so never asks for 443.
    Guard(IUpnpGateway& gateway, mw::routerholes::IRecord* record);

    /// The whole run, a step at a time. What asks the router is done here,
    /// on the caller's thread; what dials is asked of the caller, who has it
    /// made elsewhere and comes back with the answer (after()): between two
    /// steps that thread is free for every other request. In this order, and
    /// the first step that fails ends the run:
    ///   1  the address browsers reach is a public IPv4 one, and the router's
    ///      own: no second router in front of it;
    ///   2  the router says it has no TCP entry on 443. An entry, whoever it
    ///      leads to, or an answer that says nothing, ends the run. Only this
    ///      installation's own (by the record AND its description AND the
    ///      address and port noted) goes on, to step 6; but never one this run
    ///      of the server wrote and could not prove (a run given up while it
    ///      waited for its proof): that one is taken off, and the run goes on
    ///      as for a free 443. One the router keeps ends the run, which waits
    ///      kApartMs, as one that dialled;
    ///   3  the router leads back here from its outer address: a connection
    ///      to the block port there arrives at this host's listener. One that
    ///      does not cannot be checked from here;
    ///   4  nothing completes a handshake on 443 there: a rule made by hand
    ///      does not show in what step 2 asks;
    ///   5  the router is asked once more that it has no entry on 443 (the
    ///      dials took their time, and other requests were served meanwhile);
    ///      then the entry is noted in the record, written, read back, and a
    ///      connection to 443 must arrive here. Otherwise it is removed, the
    ///      removal read back, and the line struck.
    /// begin() makes steps 1 and 2: the run ends there, or asks for the dial
    /// of step 3. One run at a time: one that is under way is given up first.
    Step begin(const Situation& now);
    /// What the dial that was asked for found. The run goes on from there:
    /// to its next dial, or to its end. With no run under way nothing is
    /// done, and nothing is asked of the router.
    Step after(bool result);

    /// What giving a run up left behind.
    enum class Abandoned
    {
        /// Nothing was written yet (or no run was under way).
        Nothing,
        /// Its entry was written and waited for its proof: taken off, the
        /// removal read back, the line struck.
        Undone,
        /// The same entry, which the router keeps or does not show: the line
        /// stays, and the next start takes the entry back.
        Stays,
    };
    /// The run under way is given up, whatever its dial finds: after() is
    /// not to be called for it.
    Abandoned abandon();
    /// A dial was asked for, and after() has not been given its answer.
    bool underWay() const { return m_Stage != Stage::Idle; }
    /// ...and the run has written its entry, which waits for its proof.
    bool wrote() const { return m_Stage == Stage::Proof; }

    /// FOR THE TESTS ONLY: the same steps in a row, each dial made at once
    /// by `probe`, on the calling thread, which waits for each. Nothing of
    /// the server calls this: whoever runs the guard there (Keeper) holds no
    /// probe to give it, and has the dials made by an IDialler.
    Verdict claim(const Situation& now, IProbe& probe);

    /// Step 6, for an entry that is held: still this installation's as the
    /// router shows it, written again for its lease, read back. No dial. One
    /// that is another's by now, or not shown, is Lost, and not written again.
    /// One that no longer stands (the block port moved, the address changed,
    /// a switch went off) is taken off.
    Verdict renew(const Situation& now);

    /// The entry is taken off when the router shows it as this installation
    /// wrote it, and the removal is read back. True when nothing of this
    /// installation's is left on 443; false when it may be (the line stays,
    /// and the next start takes the entry back). A run under way is over by
    /// this: what it wrote is taken off as what is held is.
    bool release();

    bool held() const { return m_Held; }
    /// The block port 443 leads to while it is held, 0 otherwise.
    uint16_t leadsTo() const { return m_Held ? m_Internal : uint16_t{0}; }

    /// What became of an entry this installation wrote when this machine had
    /// another address on the LAN.
    enum class Former
    {
        /// No such line in the record.
        None,
        /// Still there as it was written: removed, and the line struck.
        Removed,
        /// Not there any more, or another's by now: the line is struck.
        Gone,
        /// Still there as it was written, and the router will not remove it.
        /// The line stays; formerRefused() says so until it is gone.
        Refused,
        /// The router does not say. The line stays.
        Unknown,
    };
    /// A router may keep an entry past its lease, and the address it leads to
    /// may be another machine's by now: unlike the other kinds, such a line
    /// is not left for later. It is this installation's by the record AND by
    /// its description, never by the address alone. Run by begin() before
    /// its step 2, and by whoever starts or renews.
    Former formerAddress();
    /// formerAddress() found such an entry and could not remove it: the
    /// person who runs the router has to. False again once the entry is seen
    /// to be gone; a router that says nothing leaves it as it was.
    bool formerRefused() const { return m_FormerRefused; }

private:
    /// The dial a run under way waits for: the way back (step 3), 443 in
    /// silence (step 4), or the proof of what was written (step 5).
    enum class Stage
    {
        Idle,
        WayBack,
        Silent,
        Proof,
    };

    QString text() const;
    bool ours(const IUpnpGateway::Entry& shown, const mw::routerholes::Hole& line) const;
    /// Step 5 up to its proof: asked again, noted, written, read back.
    Step write();
    /// What step 5 may have left on the router is taken back. True when
    /// nothing of this installation's is left there, and the line is struck.
    /// False: the line stays, and is the one written and never proved.
    bool undo(const mw::routerholes::Hole& line);
    /// `line` is the one this run of the server wrote and could not prove.
    bool unproved(const mw::routerholes::Hole& line) const;
    /// The record's line on 443 is struck: nothing of it is left to prove.
    void strike();

    IUpnpGateway& m_Gateway;
    mw::routerholes::IRecord* m_Record;
    bool m_Held = false;
    uint16_t m_Internal = 0;
    bool m_FormerRefused = false;
    Stage m_Stage = Stage::Idle;
    /// What the run under way was begun on.
    Situation m_Run;
    /// What its step 5 noted and wrote.
    mw::routerholes::Hole m_Line;
    /// A line step 5 wrote that the router kept when it was to be taken off,
    /// with no proof that 443 leads here: never taken as held. In memory
    /// only: the start takes back every line of an earlier run.
    mw::routerholes::Hole m_Unproved;
    bool m_HasUnproved = false;
};

/// What the admin page is told of Guard::formerRefused(), in the status it
/// reads: this one key, a yes or a no and nothing else (no address, no port,
/// nothing a router answered; the page has the words). For the admin alone:
/// whoever else asks gets a status without the key, whatever it held before.
inline constexpr const char* kStaleRefusedKey = "router443_stale_removal_refused";
void tellAdmin(QJsonObject& status, bool admin, bool staleRemovalRefused);

/// When the whole run (Guard::begin()) may be made again, from what the last
/// one said. A run asks the router some questions; from its step 3 on it also
/// dials an address that is not this host's to answer for, and from its step
/// 5 on it writes there. So:
///   - one that stopped before it dialled (steps 0 to 2: a switch, the
///     address, an entry somebody has) may be made again whenever the holes
///     are looked at;
///   - one that dialled and stopped (steps 3 and 4, and what step 5 asks the
///     router again before it writes) waits kApartMs: what it found does not
///     change by the minute. So does one stopped at step 2 by an entry this
///     run of the server wrote, could not prove, and cannot take off
///     (Step::apart): each run would try that removal again;
///   - one that wrote and did not keep what it wrote (step 5) is the last of
///     this run of the server: the router did something else than it was
///     asked, and is not asked that again until the server starts anew;
///   - an entry that was held and is lost may be asked for again at once,
///     once (a router that was restarted has forgotten it), and after
///     kApartMs if it is lost again within that time.
/// While the entry is held no run is due: it is renewed. A run that was given
/// up by this host's own doing (a switch, the port the streams share) is told
/// to nobody here: it does not count. One given up because the address
/// browsers reach this host at is another is not this host's doing, and is
/// paced as a lost entry is: a source that flips between two addresses would
/// otherwise begin a run, and from step 5 on a write and a removal on the
/// router, at every look.
class Pace
{
public:
    static constexpr uint64_t kApartMs = 60 * 60 * 1000;

    bool due(uint64_t nowMs) const noexcept { return !m_Over && !m_Held && nowMs >= m_NotBeforeMs; }
    /// What a run or renew() said, at `nowMs`: the time the run ended at.
    /// `dialled`: the run had a dial made before it ended.
    void said(const Verdict& verdict, uint64_t nowMs, bool dialled = false) noexcept;
    /// What the entry stood on changed by this host's own doing (a switch,
    /// the port the streams share): as at the start, but for what is over.
    void anew() noexcept
    {
        m_Held = false;
        m_NotBeforeMs = 0;
    }
    /// A run was given up at `nowMs` because the address browsers reach this
    /// host at changed. At once, once; given up so again within kApartMs, the
    /// next run waits kApartMs.
    void movedAway(uint64_t nowMs) noexcept;
    /// No run is made any more until the server starts anew.
    bool over() const noexcept { return m_Over; }

private:
    bool m_Held = false;
    bool m_Over = false;
    bool m_LostBefore = false;
    uint64_t m_LostMs = 0;
    bool m_MovedBefore = false;
    uint64_t m_MovedMs = 0;
    uint64_t m_NotBeforeMs = 0;
};

/// The guard as whoever has the router's thread runs it. It is looked at
/// again and again: when the holes are renewed, when the port 443 would lead
/// to gets its hole or loses it, when a switch changes. Each time it does the
/// one thing that is due: it renews what is held; takes it off when what it
/// stood on is no more; begins a whole run when one is due (Pace); and, held
/// or not, looks for an entry this installation left under a former address.
/// What there is to say comes back, for the log: never an address.
///
/// A run does not hold that thread: what it asks the router is done within a
/// look or within dialled(), and its dials are made by the dialler, on a
/// thread of its own, while every other request is served. At any time there
/// is one run at most, and one dial at most with the dialler; an answer moves
/// the run it was asked for and no other. Everything here, and everything
/// that is written on the router, is the keeper's thread's alone: the dialler
/// is given a dial, and gives back a number and a yes or a no.
class Keeper
{
public:
    struct Line
    {
        bool warning = false;
        QString text;
    };

    /// `routerAddress` asks the router for its own outer address: called only
    /// when a run or a renewal is about to be made. `dialler` makes the dials
    /// and has their answers told to dialled(), on this keeper's thread.
    Keeper(IUpnpGateway& gateway, mw::routerholes::IRecord* record, IDialler& dialler,
           std::function<QString()> routerAddress);

    /// `now` as the caller knows it: the switches, the address browsers reach
    /// this host at, the block port the router forwards in TCP (0 for none)
    /// and the lease; its routerAddress is filled in here. `ask`: the router
    /// may be asked (UPnP and Internet Access are on). Without, an entry that
    /// is held is taken back, as every hole is, and nothing else is done.
    ///
    /// With a run under way: if what it was begun on still stands, nothing
    /// is done at all (one run at a time, and it began with everything a
    /// look asks). If not (a switch, the shared port, the address), the run
    /// is given up, what it wrote is taken off, and the look goes on with
    /// what stands now: at once, unless the address changed twice within
    /// kApartMs (Pace::movedAway()).
    QList<Line> look(Situation now, bool ask, uint64_t nowMs);

    /// What the dial asked under `run` found, at `nowMs`. One that is not
    /// the dial the dialler has is nothing. One of a run that was given up
    /// is dropped before anything else: the run it belonged to is no more,
    /// and nothing is paced or said by it; it only frees the dialler for the
    /// dial of the run under way, if one waits. Otherwise the run goes on:
    /// to its next dial, or to its end, said here and paced from `nowMs`.
    QList<Line> dialled(uint32_t run, bool result, uint64_t nowMs);

    /// The block port 443 leads to while it is held, 0 otherwise.
    uint16_t leadsTo() const { return m_Guard.leadsTo(); }
    bool formerRefused() const { return m_Guard.formerRefused(); }
    /// A run is under way: begun, and not ended or given up.
    bool running() const { return m_Run != 0; }
    /// The server stops. A run under way is given up and its dial cancelled;
    /// the entry, held or just written by that run, is taken off and the
    /// removal read back. Said only when the router keeps it: a warning.
    /// With nothing held and nothing written the router is asked nothing.
    QList<Line> release();

private:
    /// The run under way is no more: its number says nothing from here on,
    /// and its dial, if the dialler has it, is cancelled.
    void retire();
    /// ...with what it wrote taken off: by this host's own doing, or because
    /// the address browsers reach it at is another (`moved`, at `nowMs`).
    void giveUp(QList<Line>& out, bool moved, uint64_t nowMs);
    /// After begin() or after(): the next dial to have made, or the end.
    void goOn(const Step& step, uint64_t nowMs, bool dialled, QList<Line>& out);
    /// The dial that waits is given to the dialler, once it has none.
    void handOver(QList<Line>& out);
    void say(const Verdict& verdict, uint64_t nowMs, bool dialled, QList<Line>& out);

    Guard m_Guard;
    IDialler& m_Dialler;
    std::function<QString()> m_RouterAddress;
    Pace m_Pace;
    /// The reason last given for leaving 443 as it is: said once, not at
    /// every look
    QString m_Left;
    bool m_FormerSaid = false;
    /// The run under way: its number, 0 for none. Never the same twice.
    uint32_t m_Run = 0;
    uint32_t m_Runs = 0;
    /// What it was begun on: the switches, the shared port, the address.
    Situation m_Stands;
    /// Its dial that waits for the dialler to be free.
    Dial m_Next;
    /// The run whose dial the dialler has, answered or not wanted any more;
    /// 0 when it has none.
    uint32_t m_Out = 0;
    /// The dialler would not take a dial: no run is begun any more.
    bool m_Unable = false;
};

/// The look that waits while something has to be done on the router first
/// (what an earlier run left there is taken back before anything is asked
/// for on 443). Only the latest one waits: each that is asked for meanwhile
/// replaces the one before it, and when the wait is over that one is made,
/// once. So a look never comes after one that was asked for later, and a
/// switch that went off meanwhile is the last word.
class Waiting
{
public:
    void put(const Situation& now, bool ask)
    {
        m_Now = now;
        m_Ask = ask;
        m_Waits = true;
    }
    /// The look that waited, if one did; none waits after that.
    bool take(Situation& now, bool& ask)
    {
        if (!m_Waits) return false;
        now = m_Now;
        ask = m_Ask;
        m_Waits = false;
        return true;
    }
    bool waits() const { return m_Waits; }

private:
    Situation m_Now;
    bool m_Ask = false;
    bool m_Waits = false;
};

} // namespace mw::tcp443
