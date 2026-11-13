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

#include <QString>

#include <cstdint>

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

} // namespace mw::tcp443
