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

#include "IceMux.h"
#include "RouterPortCore.h"

#include <QList>
#include <QString>

#include <array>
#include <cstdint>
#include <functional>

/// Which ports the streams share, and which of them the router forwards.
///
/// IceMux holds sockets and reads them; RouterPortAllocator asks the router
/// for holes. This sits between the two and decides, for each of the mux's
/// two ports (IceMux::kStandard, IceMux::kBlock):
///
///   the block port is bound when the server starts, router or not: a stream
///   on this LAN needs it at once, and it is the port a manual forward points
///   at. Its hole is asked for when the router answers;
///
///   the conferencing port is for browsers on the Internet, so only a host
///   that is open to them wants one. Where the router can be asked, the port
///   is whichever number the router lets this host have, bound once the
///   router has nothing against it. Where it cannot (UPnP off, no gateway),
///   the first number free on this machine is bound all the same: a manual
///   forward, or the router's own translation, may still lead to it;
///
///   a hole the router gave to another machine is asked for again, on the
///   next number. The port that lost it stays bound and read, for the streams
///   that are on it, and stops being handed to new ones once another serves;
///   the mux lets it go when the last of those streams' workers is gone.
///
/// Everything here runs on the main thread, except the two functions given to
/// the router with a claim, which only reach the mux. docs/design/ice-mux.md
class IceMuxPorts
{
public:
    using Hold = RouterPortCore::Hold;

    /// The router's allocator, as far as the shared ports need it:
    /// RouterPortAllocator in the server (IceMuxRouter.h), a script in the
    /// tests.
    class Router
    {
    public:
        virtual ~Router() = default;

        /// Whether holes may be asked for at all (UPnP on, Internet Access on).
        virtual bool enabled() const = 0;
        /// Start looking for the gateway, if that has not begun. Only called
        /// when enabled().
        virtual void look() = 0;
        enum class Gateway
        {
            /// Not looked for yet, or being looked for: its answer comes later
            Looking,
            Ready,
            Missing,
        };
        virtual Gateway gateway() const = 0;
        /// The port whose hole is held for that role, 0 when none is.
        virtual uint16_t hole(int which) const = 0;
        /// Whether the router forwards that port in TCP too.
        virtual bool holeTcp(int which) const = 0;
        /// This host's address as the router reports it; empty when it does
        /// not, or before it was found.
        virtual QString publicIp() const = 0;
        /// Ask for the hole (RouterPortAllocator::claimMuxPort). `done` gets
        /// the port the router forwards here, or 0 and why not, on the main
        /// thread and after this call returns.
        virtual void claim(int which, uint16_t current, std::function<Hold(uint16_t)> hold,
                           std::function<void(uint16_t)> drop,
                           std::function<void(uint16_t port, const QString& why)> done) = 0;
        /// Take the hole off the router.
        virtual void release(int which) = 0;
    };

    struct Policy
    {
        /// The numbers a port may take, in the order to try them when the
        /// router is not asked.
        std::function<QList<uint16_t>(int which)> candidates;
        /// Whether this host wants a conferencing port: not a DEV identity,
        /// and open to the Internet.
        std::function<bool()> wantsStandard;
    };

    IceMuxPorts(IceMux& mux, Router& router, Policy policy);

    /// Starts the mux and binds its block port; the rest follows as the
    /// router answers. False, the mux stopped, when no port of the block can
    /// be bound: every stream then binds its own, as without the mux.
    bool start(QString* why);
    /// Whether the streams share ports in this run: start() succeeded.
    bool inUse() const { return m_InUse; }

    /// The gateway was found, or was not, or the switches came back on.
    void routerChanged();
    /// The router now forwards a port of ours to another machine: the
    /// allocator has dropped the hole already.
    void holeLost(int which, uint16_t port);
    /// UPnP or Internet Access was switched off.
    void switchedOff();

    /// What a stream on the shared ports may announce at the public address.
    struct Announced
    {
        QString publicIp;
        /// The ports that serve AND that the router forwards here, the
        /// preferred first. A port whose hole was never made, or went to
        /// another machine, is not among them: nothing at the public address
        /// leads to it.
        QList<uint16_t> ports;
        /// Of those, the ones the router forwards in TCP too.
        QList<uint16_t> tcpPorts;
    };
    /// As of now: asked when a stream starts. No port when the router
    /// forwards none, or its public address is not known.
    Announced announced() const;

private:
    bool wanted(int which) const;
    void ensure(int which);
    void claim(int which);
    void claimed(int which, uint16_t port, const QString& why);
    void bindHere(int which);

    IceMux& m_Mux;
    Router& m_Router;
    Policy m_Policy;
    bool m_InUse = false;
    /// A claim is with the router: one at a time for each port
    std::array<bool, IceMux::kRoles> m_Claiming{};
};
