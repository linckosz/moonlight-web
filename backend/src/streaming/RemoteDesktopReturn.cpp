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

#include "RemoteDesktopReturn.h"

#include "ConsoleSession.h"
#include "WorkerService.h"
#include "common/Logger.h"
#include "server/AppSettings.h"

#include <QElapsedTimer>

#include <thread>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wtsapi32.h>
#endif

namespace {

/// How often the session's state is read: one WTS call. The card changes
/// within a few of these after mstsc is closed.
constexpr int kTickMs = 2000;

/// How long the session may take to show up on the console once the launcher
/// says it moved it, before the lock is attempted anyway.
constexpr int kArrivalTimeoutMs = 5000;

/// The lock after the move: tries, the pause between two, and how long each
/// waits for the input desktop to leave the user's. Ten tries cost under
/// three seconds in the worst case; after them the session is sent away
/// again rather than left unlocked on the PC's screen.
constexpr int kLockAttempts = 10;
constexpr int kLockRetryMs = 50;
constexpr int kLockConfirmMs = 200;

#ifdef Q_OS_WIN

/// The session is locked: the input desktop is no longer the user's own
/// "Default". When it is Winlogon (the lock screen), a user process cannot
/// even open it — which says the same.
bool inputDesktopLocked()
{
    HDESK desk = ::OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    // Access denied is Winlogon's answer to a user process. Any other failure
    // proves nothing, and a doubt sends the session away rather than calling
    // it locked.
    if (!desk) return ::GetLastError() == ERROR_ACCESS_DENIED;
    wchar_t name[64] = {};
    DWORD needed = 0;
    const bool named =
        ::GetUserObjectInformationW(desk, UOI_NAME, name, sizeof(name) - sizeof(wchar_t), &needed);
    ::CloseDesktop(desk);
    return named && ::_wcsicmp(name, L"Default") != 0;
}

bool waitLocked(int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    do {
        if (inputDesktopLocked()) return true;
        ::Sleep(10);
    } while (clock.elapsed() < timeoutMs);
    return inputDesktopLocked();
}

/// Send this session away from the screen again, to where Remote Desktop had
/// left it. A user may disconnect their own session (tsdiscon needs no
/// right); the launcher is asked only when Windows refuses it here. Empty on
/// success, the reasons otherwise.
QString hideAgain()
{
    if (::WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, FALSE)) return {};
    const DWORD own = ::GetLastError();
    QString why;
    if (WorkerService::requestHide(&why)) return {};
    return QStringLiteral("COULD NOT disconnect it again (here: error %1; launcher: %2)")
        .arg(own)
        .arg(why);
}

DWORD ownSession()
{
    DWORD session = 0xFFFFFFFF;
    if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &session)) session = 0xFFFFFFFF;
    return session;
}

RemoteDesktopReturnPolicy::Observation observe()
{
    RemoteDesktopReturnPolicy::Observation now;
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                      WTSConnectState, &buffer, &bytes) &&
        buffer) {
        if (bytes >= sizeof(WTS_CONNECTSTATE_CLASS))
            now.state = static_cast<int>(*reinterpret_cast<WTS_CONNECTSTATE_CLASS*>(buffer));
        ::WTSFreeMemory(buffer);
    }
    buffer = nullptr;
    bytes = 0;
    if (::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                      WTSClientProtocolType, &buffer, &bytes) &&
        buffer) {
        // 0 the console, 2 Remote Desktop (1 is a retired ICA value).
        if (bytes >= sizeof(USHORT)) now.remote = *reinterpret_cast<USHORT*>(buffer) == 2;
        ::WTSFreeMemory(buffer);
    }
    return now;
}

#endif

} // namespace

RemoteDesktopReturn& RemoteDesktopReturn::instance()
{
    static RemoteDesktopReturn s_Instance;
    return s_Instance;
}

void RemoteDesktopReturn::start()
{
#ifdef Q_OS_WIN
    if (m_Started) return;
    m_Started = true;
    // A server in session 0 (the old NSSM service) has no session of a user to
    // watch: what it streams is the console's, whoever is on it.
    if (ConsoleSession::launchesElsewhere()) return;
    m_Watching = true;
    m_Timer.setInterval(kTickMs);
    connect(&m_Timer, &QTimer::timeout, this, &RemoteDesktopReturn::onTick);
    m_Timer.start();
    onTick();
#endif
}

bool RemoteDesktopReturn::allowed() const
{
#ifdef Q_OS_WIN
    if (!m_Watching) return false;
    AppSettings settings;
    return settings.remoteDesktopReturn() && settings.nativeHostEnabled() &&
           WorkerService::available();
#else
    return false;
#endif
}

bool RemoteDesktopReturn::offersReturn() const
{
    // The settings file is read only when there is something to offer.
    if (!m_LeftByRemoteDesktop || !RemoteDesktopReturnPolicy::kReturnOffered) return false;
    return RemoteDesktopReturnPolicy::returnOffered(true, allowed());
}

bool RemoteDesktopReturn::signInAllowed() const
{
#ifdef Q_OS_WIN
    if (!m_Watching) return false;
    return AppSettings().nativeHostEnabled() && WorkerService::available();
#else
    return false;
#endif
}

bool RemoteDesktopReturn::offersSignIn() const
{
    // The settings file is read only when there is something to offer.
    if (!m_LeftByRemoteDesktop || !RemoteDesktopReturnPolicy::kSignInOffered) return false;
    return RemoteDesktopReturnPolicy::signInOffered(true, signInAllowed());
}

QString RemoteDesktopReturn::signInRefusal(bool booth) const
{
    return RemoteDesktopReturnPolicy::signInRequestRefusal(m_Tracker, signInAllowed(), booth);
}

bool RemoteDesktopReturn::backOnScreenNow()
{
#ifdef Q_OS_WIN
    return observe().state == RemoteDesktopReturnPolicy::Active &&
           ::WTSGetActiveConsoleSessionId() == ownSession();
#else
    return false;
#endif
}

QString RemoteDesktopReturn::signInOutcomeNow()
{
#ifdef Q_OS_WIN
    if (backOnScreenNow()) return QStringLiteral("signed in: this session is back on the screen");
    const DWORD console = ::WTSGetActiveConsoleSessionId();
    if (console != 0xFFFFFFFF && console != ownSession()) {
        LPWSTR name = nullptr;
        DWORD bytes = 0;
        bool someone = false;
        if (::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, console, WTSUserName, &name,
                                          &bytes) &&
            name) {
            someone = name[0] != 0;
            ::WTSFreeMemory(name);
        }
        // The account is not named: who signed in on the PC is not this
        // log's business, only that the stream stopped for it.
        if (someone)
            return QStringLiteral("stopped: another account is signed in on the PC (session %1)")
                .arg(console);
    }
    return QStringLiteral("closed: the PC still shows its sign-in screen");
#else
    return QStringLiteral("closed");
#endif
}

void RemoteDesktopReturn::bringBack(std::function<void(bool ok, const QString& error)> done)
{
    if (!done) return;
    // A second stream asked while the first one's return is under way: both
    // get its result, and the session moves once.
    if (m_Asking.load()) {
        m_Waiting.append(std::move(done));
        return;
    }
    const QString refusal = RemoteDesktopReturnPolicy::requestRefusal(m_Tracker, allowed());
    if (!refusal.isEmpty()) {
        Logger::info("[rdp] a stream asked for this PC's screen, not brought back: " + refusal);
        done(false,
             QStringLiteral("This PC's session cannot be brought back to its screen: ") + refusal);
        return;
    }
    m_Waiting.append(std::move(done));
    askReturn();
}

void RemoteDesktopReturn::onTick()
{
#ifdef Q_OS_WIN
    // While a return is under way the session passes through states that say
    // nothing about Remote Desktop (on the console, unlocked, for an instant):
    // the tracker hears the result instead (returnFinished).
    if (m_Asking.load()) return;
    const RemoteDesktopReturnPolicy::Observation now = observe();
    const bool left = m_Tracker.observe(now);

    const bool stateMoved = now.state != m_LastState;
    const bool leftMoved = left != m_LeftByRemoteDesktop;
    if (stateMoved && m_LastState != -1)
        Logger::info(QStringLiteral("[rdp] this session (%1) is now %2%3")
                         .arg(ownSession())
                         .arg(now.state == RemoteDesktopReturnPolicy::Active
                                  ? QStringLiteral("active")
                              : now.state == RemoteDesktopReturnPolicy::Disconnected
                                  ? QStringLiteral("disconnected")
                                  : QStringLiteral("in state %1").arg(now.state))
                         .arg(now.state == RemoteDesktopReturnPolicy::Active && now.remote
                                  ? QStringLiteral(", through Remote Desktop")
                                  : QString()));
    m_LastState = now.state;
    m_LeftByRemoteDesktop = left;
    if (leftMoved) {
        Logger::info(left ? QStringLiteral("[rdp] Remote Desktop left this session without a "
                                           "screen")
                          : QStringLiteral("[rdp] this session is no longer left by Remote "
                                           "Desktop"));
    }
    // Back on a screen, or away from it: the displays the engine sees changed
    // with it, and the card follows.
    if (leftMoved || (stateMoved && now.state == RemoteDesktopReturnPolicy::Active)) emit changed();
#endif
}

void RemoteDesktopReturn::askReturn()
{
#ifdef Q_OS_WIN
    if (m_Asking.exchange(true)) return;
    const DWORD session = ownSession();
    Logger::info(QStringLiteral("[rdp] asking the launcher to put session %1 back on this PC's "
                                "screen, locked")
                     .arg(session));
    // The pipe round trip, the wait for the session to arrive and the lock take
    // up to a few seconds: off the event loop, which serves the web UI
    // meanwhile. The ticks are not looked at until it is over (onTick).
    std::thread([this, session]() {
        // Locked BEFORE it moves, when Windows takes the lock in a
        // disconnected session: the screen then never shows the desktop. When
        // it does not, the lock after the move is the one that counts.
        const bool lockedBefore = ::LockWorkStation() != 0;
        const DWORD lockBeforeError = lockedBefore ? 0 : ::GetLastError();
        const QString before = lockedBefore
                                   ? QStringLiteral("lock before: ok")
                                   : QStringLiteral("lock before: error %1").arg(lockBeforeError);

        auto finish = [this](Outcome outcome, const QString& detail) {
            QMetaObject::invokeMethod(
                this, [this, outcome, detail]() { returnFinished(outcome, detail); },
                Qt::QueuedConnection);
        };

        QString why;
        QElapsedTimer clock;
        clock.start();
        if (!WorkerService::requestConsoleReturn(&why)) {
            finish(Outcome::Refused, why);
            return;
        }
        const qint64 movedMs = clock.elapsed();

        // Locked only once the session IS on the screen: a lock tried before
        // it arrives fails, and the session would then arrive unlocked.
        while (::WTSGetActiveConsoleSessionId() != session && clock.elapsed() < kArrivalTimeoutMs)
            ::Sleep(5);
        if (::WTSGetActiveConsoleSessionId() != session) {
            // It may still arrive later, unlocked: it is sent away again.
            const QString hidden = hideAgain();
            finish(hidden.isEmpty() ? Outcome::Hidden : Outcome::Exposed,
                   QStringLiteral("not on the console %1 ms after the launcher moved it (%2); %3")
                       .arg(kArrivalTimeoutMs)
                       .arg(before)
                       .arg(hidden.isEmpty() ? QStringLiteral("disconnected again") : hidden));
            return;
        }
        const qint64 arrivedMs = clock.elapsed();

        // LockWorkStation only starts the lock: it counts once the input
        // desktop is no longer the user's. Tried a few times before giving up.
        int attempts = 0;
        bool locked = false;
        DWORD lockError = 0;
        while (!locked && attempts < kLockAttempts) {
            ++attempts;
            if (::LockWorkStation()) {
                locked = waitLocked(kLockConfirmMs);
                if (!locked) lockError = ERROR_TIMEOUT;
            } else {
                lockError = ::GetLastError();
            }
            if (!locked) ::Sleep(kLockRetryMs);
        }
        const qint64 lockedMs = clock.elapsed();
        const QString timings = QStringLiteral("moved in %1 ms, on the console after %2 ms, "
                                               "locked after %3 ms in %4 attempt(s) (%5)")
                                    .arg(movedMs)
                                    .arg(arrivedMs)
                                    .arg(lockedMs)
                                    .arg(attempts)
                                    .arg(before);
        if (locked) {
            finish(Outcome::Back, timings);
            return;
        }
        // Never left unlocked on the PC's screen: back to where it was.
        const QString hidden = hideAgain();
        finish(hidden.isEmpty() ? Outcome::Hidden : Outcome::Exposed,
               QStringLiteral("could not be locked (last error %1, %2); %3")
                   .arg(lockError)
                   .arg(timings)
                   .arg(hidden.isEmpty() ? QStringLiteral("disconnected again") : hidden));
    }).detach();
#endif
}

void RemoteDesktopReturn::returnFinished(Outcome outcome, const QString& detail)
{
    m_Asking.store(false);
    QString error;
    switch (outcome) {
    case Outcome::Back: Logger::info("[rdp] back on this PC's screen, locked: " + detail); break;
    case Outcome::Refused:
        Logger::warning("[rdp] the session stays away from the screen: " + detail);
        error =
            QStringLiteral("This PC's session could not be brought back to its screen: ") + detail;
        break;
    case Outcome::Hidden:
        Logger::warning("[rdp] the session went back to the screen but was sent away again, not "
                        "brought back again until it is used: " +
                        detail);
        error = QStringLiteral("This PC's session could not be locked on its screen, so it was "
                               "sent away again: sign in on the PC");
        m_Tracker.giveUp();
        break;
    case Outcome::Exposed:
        Logger::error("[rdp] the session may be UNLOCKED on this PC's screen: " + detail);
        error = QStringLiteral("This PC's session could not be locked, and may be UNLOCKED on "
                               "its screen");
        m_Tracker.giveUp();
        break;
    }
    // What the session is now, before anyone waiting reads it: back on the
    // screen, the card has its displays again and a stream can open one.
    onTick();
    const QList<std::function<void(bool, const QString&)>> waiting = std::exchange(m_Waiting, {});
    for (const auto& done : waiting)
        done(outcome == Outcome::Back, error);
}
