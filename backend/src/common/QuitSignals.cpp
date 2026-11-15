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

#include "common/QuitSignals.h"

#include <atomic>

namespace {
std::atomic<bool> g_Quitting{false};
}

namespace mw {

void beginQuit()
{
    g_Quitting.store(true);
}

bool quitting()
{
    return g_Quitting.load();
}

void resetQuitForTests()
{
    g_Quitting.store(false);
}

} // namespace mw

#ifndef Q_OS_WIN

#include <QObject>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr int kSignals[] = {SIGTERM, SIGINT, SIGHUP};
constexpr int kCount = static_cast<int>(sizeof(kSignals) / sizeof(kSignals[0]));

// Written once by install() before any handler is set, read by the handler.
int g_Read = -1;
int g_Write = -1;
pid_t g_Pid = 0;
bool g_Taken[kCount] = {};

extern "C" void onQuitSignal(int signal)
{
    // Only the process that installed it: a child between fork and exec runs
    // this too, with the pipe it inherited for that instant.
    if (::getpid() != g_Pid) return;
    const int saved = errno;
    const unsigned char byte = static_cast<unsigned char>(signal);
#if defined(MSG_NOSIGNAL)
    // Never a SIGPIPE from here, never a wait: a full pipe drops the byte,
    // and one byte already waiting is enough.
    (void)::send(g_Write, &byte, 1, MSG_NOSIGNAL | MSG_DONTWAIT);
#else
    // macOS: SO_NOSIGPIPE on the socket, and the end is non-blocking.
    (void)::write(g_Write, &byte, 1);
#endif
    errno = saved;
}

[[maybe_unused]] bool closeOnExec(int fd)
{
    const int flags = ::fcntl(fd, F_GETFD);
    return flags >= 0 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool nonBlocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

} // namespace

namespace mw::quitsignals {

bool install(QObject* context, std::function<void(int signal)> onSignal, QString* why)
{
    const auto fail = [why](const char* what) {
        if (why) *why = QString::fromLatin1(what);
        return false;
    };
    if (g_Write >= 0) return fail("installed already");
    int ends[2] = {-1, -1};
#if defined(SOCK_CLOEXEC)
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends) != 0)
        return fail("no pipe could be made");
#else
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0) return fail("no pipe could be made");
    if (!closeOnExec(ends[0]) || !closeOnExec(ends[1])) {
        ::close(ends[0]);
        ::close(ends[1]);
        return fail("the pipe could not be kept from children");
    }
#endif
#if defined(SO_NOSIGPIPE)
    const int one = 1;
    ::setsockopt(ends[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    ::setsockopt(ends[1], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (!nonBlocking(ends[0]) || !nonBlocking(ends[1])) {
        ::close(ends[0]);
        ::close(ends[1]);
        return fail("the pipe could not be made non-blocking");
    }
    // Both ends stay open for the life of the process: a handler may run at
    // any moment, and a closed number could be someone else's by then.
    g_Read = ends[0];
    g_Write = ends[1];
    g_Pid = ::getpid();

    auto* notifier = new QSocketNotifier(g_Read, QSocketNotifier::Read, context);
    QObject::connect(notifier, &QSocketNotifier::activated, context,
                     [notifier, onSignal = std::move(onSignal)]() {
                         unsigned char byte = 0;
                         int got = 0;
                         // Every byte waiting: one signal or several, one answer.
                         while (::read(g_Read, &byte, 1) == 1)
                             got = byte;
                         if (got == 0) return;
                         notifier->setEnabled(false);
                         if (onSignal) onSignal(got);
                     });

    for (int i = 0; i < kCount; ++i) {
        struct sigaction old = {};
        if (::sigaction(kSignals[i], nullptr, &old) != 0) continue;
        // Inherited as ignored: it stays so (nohup, a background job).
        if (old.sa_handler == SIG_IGN) continue;
        struct sigaction mine = {};
        mine.sa_handler = onQuitSignal;
        sigemptyset(&mine.sa_mask);
        mine.sa_flags = SA_RESTART;
        if (::sigaction(kSignals[i], &mine, nullptr) == 0) g_Taken[i] = true;
    }
    return true;
}

void restoreDefaults()
{
    for (int i = 0; i < kCount; ++i) {
        if (!g_Taken[i]) continue;
        struct sigaction dfl = {};
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        ::sigaction(kSignals[i], &dfl, nullptr);
        g_Taken[i] = false;
    }
}

QString nameOf(int signal)
{
    switch (signal) {
    case SIGTERM: return QStringLiteral("SIGTERM");
    case SIGINT: return QStringLiteral("SIGINT");
    case SIGHUP: return QStringLiteral("SIGHUP");
    default: break;
    }
    return QStringLiteral("signal %1").arg(signal);
}

int readEnd()
{
    return g_Read;
}

int writeEnd()
{
    return g_Write;
}

} // namespace mw::quitsignals

#endif
