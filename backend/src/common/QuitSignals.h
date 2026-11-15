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

#include <functional>

class QObject;

namespace mw {

/// The server has begun to quit: no new stream worker is started from now on
/// (a signal that ends the server reaches its workers too, and a browser that
/// sees its stream go would ask for another). Set once, never cleared.
void beginQuit();
bool quitting();
/// Tests only: back to "not quitting", for the next test in the same process.
void resetQuitForTests();

} // namespace mw

#ifndef Q_OS_WIN

/**
 * SIGTERM, SIGINT and SIGHUP end the server as its quit route does (Unix).
 *
 * The package stops the service with `systemctl stop`, removes a desktop
 * install's server with `pkill`, the machine shuts down, a session ends: each
 * sends one of these. By their default action the process ends at once, with
 * nothing taken off the router. With install(), the signal only writes one
 * byte to a pipe of this process; the event loop reads it and @p onSignal runs
 * on the main thread, which quits as the quit route does.
 *
 * A signal this process inherited as ignored (nohup's SIGHUP, the SIGINT of a
 * job started in the background by a script) stays ignored. Only the process
 * that installed the handler answers it: a child between its fork and its exec
 * still runs this handler, and writes nothing. The pipe is never inherited and
 * never closed.
 */
namespace mw::quitsignals {

/// Once, on the server's path, with the application object made and before
/// its event loop runs. False when the pipe or the notifier could not be made
/// (the signals then keep their default action), with @p why.
bool install(QObject* context, std::function<void(int signal)> onSignal, QString* why = nullptr);

/// The default action back for every signal install() took (one inherited as
/// ignored stays ignored): a second signal, or one during the teardown, ends
/// the process at once.
void restoreDefaults();

/// "SIGTERM", "SIGINT", "SIGHUP".
QString nameOf(int signal);

/// The pipe's two ends (tests): -1 before install().
int readEnd();
int writeEnd();

} // namespace mw::quitsignals

#endif
