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
#include <QtGlobal>

/**
 * The launcher service: a stream worker that runs as SYSTEM on the console
 * desktop, so the secure desktop is no longer out of reach.
 *
 * ── Why a service at all ────────────────────────────────────────────────────
 *
 * The scheduled task of §30 level 1 raises the worker to the user's FULL token,
 * which buys every window an administrator can touch. It does not buy the
 * *secure desktop*: the UAC prompt, the lock screen and the Ctrl+Alt+Suppr
 * screen live on `Winlogon`, a separate desktop of the same window station,
 * and Windows lets nothing below SYSTEM open it. Desktop Duplication started
 * from a user process stops at the switch, and SendInput from a user process
 * reaches nothing there. Parsec, Sunshine and RDP all answer this the same
 * way, and so does this: a tiny LocalSystem service whose only job is to start
 * the worker in the console session with a SYSTEM token.
 *
 * ── What is, and is not, raised ─────────────────────────────────────────────
 *
 * Only the worker. The server that listens on the network, decodes WebRTC,
 * draws the tray and opens the browser keeps running as the ordinary user, as
 * it always has — a network-facing process is the last one to hand SYSTEM to.
 * The service itself never parses anything from the network: it reads one
 * line — a pipe name — from a local pipe, and starts one fixed command.
 *
 * ── The trust boundary, stated plainly ──────────────────────────────────────
 *
 * Any process of the console user may ask the service for a worker, and gets a
 * SYSTEM process talking to pipes it created. That is a real boundary, and it
 * is the same one Parsec's and Sunshine's services have. It is narrowed to what
 * the feature actually needs:
 *
 *  - The service runs ONE command, the image it was installed as, with fixed
 *    arguments. Nothing from the request becomes a path, a flag or a shell word.
 *  - The only thing the request carries is a pipe base name, and it is accepted
 *    only as `[A-Za-z0-9._-]{1,128}` — no separator, no traversal, no UNC.
 *  - The caller must be on the active console session (the token behind the
 *    pipe is impersonated and its session id compared), and its image must be
 *    this same executable under the installation directory.
 *  - The worker checks the same thing from its side before it reads a byte of
 *    config: a pipe server that is not this executable is refused.
 *  - The server checks the worker too, which it can only do because the
 *    service gives the worker a DACL of its own: the user who asked may read
 *    its image, wait on it and stop it — no more (a SYSTEM token's default
 *    DACL shuts a non-elevated server out, and v0.3.1 fell back to the task on
 *    every session for that alone).
 *  - The service is demand-start and does nothing until asked.
 *
 * What it deliberately does NOT claim: a user who can run the installed
 * executable can obtain a SYSTEM worker and hand it a session of their own.
 * The machine's owner installs this on purpose, exactly as they install
 * Parsec; the feature cannot exist without that step, and saying so in the
 * design is better than pretending the check closes it.
 *
 * ── Transport ───────────────────────────────────────────────────────────────
 *
 * Unchanged from level 1: three named pipes per worker, created by the server,
 * their names passed through. Only the DACL grows a SYSTEM entry, since the
 * worker is now SYSTEM. See ConsoleProcess::startThroughService().
 */
namespace WorkerService {

/// The Windows service name, per edition and per --dev, like every other
/// registration ("MoonlightWeb Worker", "MoonlightWebDev Worker",
/// "MoonlightWeb-dev Worker").
QString serviceName();

/// The control pipe's base name (no `\\.\pipe\` prefix), per edition too, so a
/// DEV install and a production one never answer for each other.
QString controlPipeName();

/// The control pipe's security descriptor, in SDDL. SYSTEM and the
/// administrators have it all. The interactive user gets 0x12019b: connect
/// (SYNCHRONIZE), read and write the data, the attributes and the extended
/// attributes, and read the descriptor. Not FILE_CREATE_PIPE_INSTANCE (0x4):
/// only this service makes instances of its own pipe.
inline constexpr char kControlPipeSddl[] = "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x12019b;;;IU)";

/// What a caller opens the control pipe with: GENERIC_READ and FILE_WRITE_DATA.
/// GENERIC_WRITE would also ask for FILE_APPEND_DATA, which on a pipe is
/// FILE_CREATE_PIPE_INSTANCE, and kControlPipeSddl gives the user no such
/// right.
constexpr quint32 kControlPipeClientAccess = 0x80000000u | 0x00000002u;

/// Whether the pipe a caller reached is this service's: the process at its
/// server end (GetNamedPipeServerProcessId) is the one the service manager
/// runs the service as (@p servicePid, 0 when it is not running). A pipe of
/// that name served by anything else is never spoken to.
inline bool controlPipeServerIsService(quint32 pipeServerPid, quint32 servicePid)
{
    return servicePid != 0 && pipeServerPid == servicePid;
}

/// Whether a native worker started from this desktop process should go through
/// the service: Windows, on the desktop, the service registered with THIS
/// executable as its image, and in a state it can be started from. Cached like
/// ConsoleSession::elevatedWorkerAvailable(), and false for the rest of the run
/// once a launch through it has failed.
///
/// Preferred over the elevated task when both are there: everything the task
/// gives, plus the secure desktop.
bool available();

/// Ask the service to start a worker attached to the three pipes named after
/// `base`. Returns true once the service has reported the process created —
/// the worker connecting back is then awaited on the pipes themselves.
bool requestWorker(const QString& base, QString* error);

/// Ask the service for a worker on the console's sign-in screen, attached to
/// the three pipes named after `base`: Remote Desktop left this process's
/// session Disconnected, with no display, and the console shows the sign-in
/// screen of a session nobody is signed in on (RemoteDesktopReturnPolicy.h).
/// The worker streams that screen and nothing else (--signin-screen, which
/// the service writes). True once the service reports the process created;
/// a refusal comes back as the reason, in English.
bool requestSignInWorker(const QString& base, QString* error);

/// Ask the service to press Ctrl+Alt+Suppr on the console session. Windows
/// takes SendSAS from a service in session 0 and ignores it from the worker,
/// which runs in the console session, SYSTEM or not (design §31.7). The same
/// checks on the caller as for a worker; true once the service says it
/// pressed it, false with the reason (the machine's policy, most often).
bool requestSecureAttention(QString* error);

/// Ask the service to put this process's own session back on the console:
/// Remote Desktop left it Disconnected, with no display at all, and nobody is
/// signed in on the PC (RemoteDesktopReturnPolicy.h). True once the service
/// says it is done; the caller then locks the session (RemoteDesktopReturn).
/// A refusal comes back as the reason, in English.
bool requestConsoleReturn(QString* error);

/// The way back when a return could not lock the session: ask the service to
/// disconnect this process's own session again, as Remote Desktop had left it.
/// Only while that session is Active on the console. Used when the server's
/// own WTSDisconnectSession is refused.
bool requestHide(QString* error);

/// `--worker-service`: the service's own entry point. Runs the control loop
/// until the SCM stops it, and returns the process exit code.
int runService();

/// `--worker-service-install` / `--worker-service-remove`, run elevated by the
/// installer. Both are idempotent, and return a process exit code.
int installService();
int removeService();

} // namespace WorkerService
