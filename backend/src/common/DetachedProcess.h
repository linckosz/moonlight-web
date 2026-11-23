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
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace mw {

/// Ask a child process to start without our open descriptors or handles.
///
/// A child inherits what the parent left inheritable: on Unix every
/// descriptor not marked close-on-exec, on Windows every inheritable handle
/// when the process is created with inheritance on, which QProcess does. The
/// network libraries open most of their sockets that way. A child holding one
/// keeps its port bound after we close it, for as long as the child runs: an
/// installer's prompt nobody answers, a browser, a game, or our own relaunched
/// server, which would hold the sockets of the server it replaces.
///
/// On Unix Qt 6.6 and later close the descriptors for us. On Windows the
/// handles are not passed at all: none of our detached children has a
/// redirected standard channel, which is the only thing inheritance is
/// needed for here.
inline void closeInheritedDescriptors(QProcess& process)
{
#if defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    process.setUnixProcessParameters(QProcess::UnixProcessFlag::CloseFileDescriptors);
#elif defined(Q_OS_WIN)
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* arguments) { arguments->inheritHandles = false; });
#else
    Q_UNUSED(process);
#endif
}

/// QProcess::startDetached, without our open descriptors (see above). No
/// standard channel is redirected: with the handles kept back on Windows, a
/// redirected channel would not reach the child.
inline bool startDetached(const QString& program, const QStringList& arguments,
                          qint64* pid = nullptr)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    closeInheritedDescriptors(process);
    return process.startDetached(pid);
}

} // namespace mw
