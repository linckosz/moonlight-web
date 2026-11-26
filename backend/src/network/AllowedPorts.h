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

#include <QList>
#include <QSet>
#include <QString>

#include <cstdint>

class QJsonObject;
class QJsonValue;

/// The ports the router may forward to this host, when its owner says so
/// (settings.json router_ports_allowed, issue #82): some providers share one
/// IPv4 address between customers and let each forward only a few external
/// ports. Without the key, every hole is drawn from the lists of
/// RouterPortPools.h, as ever. With it, every hole this host asks a router for
/// (the shared ports of the streams, the tunnel's, a stream slot's own) is
/// drawn from the list alone, in its order, and TCP 443 never is. A hole the
/// router forwards to itself (the shared ports, the tunnel) binds the very
/// number, so a list forwarded by hand 1:1 works as well as one UPnP writes.
///
///   "router_ports_allowed": [11756, "11760-11775"]
///
/// Read once, as the server starts, and strictly: whole numbers, or strings
/// "a-b" with a <= b, nothing else; 64 items and 4096 ports at most; no port
/// twice; none below 1024, none of the tunnel's STUN ports (3478-3481), none of
/// the streaming servers' (47984-48010), none of a DEV identity's, none this
/// server listens on, and not the port the sweep asks about first. A list that
/// breaks any of these is refused whole, said once with the rule it breaks,
/// and nothing is written to the router until it is mended: what this host
/// wrote there before is still taken off. Never mended here.
namespace mw::routerports {

struct PortRange
{
    uint16_t first = 0;
    uint16_t last = 0;
};

/// A stretch the list may not touch, and what it is, for the warning.
struct Forbidden
{
    uint16_t first = 0;
    uint16_t last = 0;
    QString what;
};

struct AllowedPorts
{
    enum class State
    {
        /// No key: the lists of RouterPortPools.h.
        Absent,
        /// The list, as written.
        Valid,
        /// Refused whole: nothing is asked of the router.
        Invalid,
    };
    State state = State::Absent;
    QList<PortRange> ranges;
    /// Why it was refused: the rule, and the value that breaks it.
    QString why;

    bool defined() const { return state != State::Absent; }
    bool valid() const { return state == State::Valid; }
    bool contains(uint16_t port) const;
    /// Every port of the list, in its order.
    QList<uint16_t> ports() const;
};

inline constexpr int kAllowedItemsMax = 64;
inline constexpr int kAllowedPortsMax = 4096;
inline constexpr const char* kAllowedKey = "router_ports_allowed";

/// The list `stored` holds (undefined when the key is absent), against the
/// stretches it may not touch beyond the fixed ones (this server's own ports:
/// fixedForbidden() has the others).
AllowedPorts parseAllowedPorts(const QJsonValue& stored, const QList<Forbidden>& local);

/// What no list may touch, whatever the server: ports below 1024, the tunnel's
/// STUN ports, the streaming servers', a DEV identity's, the port the sweep
/// asks about first.
QList<Forbidden> fixedForbidden();

// ── This process ─────────────────────────────────────────────────────────────

/// Settled once, as the server starts, before anything asks a router.
/// From any thread. A list in force that is replaced leaves its numbers to
/// take back (removableByList()).
void setAllowedPorts(const AllowedPorts& allowed);
AllowedPorts allowedPorts();

/// Numbers this host may have written under an earlier list: the ones the
/// settings still remember (router_ports), read as the server starts. With
/// the list in force, a record's line on one of them is this host's to take
/// back (RouterHoles' allowed()); never one to ask for.
void setRememberedForRemoval(const QSet<uint16_t>& remembered);
/// Whether a line of the record on `port` may be taken back although it is
/// not one of RouterPortPools.h's: of the list in force, or remembered. Never
/// below 1024.
bool removableByList(uint16_t port);

/// What the admin page is told of a list that was refused: this one key, a
/// yes or a no (the page has the words; the rule it breaks is in the log).
/// For the admin alone: whoever else asks gets a status without the key.
inline constexpr const char* kAllowedRefusedKey = "router_ports_allowed_refused";
void tellAdmin(QJsonObject& status, bool admin);

} // namespace mw::routerports
