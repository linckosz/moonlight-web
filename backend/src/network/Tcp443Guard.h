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

/// What the guard learns by dialling, which no router can be asked.
class IProbe
{
public:
    virtual ~IProbe() = default;

    /// Dials `address:port` and sends one frame, which names a fragment made
    /// for this one try: true when that very frame came in at this host's own
    /// TCP listener on the shared ports, within the time a try has.
    virtual bool arrivesHere(const QString& address, uint16_t port) = 0;
    /// Dials `address:port` and sends nothing: true when something there
    /// completes the handshake within that time.
    virtual bool answers(const QString& address, uint16_t port) = 0;
    /// The shared port a connection that leads here comes in on, told before
    /// a run: the block port, which its step 3 dials and to which 443 leads
    /// at its step 5.
    virtual void leadsHereOn(uint16_t port) { (void)port; }
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

class Guard
{
public:
    /// `record` null: a run that keeps no record, and so never asks for 443.
    Guard(IUpnpGateway& gateway, mw::routerholes::IRecord* record, IProbe& probe);

    /// The whole run. In this order, and the first step that fails ends it:
    ///   1  the address browsers reach is a public IPv4 one, and the router's
    ///      own: no second router in front of it;
    ///   2  the router says it has no TCP entry on 443. An entry, whoever it
    ///      leads to, or an answer that says nothing, ends the run. Only this
    ///      installation's own (by the record AND its description AND the
    ///      address and port noted) goes on, to step 6;
    ///   3  the router leads back here from its outer address: a connection
    ///      to the block port there arrives at this host's listener. One that
    ///      does not cannot be checked from here;
    ///   4  nothing completes a handshake on 443 there: a rule made by hand
    ///      does not show in what step 2 asks;
    ///   5  the entry is noted in the record, written, read back, and a
    ///      connection to 443 must then arrive here. Otherwise it is removed,
    ///      the removal read back, and the line struck.
    Verdict claim(const Situation& now);

    /// Step 6, for an entry that is held: still this installation's as the
    /// router shows it, written again for its lease, read back. No dial. One
    /// that is another's by now, or not shown, is Lost, and not written again.
    /// One that no longer stands (the block port moved, the address changed,
    /// a switch went off) is taken off.
    Verdict renew(const Situation& now);

    /// The entry is taken off when the router shows it as this installation
    /// wrote it, and the removal is read back. True when nothing of this
    /// installation's is left on 443; false when it may be (the line stays,
    /// and the next start takes the entry back).
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
    /// its description, never by the address alone. Run by claim() before
    /// its step 2, and by whoever starts or renews.
    Former formerAddress();
    /// formerAddress() found such an entry and could not remove it: the
    /// person who runs the router has to. False again once the entry is seen
    /// to be gone; a router that says nothing leaves it as it was.
    bool formerRefused() const { return m_FormerRefused; }

private:
    QString text() const;
    bool ours(const IUpnpGateway::Entry& shown, const mw::routerholes::Hole& line) const;
    /// What step 5 may have left on the router is taken back.
    void undo(const mw::routerholes::Hole& line);

    IUpnpGateway& m_Gateway;
    mw::routerholes::IRecord* m_Record;
    IProbe& m_Probe;
    bool m_Held = false;
    uint16_t m_Internal = 0;
    bool m_FormerRefused = false;
};

/// What the admin page is told of Guard::formerRefused(), in the status it
/// reads: this one key, a yes or a no and nothing else (no address, no port,
/// nothing a router answered; the page has the words). For the admin alone:
/// whoever else asks gets a status without the key, whatever it held before.
inline constexpr const char* kStaleRefusedKey = "router443_stale_removal_refused";
void tellAdmin(QJsonObject& status, bool admin, bool staleRemovalRefused);

/// When the whole run (Guard::claim()) may be made again, from what the last
/// one said. A run asks the router some questions; from its step 3 on it also
/// dials an address that is not this host's to answer for, and from its step
/// 5 on it writes there. So:
///   - one that stopped before it dialled (steps 0 to 2: a switch, the
///     address, an entry somebody has) may be made again whenever the holes
///     are looked at;
///   - one that dialled and stopped (steps 3 and 4) waits kApartMs: what it
///     found does not change by the minute;
///   - one that wrote and did not keep what it wrote (step 5) is the last of
///     this run of the server: the router did something else than it was
///     asked, and is not asked that again until the server starts anew;
///   - an entry that was held and is lost may be asked for again at once,
///     once (a router that was restarted has forgotten it), and after
///     kApartMs if it is lost again within that time.
/// While the entry is held no run is due: it is renewed.
class Pace
{
public:
    static constexpr uint64_t kApartMs = 60 * 60 * 1000;

    bool due(uint64_t nowMs) const noexcept { return !m_Over && !m_Held && nowMs >= m_NotBeforeMs; }
    /// What claim() or renew() said, at `nowMs`.
    void said(const Verdict& verdict, uint64_t nowMs) noexcept;
    /// What the entry stood on changed by this host's own doing (a switch,
    /// the port the streams share): as at the start, but for what is over.
    void anew() noexcept
    {
        m_Held = false;
        m_NotBeforeMs = 0;
    }
    /// No run is made any more until the server starts anew.
    bool over() const noexcept { return m_Over; }

private:
    bool m_Held = false;
    bool m_Over = false;
    bool m_LostBefore = false;
    uint64_t m_LostMs = 0;
    uint64_t m_NotBeforeMs = 0;
};

/// The guard as whoever has the router's thread runs it. It is looked at
/// again and again: when the holes are renewed, when the port 443 would lead
/// to gets its hole or loses it, when a switch changes. Each time it does the
/// one thing that is due: it renews what is held; takes it off when what it
/// stood on is no more; makes a whole run when one is due (Pace); and, held
/// or not, looks for an entry this installation left under a former address.
/// What there is to say comes back, for the log: never an address.
class Keeper
{
public:
    struct Line
    {
        bool warning = false;
        QString text;
    };

    /// `routerAddress` asks the router for its own outer address: called only
    /// when a run or a renewal is about to be made.
    Keeper(IUpnpGateway& gateway, mw::routerholes::IRecord* record, IProbe& probe,
           std::function<QString()> routerAddress);

    /// `now` as the caller knows it: the switches, the address browsers reach
    /// this host at, the block port the router forwards in TCP (0 for none)
    /// and the lease; its routerAddress is filled in here. `ask`: the router
    /// may be asked (UPnP and Internet Access are on). Without, an entry that
    /// is held is taken back, as every hole is, and nothing else is done.
    QList<Line> look(Situation now, bool ask, uint64_t nowMs);

    /// The block port 443 leads to while it is held, 0 otherwise.
    uint16_t leadsTo() const { return m_Guard.leadsTo(); }
    bool formerRefused() const { return m_Guard.formerRefused(); }
    /// The entry is taken off, as when the server stops.
    bool release() { return m_Guard.release(); }

private:
    Guard m_Guard;
    IProbe& m_Probe;
    std::function<QString()> m_RouterAddress;
    Pace m_Pace;
    /// The reason last given for leaving 443 as it is: said once, not at
    /// every look
    QString m_Left;
    bool m_FormerSaid = false;
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
