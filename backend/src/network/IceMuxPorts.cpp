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

#include "IceMuxPorts.h"

#include <QDebug>

#include <string>

namespace {

const char* roleName(int which)
{
    return which == IceMux::kStandard ? "conferencing" : "stream block";
}

/// Ends, when it goes, an asking that IceMux::beginAsking() began.
class Asked
{
public:
    explicit Asked(IceMux& mux)
        : m_Mux(mux)
    {}
    ~Asked() { m_Mux.endAsking(); }
    Asked(const Asked&) = delete;
    Asked& operator=(const Asked&) = delete;

private:
    IceMux& m_Mux;
};

} // namespace

IceMuxPorts::IceMuxPorts(IceMux& mux, Router& router, Policy policy)
    : m_Mux(mux)
    , m_Router(router)
    , m_Policy(std::move(policy))
{}

bool IceMuxPorts::start(QString* why)
{
    if (m_InUse) return true;
    std::string reason;
    if (!m_Mux.start(&reason)) {
        if (why) *why = QString::fromStdString(reason);
        return false;
    }
    bindHere(IceMux::kBlock);
    if (m_Mux.port(IceMux::kBlock) == 0) {
        m_Mux.stop();
        if (why) *why = QStringLiteral("no port of the stream block can be bound");
        return false;
    }
    m_InUse = true;
    routerChanged();
    return true;
}

bool IceMuxPorts::wanted(int which) const
{
    if (which == IceMux::kBlock) return true;
    return which == IceMux::kStandard && m_Policy.wantsStandard && m_Policy.wantsStandard();
}

void IceMuxPorts::routerChanged()
{
    for (int which = 0; which < IceMux::kRoles; ++which)
        ensure(which);
}

void IceMuxPorts::ensure(int which)
{
    if (!m_InUse || !wanted(which) || m_Claiming[static_cast<size_t>(which)]) return;
    if (m_Router.enabled()) {
        m_Router.look();
        switch (m_Router.gateway()) {
        case Router::Gateway::Looking:
            // Its answer comes back here, found or not.
            return;
        case Router::Gateway::Ready:
            if (m_Router.hole(which) == 0) claim(which);
            return;
        case Router::Gateway::Missing: break;
        }
    }
    // No router to ask: the port is bound all the same.
    if (m_Mux.port(which) == 0) bindHere(which);
}

void IceMuxPorts::claim(int which)
{
    m_Claiming[static_cast<size_t>(which)] = true;
    // Until claimed(): the answer may be a port the mux has replaced, which
    // must not be let go between the router's look at it and its serving.
    m_Mux.beginAsking();
    // These two run on the router's thread, and may run after this object is
    // gone: they know the mux and nothing else. The mux outlives that thread.
    IceMux* mux = &m_Mux;
    m_Router.claim(
        which, m_Mux.port(which),
        [mux](uint16_t port) {
            switch (mux->hold(port)) {
            case IceMux::Held::NewlyBound: return Hold::NewlyBound;
            case IceMux::Held::AlreadyHeld: return Hold::AlreadyHeld;
            case IceMux::Held::Failed: break;
            }
            return Hold::Failed;
        },
        [mux](uint16_t port) { mux->drop(port); },
        [this, which](uint16_t port, const QString& why) { claimed(which, port, why); });
}

void IceMuxPorts::claimed(int which, uint16_t port, const QString& why)
{
    m_Claiming[static_cast<size_t>(which)] = false;
    // The asking that claim() began ends when this returns, whatever it does
    // with the answer (a port asked for again from here has its own).
    const Asked asked(m_Mux);
    if (port == 0) {
        qInfo().noquote() << QStringLiteral("[IceMux] no router hole for the %1 port — %2")
                                 .arg(QLatin1String(roleName(which)), why);
        // The router went, or was switched off, while it was asked: the port
        // is bound without it. A router that is there and has no number left
        // is not asked again until it changes: a host that finds the
        // conferencing ports all taken streams on its block port alone.
        if (!m_Router.enabled() || m_Router.gateway() == Router::Gateway::Missing) ensure(which);
        return;
    }

    const uint16_t before = m_Mux.port(which);
    if (!m_Router.enabled() || !wanted(which)) {
        // Switched off while the router was answering: the hole goes back.
        // The port serves if the role had none and still wants one (UPnP off
        // alone leaves a host open to the Internet), and is let go otherwise
        // (a port that has served is kept, whatever is asked here).
        m_Router.release(which);
        if (before == 0 && wanted(which))
            m_Mux.serve(which, port);
        else
            m_Mux.drop(port);
        return;
    }
    if (!m_Mux.serve(which, port)) {
        // The hole would lead to a port no stream is given.
        qWarning().noquote() << QStringLiteral("[IceMux] port %1 has a router hole and cannot "
                                               "serve as the %2 port: the hole is given back")
                                    .arg(port)
                                    .arg(QLatin1String(roleName(which)));
        m_Router.release(which);
        m_Mux.drop(port);
        return;
    }
    if (before != 0 && before != port)
        qInfo().noquote() << QStringLiteral("[IceMux] the %1 port moves from %2 to %3 — streams "
                                            "already on %2 keep it")
                                 .arg(QLatin1String(roleName(which)))
                                 .arg(before)
                                 .arg(port);
    qInfo().noquote() << QStringLiteral("[IceMux] streams share UDP port %1 (%2), which the "
                                        "router forwards here")
                             .arg(port)
                             .arg(QLatin1String(roleName(which)));
}

void IceMuxPorts::bindHere(int which)
{
    m_Mux.beginAsking();
    const Asked asked(m_Mux);
    std::string reason;
    const QList<uint16_t> ports =
        m_Policy.candidates ? m_Policy.candidates(which) : QList<uint16_t>();
    for (const uint16_t port : ports) {
        const IceMux::Held held = m_Mux.hold(port, &reason);
        if (held == IceMux::Held::Failed) continue;
        if (!m_Mux.serve(which, port)) {
            if (held == IceMux::Held::NewlyBound) m_Mux.drop(port);
            continue;
        }
        qInfo().noquote() << QStringLiteral("[IceMux] streams share UDP port %1 (%2)")
                                 .arg(port)
                                 .arg(QLatin1String(roleName(which)));
        return;
    }
    qWarning().noquote() << QStringLiteral("[IceMux] no %1 port can be bound: %2")
                                .arg(QLatin1String(roleName(which)),
                                     QString::fromStdString(reason));
}

void IceMuxPorts::holeLost(int which, uint16_t port)
{
    if (!m_InUse || which < 0 || which >= IceMux::kRoles) return;
    qWarning().noquote() << QStringLiteral("[IceMux] the router no longer forwards port %1 here: "
                                           "asking for another %2 port")
                                .arg(port)
                                .arg(QLatin1String(roleName(which)));
    ensure(which);
}

void IceMuxPorts::switchedOff()
{
    if (!m_InUse) return;
    for (int which = 0; which < IceMux::kRoles; ++which)
        if (m_Router.hole(which) != 0) m_Router.release(which);
    // A port the router was still being asked for is bound without it.
    routerChanged();
}

IceMuxPorts::Announced IceMuxPorts::announced() const
{
    Announced out;
    if (!m_InUse) return out;
    out.publicIp = m_Router.publicIp();
    if (out.publicIp.isEmpty()) return out;
    for (int which = 0; which < IceMux::kRoles; ++which) {
        // The hole and the port that serves are one number, or the hole is
        // not this port's: nothing is announced on a guess.
        const uint16_t port = m_Router.hole(which);
        if (port != 0 && port == m_Mux.port(which)) out.ports.append(port);
    }
    return out;
}
