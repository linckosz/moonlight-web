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

#include "IceMuxPorts.h"
#include "RouterPortAllocator.h"

#include <QObject>

#include <utility>

/// RouterPortAllocator, as the streams' shared ports ask of the router
/// (IceMuxPorts::Router), and the allocator's signals brought to them.
///
/// Declared after the allocator and before the IceMuxPorts it serves, so that
/// it goes between the two: what the allocator still has to answer then
/// reaches nobody.
class IceMuxRouter : public IceMuxPorts::Router
{
public:
    explicit IceMuxRouter(RouterPortAllocator* allocator)
        : m_Allocator(allocator)
    {}

    bool enabled() const override { return m_Allocator->enabled(); }
    void look() override { m_Allocator->ensureStarted(); }

    Gateway gateway() const override
    {
        switch (m_Allocator->gatewayState()) {
        case RouterPortAllocator::GatewayState::Ready: return Gateway::Ready;
        case RouterPortAllocator::GatewayState::Missing: return Gateway::Missing;
        case RouterPortAllocator::GatewayState::Unknown:
        case RouterPortAllocator::GatewayState::Discovering: break;
        }
        return Gateway::Looking;
    }

    bool holds(int which) const override { return m_Allocator->heldMuxPort(which).has_value(); }

    void claim(int which, uint16_t current, std::function<IceMuxPorts::Hold(uint16_t)> hold,
               std::function<void(uint16_t)> drop,
               std::function<void(uint16_t port, const QString& why)> done) override
    {
        m_Allocator->claimMuxPort(
            which, current, std::move(hold), std::move(drop), &m_Context,
            [done = std::move(done)](const RouterPortAllocator::Claim& claim, const QString& why) {
                done(claim.external, why);
            });
    }

    void release(int which) override { m_Allocator->releaseMuxPort(which); }

    /// What the allocator learns of the router from now on reaches `ports`.
    void attach(IceMuxPorts* ports)
    {
        QObject::connect(m_Allocator, &RouterPortAllocator::gatewayReady, &m_Context,
                         [ports]() { ports->routerChanged(); });
        QObject::connect(m_Allocator, &RouterPortAllocator::gatewayMissing, &m_Context,
                         [ports]() { ports->routerChanged(); });
        QObject::connect(m_Allocator, &RouterPortAllocator::switchedOn, &m_Context,
                         [ports]() { ports->routerChanged(); });
        QObject::connect(m_Allocator, &RouterPortAllocator::switchedOff, &m_Context,
                         [ports]() { ports->switchedOff(); });
        QObject::connect(m_Allocator, &RouterPortAllocator::portLost, &m_Context,
                         [ports](int purpose, int slot, quint16 port, const QString&) {
                             if (purpose == static_cast<int>(RouterPortAllocator::Purpose::Mux))
                                 ports->holeLost(slot, port);
                         });
    }

private:
    RouterPortAllocator* m_Allocator;
    /// What the claims in flight and the connections above end with
    QObject m_Context;
};
