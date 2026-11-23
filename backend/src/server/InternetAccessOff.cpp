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

#include "server/InternetAccessOff.h"

#include "common/Logger.h"
#include "server/AppSettings.h"

#include <QObject>
#include <QTimer>

namespace mw::internetoff {

void turnOff(const Steps& steps, QObject* context, const QString& why)
{
    // The switch first: the tunnel's gate reads it, so from here no browser is
    // taken, even one the line brings before the step below.
    if (steps.writeOff) steps.writeOff();
    if (steps.stopAccess) steps.stopAccess();
    Logger::info(QStringLiteral("[InternetAccess] Turned off (%1): the browser tunnels, the "
                                "router's holes and the line go next")
                     .arg(why));

    // After the answer: a page that asked through a tunnel is sent it before
    // its tunnel goes. Together, in this order: a peer's hole goes back to the
    // router as the peer goes, then the allocator lets the rest go, then the line.
    // Turned back on in between: the peers taken before the off still go, the
    // allocator reads the switch itself, and the line stays up.
    QTimer::singleShot(0, context, [steps]() {
        if (steps.dropTunnels) steps.dropTunnels(QStringLiteral("Internet Access turned off"));
        if (steps.switchChanged) steps.switchChanged();
        if (steps.stillOff && !steps.stillOff()) {
            Logger::info(
                QStringLiteral("[InternetAccess] Turned back on before the line went: line kept"));
            return;
        }
        if (steps.lineOff) steps.lineOff();
    });
}

bool tunnelAdmits(const AppSettings& settings)
{
    return settings.internetAccessEnabled();
}

} // namespace mw::internetoff
