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

#include <QProcess>
#include <QtGlobal>

#include <functional>
#include <vector>

namespace mw {

/// A child of the server receives its standard channels and nothing else.
///
/// Call it on a QProcess before start(), for a child whose standard channels
/// this process reads or writes (a detached child goes through
/// mw::startDetached, DetachedProcess.h). The child then gets its stdin,
/// stdout and stderr, and none of the other descriptors or handles of this
/// process: no socket, file or pipe that a library left inheritable.
///
/// Windows: the child is created with the list of the handles it may inherit
/// (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), made of its three standard handles.
/// When that list cannot be made, the child is not started: QProcess reports
/// FailedToStart, and one warning gives the system's code.
///
/// Unix: in the child, between its fork and its exec, every descriptor from 3
/// up is marked close-on-exec (close_range where the system has it,
/// otherwise one fcntl each, up to a bound read here, in the parent). Only
/// calls that are safe there are made: no allocation, no lock.
///
/// It sets the process's own modifier (the CreateProcess arguments on
/// Windows, the child's modifier on Unix); a caller that needs code of its own
/// in the child gives it to the second form below. Calling it twice on one
/// QProcess changes nothing the second time.
void passOnlyStdio(QProcess& process);

#ifndef Q_OS_WIN
/// The same, with descriptors that this child is meant to receive: those of
/// @p leftOpen (above 2) are not marked, and @p thenInChild runs in the child
/// after the marking, to clear their close-on-exec flag. It runs between fork
/// and exec, so it may only make the calls that are safe there.
/// Both are fixed by the first call on a QProcess: a later call changes
/// nothing, so a child that needs other descriptors gets a new QProcess.
void passOnlyStdio(QProcess& process, std::vector<int> leftOpen, std::function<void()> thenInChild);
#endif

namespace childstdio {
/// Tests only: the next child started through passOnlyStdio is refused as if
/// its handle list could not be made (Windows; nothing on Unix).
void refuseNextForTests();
} // namespace childstdio

} // namespace mw
