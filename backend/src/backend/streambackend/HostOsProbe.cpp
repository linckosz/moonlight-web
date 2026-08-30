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

#include "backend/streambackend/HostOsProbe.h"

namespace HostOsProbe {

QString toString(HostOs os)
{
    switch (os) {
    case HostOs::Windows: return QStringLiteral("windows");
    case HostOs::Linux: return QStringLiteral("linux");
    case HostOs::MacOs: return QStringLiteral("macos");
    case HostOs::Unknown: break;
    }
    return QStringLiteral("unknown");
}

HostOs fromString(const QString& s)
{
    if (s == QLatin1String("windows")) return HostOs::Windows;
    if (s == QLatin1String("linux")) return HostOs::Linux;
    if (s == QLatin1String("macos")) return HostOs::MacOs;
    return HostOs::Unknown;
}

HostOs fromInitialTtl(int observedTtl)
{
    // Above 64 and no higher than a Windows stack's start: every hop on the way
    // only lowers it, so nothing that began at 64 can land here.
    if (observedTtl > 64 && observedTtl <= 128) return HostOs::Windows;

    // At or below 64 the packet began at 64 — Linux or macOS, which want
    // opposite handling, so this is where the fingerprint stops being useful.
    // Above 128 means a 255-based stack, which no GameStream host we know of
    // is. Both are honest Unknowns rather than a coin toss.
    return HostOs::Unknown;
}

HostOs infer(const OsEvidence& ev)
{
    // Certainty first: this host IS us, so its OS is the one we were built for.
    if (ev.isLocalMachine) return thisMachine();

    // A backend type is only ever set by an API that identified itself, so it
    // carries the platform that backend ships on. Wolf is Linux containers;
    // MultiSeat drives Windows sessions and has no other port.
    if (ev.backendType == QLatin1String("wolf")) return HostOs::Linux;
    if (ev.backendType == QLatin1String("multiseat")) return HostOs::Windows;

    // Even without MultiSeat being set up here, its control API answering at
    // all means the machine is running it — and it only runs on Windows.
    if (ev.multiSeatApiPresent) return HostOs::Windows;

    // GeForce Experience never shipped anywhere else.
    if (ev.isNvidiaServerSoftware) return HostOs::Windows;

    // Last and weakest, and the only one that needs a live stream to have run.
    return fromInitialTtl(ev.observedIpTtl);
}

} // namespace HostOsProbe
