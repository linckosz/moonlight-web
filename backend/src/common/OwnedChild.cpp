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

#include "common/OwnedChild.h"

#include <QDir>
#include <QWinEventNotifier>

#include <vector>

#include <windows.h>

namespace mw {

OwnedChild::OwnedChild(QObject* parent)
    : QObject(parent)
{}

OwnedChild::~OwnedChild()
{
    // Never TerminateProcess: the child is what asked this process to quit.
    delete m_Notifier;
    if (m_Process) ::CloseHandle(static_cast<HANDLE>(m_Process));
}

QString OwnedChild::commandLine(const QString& program, const QStringList& arguments)
{
    // As qt_create_commandline (qprocess_win.cpp) makes it, so that the
    // installer and schtasks read the same arguments they read today.
    QString args;
    if (!program.isEmpty()) {
        QString name = program;
        if (!name.startsWith(QLatin1Char('"')) && !name.endsWith(QLatin1Char('"')) &&
            name.contains(QLatin1Char(' ')))
            name = QLatin1Char('"') + name + QLatin1Char('"');
        name.replace(QLatin1Char('/'), QLatin1Char('\\'));
        args = name + QLatin1Char(' ');
    }
    for (const QString& argument : arguments) {
        QString tmp = argument;
        qsizetype index = tmp.indexOf(QLatin1Char('"'));
        while (index >= 0) {
            tmp.insert(index++, QLatin1Char('\\'));
            for (qsizetype i = index - 2; i >= 0 && tmp.at(i) == QLatin1Char('\\'); --i) {
                tmp.insert(i, QLatin1Char('\\'));
                index++;
            }
            index = tmp.indexOf(QLatin1Char('"'), index + 1);
        }
        if (tmp.isEmpty() || tmp.contains(QLatin1Char(' ')) || tmp.contains(QLatin1Char('\t'))) {
            qsizetype i = tmp.size();
            while (i > 0 && tmp.at(i - 1) == QLatin1Char('\\'))
                --i;
            tmp.insert(i, QLatin1Char('"'));
            tmp.prepend(QLatin1Char('"'));
        }
        args += QLatin1Char(' ') + tmp;
    }
    return args;
}

bool OwnedChild::start(const QString& program, const QStringList& arguments, Finished finished,
                       QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (m_Process) return fail(QStringLiteral("already started"));
    if (!QDir::isAbsolutePath(program)) return fail(QStringLiteral("not an absolute path"));

    const std::wstring application = QDir::toNativeSeparators(program).toStdWString();
    // CreateProcessW may write into the command line: a buffer of its own.
    std::wstring line = commandLine(program, arguments).toStdWString();
    std::vector<wchar_t> buffer(line.begin(), line.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info = {};
    if (!::CreateProcessW(application.c_str(), buffer.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
        return fail(QStringLiteral("CreateProcess failed (%1)").arg(::GetLastError()));
    ::CloseHandle(info.hThread);
    m_Process = info.hProcess;
    m_Finished = std::move(finished);
    m_Notifier = new QWinEventNotifier(info.hProcess);
    connect(m_Notifier, &QWinEventNotifier::activated, this, [this]() { ended(); });
    return true;
}

void OwnedChild::ended()
{
    if (!m_Process) return;
    DWORD code = 0;
    if (!::GetExitCodeProcess(static_cast<HANDLE>(m_Process), &code)) code = 0xFFFFFFFFu;
    m_Notifier->setEnabled(false);
    if (m_Finished) {
        // Once: the slot may destroy this object.
        Finished told = std::move(m_Finished);
        m_Finished = nullptr;
        told(static_cast<quint32>(code));
    }
}

} // namespace mw
