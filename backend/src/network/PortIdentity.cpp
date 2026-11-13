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

#include "PortIdentity.h"

#include "RouterPortPools.h"
#include "common/Edition.h"

#include <atomic>

namespace mw::portidentity {

namespace {

// -1 until settle(): the identity alone decides.
std::atomic<int> g_DevPorts{-1};

} // namespace

Verdict benchVerdict(bool devIdentity, bool key, const Listens& listens)
{
    Verdict verdict;
    if (!key) return verdict;
    if (!devIdentity) {
        verdict.bench = Bench::NotDev;
        return verdict;
    }
    if (!listens) {
        verdict.bench = Bench::Unknown;
        return verdict;
    }
    bool unknown = false;
    for (const uint16_t port : mw::routerports::kTunnelPreferred) {
        for (const bool tcp : {false, true}) {
            switch (listens(port, tcp)) {
            case mw::sockettables::Listens::No: break;
            case mw::sockettables::Listens::Yes:
                verdict.bench = Bench::Held;
                verdict.port = port;
                verdict.tcp = tcp;
                return verdict;
            case mw::sockettables::Listens::Unknown: unknown = true; break;
            }
        }
    }
    verdict.bench = unknown ? Bench::Unknown : Bench::Granted;
    return verdict;
}

QString said(const Verdict& verdict)
{
    const QString refused =
        QStringLiteral("DEBUG debug_dev_standard_ports is set and REFUSED: %1. This instance keeps "
                       "a DEV identity's ports");
    switch (verdict.bench) {
    case Bench::Off: return QString();
    case Bench::NotDev:
        return QStringLiteral("debug_dev_standard_ports is set and changes nothing: this is not a "
                              "DEV instance");
    case Bench::Held:
        return refused.arg(QStringLiteral("%1 port %2 is held on this machine")
                               .arg(verdict.tcp ? QStringLiteral("TCP") : QStringLiteral("UDP"))
                               .arg(verdict.port));
    case Bench::Unknown:
        return refused.arg(
            QStringLiteral("the system would not say what holds the conferencing ports"));
    case Bench::Granted:
        return QStringLiteral("DEBUG debug_dev_standard_ports is ON — this DEV instance walks the "
                              "installed host's ports for this run (the conferencing ports and its "
                              "half of the stream block). A bench key, never for a user");
    }
    return QString();
}

Verdict settle(bool key)
{
    const bool dev = mw::edition::isDev();
    const Verdict verdict = benchVerdict(
        dev, key, [](uint16_t port, bool tcp) { return mw::sockettables::listens(port, tcp); });
    g_DevPorts.store(devPortsFor(dev, verdict.bench) ? 1 : 0);
    return verdict;
}

bool devPorts()
{
    const int settled = g_DevPorts.load();
    return settled < 0 ? mw::edition::isDev() : settled == 1;
}

} // namespace mw::portidentity
