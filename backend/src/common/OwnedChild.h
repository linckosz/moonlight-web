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

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QWinEventNotifier;

namespace mw {

/**
 * A child process (Windows) whose end is told, and which this process never
 * ends: not when this object goes, not when this process quits.
 *
 * QProcess ends its child when it is destroyed, and the self-update starts
 * the installer as one: the installer then asks this very server to quit
 * (--quit-server), the server quits cleanly, and its QProcess would end the
 * installer that asked. This one only closes its handles.
 *
 * The program is named by its absolute path to CreateProcessW, never by the
 * command line alone (an unquoted path with a space would run another
 * program). The command line is built as QProcess builds it.
 */
class OwnedChild : public QObject
{
public:
    /// The child's exit code, once it has ended.
    using Finished = std::function<void(quint32 exitCode)>;

    explicit OwnedChild(QObject* parent = nullptr);
    /// Closes the handles. The child goes on.
    ~OwnedChild() override;

    /// Starts @p program, an absolute path, with @p arguments; no handle is
    /// inherited, no window is made. False, with @p error, when it did not
    /// start. Once only.
    bool start(const QString& program, const QStringList& arguments, Finished finished,
               QString* error = nullptr);

    /// The child's process handle, while this object holds it: for a test
    /// that waits on its own copy.
    void* process() const { return m_Process; }

    /// The command line, as QProcess makes it: the program quoted when it has
    /// a space, slashes turned around; each argument with its quotes escaped
    /// (and the backslashes before them doubled), quoted when it is empty or
    /// has a space or a tab, with trailing backslashes outside the quote.
    static QString commandLine(const QString& program, const QStringList& arguments);

private:
    void ended();

    void* m_Process = nullptr;
    QWinEventNotifier* m_Notifier = nullptr;
    Finished m_Finished;
};

} // namespace mw
