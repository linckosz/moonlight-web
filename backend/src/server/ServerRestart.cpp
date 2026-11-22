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

#include "server/ServerRestart.h"

#include "common/DetachedProcess.h"
#include "common/Logger.h"
#include "common/QuitSignals.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTimer>

#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <sys/event.h>
#include <unistd.h>
#else
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#endif

namespace mw::restart {

namespace {
QStringList g_Arguments;
} // namespace

QStringList relaunchArguments(const std::function<bool(const QString& name)>& isSet,
                              const std::function<QString(const QString& name)>& value)
{
    QStringList out;
    for (const char* flag : {"autostart", "noadmin", "dev", "debug", "verbose"}) {
        const QString name = QLatin1String(flag);
        if (isSet(name)) out << QStringLiteral("--") + name;
    }
    for (const char* option : {"port", "ws-port", "log", "config"}) {
        const QString name = QLatin1String(option);
        if (isSet(name)) out << QStringLiteral("--") + name << value(name);
    }
    return out;
}

void setRelaunchArguments(const QStringList& arguments)
{
    g_Arguments = arguments;
}

bool parsePid(const QString& text, qint64* pid)
{
    if (text.isEmpty() || text.size() > 10) return false;
    for (const QChar c : text)
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
    bool ok = false;
    const qint64 n = text.toLongLong(&ok);
    if (!ok || n <= 0 || n > std::numeric_limits<qint32>::max()) return false;
    if (pid) *pid = n;
    return true;
}

namespace {
// The same file as this executable, by its full path.
bool sameImage(const QString& path)
{
    if (path.isEmpty()) return false;
    const QString mine = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    const QString theirs = QFileInfo(path).canonicalFilePath();
#ifdef _WIN32
    return !mine.isEmpty() && mine.compare(theirs, Qt::CaseInsensitive) == 0;
#else
    return !mine.isEmpty() && mine == theirs;
#endif
}
} // namespace

bool waitForPredecessor(qint64 pid, int timeoutMs)
{
    if (pid <= 0 || pid == QCoreApplication::applicationPid()) return false;
#ifdef _WIN32
    // Opened first: from here the handle names that process, whatever becomes
    // of its number.
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(pid));
    if (!process) return false;
    wchar_t image[MAX_PATH * 4];
    DWORD size = static_cast<DWORD>(sizeof(image) / sizeof(image[0]));
    const bool ours = QueryFullProcessImageNameW(process, 0, image, &size) &&
                      sameImage(QString::fromWCharArray(image, static_cast<int>(size)));
    bool gone = false;
    if (ours) gone = WaitForSingleObject(process, static_cast<DWORD>(timeoutMs)) == WAIT_OBJECT_0;
    CloseHandle(process);
    return ours && gone;
#elif defined(__APPLE__)
    // The kqueue registration is the handle: it follows that process, not a
    // later one given its number.
    const int queue = kqueue();
    if (queue < 0) return false;
    struct kevent change;
    EV_SET(&change, static_cast<uintptr_t>(pid), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0,
           nullptr);
    if (kevent(queue, &change, 1, nullptr, 0, nullptr) != 0) {
        close(queue);
        return false;
    }
    char path[PROC_PIDPATHINFO_MAXSIZE];
    const int n = proc_pidpath(static_cast<int>(pid), path, sizeof(path));
    bool gone = false;
    if (n > 0 && sameImage(QString::fromUtf8(path, n))) {
        struct kevent event;
        const struct timespec limit = {timeoutMs / 1000, (timeoutMs % 1000) * 1000000L};
        gone = kevent(queue, nullptr, 0, &event, 1, &limit) == 1;
    }
    close(queue);
    return gone;
#else
    // Linux: the process is named by its number AND its start time (field 22
    // of /proc/<pid>/stat), read once: a number given to another process
    // later has another start time.
    const QString dir = QStringLiteral("/proc/%1").arg(pid);
    const auto startTime = [&dir]() -> QByteArray {
        QFile stat(dir + QStringLiteral("/stat"));
        if (!stat.open(QIODevice::ReadOnly)) return {};
        const QByteArray line = stat.readAll();
        // After the command's closing parenthesis, fields 3.. are spaced.
        const int close = line.lastIndexOf(')');
        if (close < 0) return {};
        const QList<QByteArray> fields = line.mid(close + 2).split(' ');
        // A process that has ended and is not yet reaped (Z, X) is gone.
        if (fields.isEmpty() || fields.at(0) == "Z" || fields.at(0) == "X") return {};
        return fields.size() > 19 ? fields.at(19) : QByteArray();
    };
    const QByteArray started = startTime();
    if (started.isEmpty()) return false;
    if (!sameImage(QFile::symLinkTarget(dir + QStringLiteral("/exe")))) return false;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (startTime() != started) return true;
        QThread::msleep(100);
    }
    return false;
#endif
}

bool now(const QString& why)
{
    const bool supervised = !qEnvironmentVariableIsEmpty("MW_SERVICE");
    if (!supervised) {
        QStringList arguments = g_Arguments;
        arguments << QStringLiteral("--after-pid")
                  << QString::number(QCoreApplication::applicationPid());
        if (!mw::startDetached(QCoreApplication::applicationFilePath(), arguments)) {
            Logger::warning(QStringLiteral("[System] Restart (%1) not done: a new copy of the "
                                           "server could not be started")
                                .arg(why));
            return false;
        }
    }
    Logger::info(QStringLiteral("[System] Restart (%1): %2")
                     .arg(why, supervised ? QStringLiteral("supervisor respawn")
                                          : QStringLiteral("self relaunch")));
    // From here nothing new starts in this process (common/QuitSignals.h).
    mw::beginQuit();
    // The exit waits a moment, so that an HTTP answer under way can leave
    // before its socket goes.
    QTimer::singleShot(300, qApp, [supervised]() {
        QCoreApplication::exit(supervised ? kServiceRestartExitCode : 0);
    });
    return true;
}

} // namespace mw::restart
