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

#include "server/NetClassify.h"

#include <QString>
#include <functional>

/**
 * @brief The virtual gamepad bus: is it there, and may we offer to install it.
 *
 * ── Why this exists ─────────────────────────────────────────────────────────
 *
 * The installer already puts ViGEmBus down silently, but that can have failed
 * (no network while it downloaded), been bypassed (a build run from sources) or
 * been undone since. In every one of those cases the gamepad simply does not
 * work and NOTHING says so: the log knows, the user does not.
 *
 * So the frontend shows a discreet notice offering to install it. This is the
 * half of that which has to be right.
 *
 * ── The guard is the real subject ───────────────────────────────────────────
 *
 * The driver would land on the machine running THIS process, not on the one
 * running the browser. Showing that button to someone looking from another PC
 * offers them a change to a machine that is not in front of them, and they have
 * no way to guess it.
 *
 * Hence mayOffer(), whose condition is strictly NetClassify::Kind::Loopback —
 * NOT isPrivateOrLoopback(), NOT the LAN flag, and NOT HttpRequest::isLocal,
 * which a password-unlocked LAN admin also satisfies. Another PC on the same
 * network classifies as Private and is not the right machine. A visitor
 * arriving through the rendezvous classifies as Tunnel or Public and is
 * likewise hidden, with no extra rule to write.
 *
 * The verdict is computed here and sent to the browser as a boolean already
 * decided. The page must never derive it from its own URL: `localhost` in the
 * address bar proves nothing, an SSH tunnel manufactures it.
 */
namespace GamepadDriver {

/// What the driver itself answered — never a registry key or a file on disk,
/// both of which survive a partial uninstall and then lie.
struct Status
{
    bool supported = false; ///< this platform has a virtual-pad backend at all
    bool present = false;   ///< the driver answered: a pad can be created
    QString diagnostic;     ///< English, for the log line only
};

/// The answer, probed on first use and cached once the driver is there. While
/// it is missing every call asks again — that is the state the user can change
/// behind our back, by installing it by hand from the link the notice offers,
/// and the notice has to go away on its own when they do.
Status status();

/// Ask the driver again. Called after an install attempt — and nowhere else.
Status refresh();

/// Whether the notice may be shown to this caller at all.
///
/// @param kind      classification of the socket peer (NOT of the Host header)
/// @param viaTunnel the request arrived over a rendezvous control tunnel
///
/// Inline and pure, so the one rule that keeps a remote visitor from installing
/// drivers on someone else's machine is covered by a test that needs neither a
/// driver nor a network — see tests/test_gamepad_driver.cpp.
inline bool mayOffer(NetClassify::Kind kind, bool viaTunnel, const Status& status)
{
    // A request carried in over the rendezvous never touched a socket here, so
    // its peer address says nothing about where the caller sits.
    if (viaTunnel) return false;
    // Strictly this machine. Private is another PC on the same LAN — the notice
    // would offer it a change to a machine it cannot see.
    if (kind != NetClassify::Kind::Loopback) return false;
    // Nothing to install on a platform with no backend, and nothing to say once
    // the driver is there.
    if (!status.supported) return false;
    return !status.present;
}

/// Whether a one-click install can work here: the process must be elevated.
/// As a service (SYSTEM) it is; as a `--dev` instance or a build run from
/// sources it is not, and the notice then offers the upstream link instead of a
/// button that would fail behind a UAC prompt nobody sees.
bool canInstall();

/// Where the driver comes from. Also what the notice links to when canInstall()
/// is false, so the address is stated in exactly one place.
///
/// The exact asset the installer already fetches — one release covers x64, x86
/// and ARM64. Kept in step with backend/installer/moonlightweb.iss (VigemBusUrl):
/// the two must not drift, or a machine repaired from here ends up on a
/// different driver version than a machine installed normally.
inline QString downloadUrl()
{
    return QStringLiteral("https://github.com/nefarius/ViGEmBus/releases/download/"
                          "v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe");
}

/// SHA-256 of that exact asset, lowercase hex. The downloaded bundle runs as
/// SYSTEM; a transport that is merely HTTPS-to-GitHub is not a reason to hand
/// the machine to whatever bytes come back. Computed from the published file
/// (6 278 576 bytes) and pinned with the URL, so bumping one without the other
/// fails every install instead of silently trusting a new binary.
inline QString downloadSha256()
{
    return QStringLiteral("89220a7865076b342892f98865f3499fb7c4cfd673159e89d352c360fd014c6a");
}

/// How an install attempt ended.
enum class Result
{
    Installed,       ///< the bus answered on the re-probe: usable now
    RestartRequired, ///< the installer succeeded, the bus is not up yet
    Failed,          ///< nothing was installed
};

/// Download and install the driver silently, then re-probe.
///
/// Fully asynchronous — a driver install takes tens of seconds and this process
/// serves every other request meanwhile. @p done receives the outcome and, when
/// it is Failed, a human-readable reason.
///
/// RestartRequired is its own answer and not a failure: a driver install may
/// legitimately need a reboot, and telling someone "it did not work" when it
/// did — and a restart would prove it — sends them chasing a problem that no
/// longer exists.
///
/// Refuses a second concurrent attempt: the bundle would fight itself.
void install(std::function<void(Result result, QString error)> done);

} // namespace GamepadDriver
