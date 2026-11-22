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

#include <QString>
#include <QStringList>

#include <functional>

/// The server restarting itself: what POST /api/system/restart does (the
/// tray's "Restart"), and what a setting that only a new process can apply
/// asks for (SharedPortsRestart).
///
/// Under a supervisor (MW_SERVICE: the Windows service, systemd, launchd) the
/// process exits with the code that means "come back". Without one (the
/// Windows logon task, a desktop autostart, a --dev) it starts a fresh copy of
/// itself first, given the arguments it was started with that a server run
/// keeps (relaunchArguments) and "--after-pid <its pid>": the copy waits for
/// this process to be gone before it takes the single-instance lock and the
/// ports (waitForPredecessor). Without that wait the copy lost the lock to the
/// process still exiting, and no server came back.
namespace mw::restart {

/// The exit code a supervisor reads as "start me again".
constexpr int kServiceRestartExitCode = 79;

/// The arguments a relaunched server is given, out of those it was started
/// with: an allowlist, never what is not on it. Flags: --autostart, --noadmin,
/// --dev, --debug, --verbose. With a value: --port, --ws-port, --log, --config.
/// Everything else is a one-off gesture (--print-link, --local-request,
/// --status, a PIN, a claim...) or a helper's verb, and is not repeated.
QStringList relaunchArguments(const std::function<bool(const QString& name)>& isSet,
                              const std::function<QString(const QString& name)>& value);

/// Set once by main(), from its parser (paths made absolute).
void setRelaunchArguments(const QStringList& arguments);

/// "--after-pid"'s value: decimal digits only, a number above 0 that fits; any
/// other text is no PID and is ignored.
bool parsePid(const QString& text, qint64* pid);

/// "--after-pid": opens the process now, checks that it is this executable,
/// then waits on what was opened (never on the number again) until it is gone,
/// at most @p timeoutMs. False when there was nothing to wait for (no such
/// process, another program, no right to look) or the wait ran out: the start
/// goes on either way.
bool waitForPredecessor(qint64 pid, int timeoutMs);

/// Restarts the server, as said above. @p why goes to the log. False, nothing
/// changed, when the fresh copy could not be started.
bool now(const QString& why);

} // namespace mw::restart
