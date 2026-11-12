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

#include "backend/VirtualDisplayJob.h"

#include "backend/streambackend/NativeProbeService.h"
#include "common/Logger.h"
#include "mw/native/VirtualDisplay.h"
#include "server/AppSettings.h"
#include "streaming/ConsoleSession.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>

#ifdef Q_OS_WIN
#include "common/DetachedProcess.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr int kHelperTimeoutMs = 2 * 60 * 1000;
constexpr int kPollIntervalMs = 250;
/// How long a start waits for the elevated task's last run to end, and how
/// often it looks (VirtualDisplayJob::runTask).
constexpr int kTaskBusyMs = 10 * 1000;
constexpr int kTaskBusyPollMs = 50;
/// How long the OS may take to bring an in-process display online.
constexpr int kOnlineTimeoutMs = 15 * 1000;
/// The grace between the last stream's end and the display going off: long
/// enough for a reload or a quality switch to come back, short enough that
/// the desktop is not left on a screen nobody looks at.
constexpr int kReleaseGraceMs = 4 * 1000;

using Action = VirtualDisplay::Request::Action;

const char* stateName(VirtualDisplayJob::State s)
{
    switch (s) {
    case VirtualDisplayJob::State::Idle: return "idle";
    case VirtualDisplayJob::State::Elevating: return "elevating";
    case VirtualDisplayJob::State::Applying: return "applying";
    case VirtualDisplayJob::State::Configuring: return "configuring";
    case VirtualDisplayJob::State::Refreshing: return "refreshing";
    case VirtualDisplayJob::State::Done: return "done";
    case VirtualDisplayJob::State::Failed: return "failed";
    }
    return "idle";
}

/// The last run's verdict taken away before the next run starts, so that a
/// poll can only read the verdict of the run it waits for. False when the
/// file stays (still open somewhere): no run may start then, or it would be
/// answered by its predecessor.
bool discardResult()
{
    const QString path = VirtualDisplay::resultPath();
    if (QFile::remove(path) || !QFile::exists(path)) return true;
    Logger::warning(QStringLiteral("[vdisplay] %1 cannot be removed — the next run's verdict "
                                   "could not be told from the last one's")
                        .arg(path));
    return false;
}

/// The helper's verdict is its last stdout line; anything before it is noise.
QByteArray lastLine(const QByteArray& out)
{
    const QList<QByteArray> lines = out.trimmed().split('\n');
    return lines.isEmpty() ? QByteArray() : lines.last().trimmed();
}

} // namespace

VirtualDisplayJob& VirtualDisplayJob::instance()
{
    static VirtualDisplayJob job;
    return job;
}

VirtualDisplayJob::VirtualDisplayJob(QObject* parent)
    : QObject(parent)
{
    m_Poll.setInterval(kPollIntervalMs);
    connect(&m_Poll, &QTimer::timeout, this, &VirtualDisplayJob::pollResult);
    m_Deadline.setSingleShot(true);
    connect(&m_Deadline, &QTimer::timeout, this, [this]() {
        m_Poll.stop();
        if (m_InProcessResult) {
            m_InProcessResult.reset();
            mw::native::vdisplay::destroy();
            fail(QStringLiteral("The virtual display did not come online"));
            return;
        }
        fail(QStringLiteral("The elevated helper did not answer in time"));
    });
    m_Release.setSingleShot(true);
    connect(&m_Release, &QTimer::timeout, this, [this]() {
        // A take-over on the tile (another device) asks for the display —
        // cancelling the grace — BEFORE the stream it replaces is torn down,
        // and that teardown starts the grace again: without this, the display
        // went off four seconds into the new stream.
        if (m_InUse && m_InUse()) {
            Logger::info(QStringLiteral("[vdisplay] still streamed — stays on"));
            if (othersDueBack()) enqueue(Action::OthersBack, 0, 0, 0, false, false, nullptr);
            return;
        }
        Logger::info(QStringLiteral("[vdisplay] no stream left on the virtual display"));
        if (m_BeforeRelease && m_BeforeRelease()) {
            // What was opened there is closing: off once it is gone, unless
            // a stream came back in the meantime.
            QTimer::singleShot(kCloseSettleMs, this, [this]() {
                if (m_InUse && m_InUse()) return;
                deactivate(nullptr);
            });
            return;
        }
        deactivate(nullptr);
    });
}

bool VirtualDisplayJob::othersDueBack() const
{
    if (!m_OwnerOn) return false;
    return VirtualDisplay::othersComeBack(m_OwnerOn(),
                                          QFile::exists(VirtualDisplay::othersOffPath()));
}

bool VirtualDisplayJob::running() const
{
    return m_State != State::Idle && m_State != State::Done && m_State != State::Failed;
}

void VirtualDisplayJob::setState(State s)
{
    m_State = s;
    Logger::info(QStringLiteral("[vdisplay] %1").arg(QLatin1String(stateName(s))));
}

QJsonObject VirtualDisplayJob::statusJson() const
{
    QJsonObject obj;
    obj["state"] = QLatin1String(stateName(m_State));
    obj["action"] = VirtualDisplay::toString(m_Request.action);
    if (m_StartedAt.isValid()) obj["started_at"] = m_StartedAt.toString(Qt::ISODate);
    if (m_FinishedAt.isValid()) obj["finished_at"] = m_FinishedAt.toString(Qt::ISODate);
    if (m_State == State::Failed) obj["error"] = m_Error;
    if (m_State == State::Done && !m_Display.isEmpty()) obj["display"] = m_Display;
    return obj;
}

// ── Entry points ────────────────────────────────────────────────────────────

void VirtualDisplayJob::activate(int width, int height, int refreshMilliHz, bool hdr,
                                 bool othersOff, Callback cb)
{
    activateAs(VirtualDisplay::Asker::Owner, width, height, refreshMilliHz, hdr, othersOff,
               std::move(cb));
}

void VirtualDisplayJob::activateAs(VirtualDisplay::Asker asker, int width, int height,
                                   int refreshMilliHz, bool hdr, bool othersOff, Callback cb)
{
    m_Release.stop();
    // Linux: the stream's own portal session makes the display, at the
    // client's size, when the worker starts. Nothing to do first.
    if (VirtualDisplay::livesInStream()) {
        if (cb) cb(true, QString());
        return;
    }
    VirtualDisplay::normaliseMode(width, height);
    int refresh = refreshMilliHz;
    VirtualDisplay::normaliseRate(refresh);
    // Already on and seen by the engine: nothing to wait for. (A running
    // operation is asked first — its outcome is what the caller wants.)
    if (!running() && m_Queue.isEmpty()) {
        const VirtualDisplay::Status st = VirtualDisplay::probe();
        if (!st.supported) {
            if (cb)
                cb(false, QStringLiteral("Virtual displays are not supported on this platform"));
            return;
        }
        if (!st.installed) {
            if (cb)
                cb(false, QStringLiteral("\"%1\" is not installed on this machine")
                              .arg(VirtualDisplay::displayName()));
            return;
        }
        // On already — and, when a size or a rate was asked for, on in that
        // mode. The mode this process last applied is the only one it can
        // vouch for: after a restart it knows none, and the operation runs
        // again.
        if (st.active && (width == 0 || (width == m_ActiveWidth && height == m_ActiveHeight)) &&
            (refresh == 0 || refresh == m_ActiveRefresh) && hdr == m_ActiveHdr) {
            if (cb) cb(true, QString());
            return;
        }
    }
    enqueue(Action::Activate, width, height, refresh, hdr,
            VirtualDisplay::othersGoOff(asker, othersOff, m_GuestOn && m_GuestOn(),
                                        QFile::exists(VirtualDisplay::othersOffPath())),
            std::move(cb));
}

void VirtualDisplayJob::activateIfOff(int width, int height, int refreshMilliHz, Callback cb)
{
    m_Release.stop();
    if (VirtualDisplay::livesInStream()) {
        if (cb) cb(true, QString());
        return;
    }
    // On its way on, in whatever mode, or on with the other displays coming
    // back: that operation's outcome is the answer. (One on its way off is
    // followed by this one.)
    const auto keepsItOn = [](Action action) {
        return action == Action::Activate || action == Action::OthersBack;
    };
    const bool lastKeepsItOn = m_Queue.isEmpty() ? running() && keepsItOn(m_Request.action)
                                                 : keepsItOn(m_Queue.last().action);
    if (lastKeepsItOn) {
        if (!cb) return;
        if (m_Queue.isEmpty()) {
            m_Callbacks.append(std::move(cb));
            return;
        }
        Callback prev = m_Queue.last().cb;
        m_Queue.last().cb = [prev, cb](bool ok, const QString& err) {
            if (prev) prev(ok, err);
            cb(ok, err);
        };
        return;
    }
    if (!running() && m_Queue.isEmpty() && VirtualDisplay::probe().active) {
        // Arrived during the grace of an owner who left it exclusive: that grace
        // was just cancelled, and nothing else would bring the others back.
        if (othersDueBack()) {
            enqueue(Action::OthersBack, 0, 0, 0, false, false, std::move(cb));
            return;
        }
        if (cb) cb(true, QString());
        return;
    }
    activateAs(VirtualDisplay::Asker::Guest, width, height, refreshMilliHz, false, false,
               std::move(cb));
}

void VirtualDisplayJob::deactivate(Callback cb)
{
    m_Release.stop();
    if (!running() && m_Queue.isEmpty()) {
        const VirtualDisplay::Status st = VirtualDisplay::probe();
        if (!st.supported || !st.installed || !st.enabled) {
            AppSettings().clearVirtualDisplay();
            if (cb) cb(true, QString());
            return;
        }
    }
    enqueue(Action::Deactivate, 0, 0, 0, false, false, std::move(cb));
}

void VirtualDisplayJob::releaseSoon(int graceMs)
{
    // Linux: it went with the stream's portal session — what was opened on
    // it is closed now, with nothing left to turn off.
    if (VirtualDisplay::livesInStream()) {
        if (m_BeforeRelease && !(m_InUse && m_InUse())) m_BeforeRelease();
        return;
    }
    const int ms = qMax(graceMs, kReleaseGraceMs);
    if (m_Release.isActive() && m_Release.remainingTime() > ms) return;
    m_Release.start(ms);
}

void VirtualDisplayJob::enqueue(Action action, int width, int height, int refreshMilliHz, bool hdr,
                                bool exclusive, Callback cb)
{
    // The same verb AND the same mode as the running or last queued
    // operation: join it. A different mode is a different operation, and it
    // waits its turn rather than riding on one that will not deliver it.
    // While a mode falls back, the caller's own one is what it is running.
    // A join keeps the operation's exclusive choice, so one that leaves the
    // other displays on (a guest's) never starts turning them off.
    const VirtualDisplay::Request& runningAsked =
        m_AskedBeforeFallback ? *m_AskedBeforeFallback : m_Request;
    if (running() && runningAsked.action == action && runningAsked.width == width &&
        runningAsked.height == height && runningAsked.refreshMilliHz == refreshMilliHz &&
        runningAsked.hdr == hdr && m_Queue.isEmpty()) {
        if (cb) m_Callbacks.append(std::move(cb));
        return;
    }
    if (!m_Queue.isEmpty() && m_Queue.last().action == action && m_Queue.last().width == width &&
        m_Queue.last().height == height && m_Queue.last().refreshMilliHz == refreshMilliHz &&
        m_Queue.last().hdr == hdr) {
        if (cb) {
            Callback prev = m_Queue.last().cb;
            m_Queue.last().cb = [prev, cb](bool ok, const QString& err) {
                if (prev) prev(ok, err);
                cb(ok, err);
            };
        }
        return;
    }
    m_Queue.append(Pending{action, width, height, refreshMilliHz, hdr, exclusive, std::move(cb)});
    if (!running()) startNext();
}

void VirtualDisplayJob::startNext()
{
    if (m_Queue.isEmpty()) return;
    Pending next = m_Queue.takeFirst();
    m_Request = VirtualDisplay::Request{};
    m_Request.action = next.action;
    m_Request.width = next.width;
    m_Request.height = next.height;
    m_Request.refreshMilliHz = next.refreshMilliHz;
    m_Request.hdr = next.hdr;
    // Bench only (MW_BENCH_VD_HDR=1): a stream that asked for HDR keeps the
    // Windows display in HDR. See Request::benchHdr.
    m_Request.benchHdr = next.action == Action::Activate && next.hdr &&
                         qEnvironmentVariableIntValue("MW_BENCH_VD_HDR") == 1;
    // Bench only (MW_BENCH_VD_SECONDARY=1): the primary display keeps its
    // role. See Request::benchSecondary.
    m_Request.benchSecondary = next.action == Action::Activate &&
                               qEnvironmentVariableIntValue("MW_BENCH_VD_SECONDARY") == 1;
    m_Request.exclusive = next.exclusive;
    // The GPU the owner chose on the display's card, read as each activation
    // starts: the next stream follows a change.
    if (next.action == Action::Activate) m_Request.gpu = AppSettings().virtualDisplayGpu();
    m_Callbacks.clear();
    if (next.cb) m_Callbacks.append(std::move(next.cb));
    m_Error.clear();
    m_Display.clear();
    m_StartedAt = QDateTime::currentDateTimeUtc();
    m_FinishedAt = QDateTime();
    m_HelperOut.clear();
    m_NextStages.clear();
    m_InProcessResult.reset();
    m_AskedBeforeFallback.reset();

    const VirtualDisplay::Status st = VirtualDisplay::probe();
    if (!st.supported) {
        fail(QStringLiteral("Virtual displays are not supported on this platform"));
        return;
    }
    if (!st.canManage) {
        fail(QStringLiteral("No way to elevate on this install — the installer's task is missing"));
        return;
    }
    if (m_Request.action == Action::Deactivate) {
        // The display to give the primary role back to, as Activate saw it.
        m_Request.restorePrimary =
            AppSettings().virtualDisplay().value(QLatin1String("previous_primary")).toString();
    }
    if (!QDir().mkpath(VirtualDisplay::stagingDir())) {
        fail(QStringLiteral("Cannot create the staging directory"));
        return;
    }
    // Leftovers of an interrupted attempt would be acted on as-is otherwise.
    if (!discardResult()) {
        fail(QStringLiteral("The virtual display's last verdict cannot be removed"));
        return;
    }
    QFile::remove(VirtualDisplay::requestPath());
    dispatch();
}

void VirtualDisplayJob::settle(bool ok, const QString& error)
{
    const QList<Callback> callbacks = m_Callbacks;
    m_Callbacks.clear();
    for (const Callback& cb : callbacks)
        if (cb) cb(ok, error);
    startNext();
}

void VirtualDisplayJob::fail(const QString& error)
{
    // A display that did not come up at the client's mode is asked again
    // closer to the default one, which always comes up: the stream then runs
    // at that mode instead of failing to launch. The helper has already
    // switched the node back off and taken the client's mode out of the list.
    // Its verdict goes first: the next run must be answered by its own.
    if (const auto next = VirtualDisplay::fallbackTowardsDefault(m_Request, error);
        next && discardResult()) {
        if (!m_AskedBeforeFallback) m_AskedBeforeFallback = m_Request;
        Logger::warning(QStringLiteral("[vdisplay] %1x%2@%3 did not come up — trying %4x%5@%6")
                            .arg(m_Request.width)
                            .arg(m_Request.height)
                            .arg(VirtualDisplay::rateText(m_Request.refreshMilliHz))
                            .arg(next->width)
                            .arg(next->height)
                            .arg(VirtualDisplay::rateText(next->refreshMilliHz)));
        m_Poll.stop();
        m_Deadline.stop();
        m_Request = *next;
        m_HelperOut.clear();
        m_NextStages.clear();
        QFile::remove(VirtualDisplay::requestPath());
        dispatch();
        return;
    }
    m_Error = error;
    m_FinishedAt = QDateTime::currentDateTimeUtc();
    m_Poll.stop();
    m_Deadline.stop();
    QFile::remove(VirtualDisplay::requestPath());
    Logger::warning(QStringLiteral("[vdisplay] %1 failed: %2")
                        .arg(VirtualDisplay::toString(m_Request.action), error));
    setState(State::Failed);
    // The other displays staying off takes nothing from a guest's stream: the
    // display is on, and whoever waited on this goes ahead.
    settle(m_Request.action == Action::OthersBack, error);
}

void VirtualDisplayJob::succeed(const VirtualDisplay::Result& res)
{
    m_Display = res.display;
    m_FinishedAt = QDateTime::currentDateTimeUtc();
    QFile::remove(VirtualDisplay::requestPath());
    AppSettings settings;
    if (m_Request.action == Action::Activate) {
        QJsonObject rec;
        rec["active"] = true;
        rec["previous_primary"] = res.previousPrimary;
        rec["activated_at"] = m_FinishedAt.toString(Qt::ISODate);
        if (m_Request.width > 0) {
            rec["width"] = m_Request.width;
            rec["height"] = m_Request.height;
        }
        if (m_Request.refreshMilliHz > 0) rec["refresh_mhz"] = m_Request.refreshMilliHz;
        settings.setVirtualDisplay(rec);
        // After a fallback, the mode the caller asked for is remembered as
        // answered: the next launch asking it again joins the display as it
        // is, instead of waiting on the same refusal before the same fallback.
        const VirtualDisplay::Request& asked =
            m_AskedBeforeFallback ? *m_AskedBeforeFallback : m_Request;
        m_ActiveWidth = asked.width;
        m_ActiveHeight = asked.height;
        m_ActiveRefresh = asked.refreshMilliHz;
        m_ActiveHdr = asked.hdr;
        if (m_Request.exclusive) startGuard();
    } else if (m_Request.action == Action::Deactivate) {
        settings.clearVirtualDisplay();
        m_ActiveWidth = m_ActiveHeight = m_ActiveRefresh = 0;
        m_ActiveHdr = false;
    }
    setState(State::Done);
    settle(true, QString());
}

void VirtualDisplayJob::startGuard()
{
#ifdef Q_OS_WIN
    // A service has no desktop to put a layout back on, and its supervisor
    // restarts it; a server in the user's session may simply die.
    if (!qEnvironmentVariableIsEmpty("MW_SERVICE")) return;
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return;
    const quint64 created = (quint64(c.dwHighDateTime) << 32) | quint64(c.dwLowDateTime);
    const QString server = QStringLiteral("%1:%2").arg(GetCurrentProcessId()).arg(created);
    if (!mw::startDetached(QCoreApplication::applicationFilePath(),
                           {VirtualDisplay::guardArgument(), server}))
        Logger::warning(QStringLiteral("[vdisplay] the guard of the other displays did not start"));
#endif
}

// ── Reaching the helper ─────────────────────────────────────────────────────

void VirtualDisplayJob::dispatch()
{
    QFile req(VirtualDisplay::requestPath());
    if (!req.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("Cannot write the request file"));
        return;
    }
    req.write(VirtualDisplay::toJson(m_Request));
    req.close();

    const QString method = VirtualDisplay::probe().method;
    if (method == QLatin1String("inprocess")) {
        applyInProcess();
        return;
    }

    setState(State::Elevating);
    const QStringList dirArg = {QStringLiteral("--vdisplay-dir"),
                                QDir::toNativeSeparators(VirtualDisplay::stagingDir())};

    if (method == QLatin1String("task")) {
        runTask();
    } else if (method == QLatin1String("elevated")) {
        if (!qEnvironmentVariableIsEmpty("MW_SERVICE")) {
            // Session 0 can switch the device node but has no desktop to read
            // or set a layout on: those halves run in the console session,
            // one child per stage, in the order VirtualDisplayApply::run
            // would take them in one process.
            switch (m_Request.action) {
            case Action::Activate:
                m_NextStages = {QStringLiteral("snapshot"), QStringLiteral("driver"),
                                QStringLiteral("mode")};
                break;
            case Action::OthersBack: m_NextStages = {QStringLiteral("mode")}; break;
            case Action::Deactivate:
                m_NextStages = {QStringLiteral("mode"), QStringLiteral("driver")};
                break;
            default: m_NextStages = {QStringLiteral("driver"), QStringLiteral("mode")}; break;
            }
            const QString firstStage = m_NextStages.takeFirst();
            runHelper(QStringList{VirtualDisplay::applyArgument(),
                                  QStringLiteral("--stage=") + firstStage}
                          << dirArg,
                      firstStage != QLatin1String("driver"));
        } else {
            runHelper(QStringList{VirtualDisplay::applyArgument(), QStringLiteral("--stage=all")}
                          << dirArg,
                      false);
        }
    } else {
        fail(QStringLiteral("No way to elevate on this install"));
    }
}

void VirtualDisplayJob::applyInProcess()
{
    // macOS: the display is an object of this process (mw::native::vdisplay).
    // No helper, no file: the request is applied here and now, and the only
    // wait is for the OS to bring the display online — polled, not slept —
    // after which it takes the main display's role.
    setState(State::Configuring);
    VirtualDisplay::Result res;
    if (!VirtualDisplay::applyInProcess(m_Request, &res)) {
        handleResult(res);
        return;
    }
    if (m_Request.action != Action::Activate || mw::native::vdisplay::isOnline()) {
        if (m_Request.action == Action::Activate) makeMain();
        handleResult(res);
        return;
    }
    m_InProcessResult = res;
    m_Deadline.start(kOnlineTimeoutMs);
    m_Poll.start();
}

void VirtualDisplayJob::makeMain()
{
    // macOS: the created display takes the main display's role (menu bar,
    // where new windows open) for the stream. Logged here, in the server's
    // log: the native module's own lines do not reach it from this process.
    std::string error;
    if (mw::native::vdisplay::setMain(mw::native::vdisplay::displayId(), &error))
        Logger::info(QStringLiteral("[vdisplay] the virtual display is now the main display"));
    else
        Logger::warning(QStringLiteral("[vdisplay] not made the main display: %1")
                            .arg(QString::fromStdString(error)));
}

void VirtualDisplayJob::runTask()
{
    // The installer's trigger-less elevated task: started by name, it runs our
    // exe with --vdisplay-apply as this user, elevated, hidden. Its result
    // reaches us through the result file — there is no stdout to read.
    //
    // The helper writes that file and only then exits, and the task ignores a
    // start while its last run is still going: a run asked for in that gap —
    // the next mode of a client whose screen changed twice, a fallback — was
    // lost, schtasks said it had started it, and the job waited two minutes
    // for nothing (15/10/2026, a black screen on the client). The last run is
    // let go first; a run still going after kTaskBusyMs is no longer one
    // ending, and the start is tried all the same.
    if (VirtualDisplay::elevatedTaskRunning()) {
        if (!m_TaskBusySince.isValid()) {
            m_TaskBusySince.start();
            Logger::info(QStringLiteral("[vdisplay] the task's last run is still ending — "
                                        "waiting for it"));
        }
        if (m_TaskBusySince.elapsed() < kTaskBusyMs) {
            QTimer::singleShot(kTaskBusyPollMs, this, [this]() { runTask(); });
            return;
        }
        Logger::warning(QStringLiteral("[vdisplay] the task still runs after %1 ms — started "
                                       "anyway")
                            .arg(kTaskBusyMs));
    }
    m_TaskBusySince.invalidate();
    QProcess* proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        if (status != QProcess::NormalExit || code != 0) {
            fail(QStringLiteral("schtasks could not start \"%1\" (exit %2)")
                     .arg(VirtualDisplay::taskName())
                     .arg(code));
            return;
        }
        setState(State::Applying);
        m_Deadline.start(kHelperTimeoutMs);
        m_Poll.start();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart) return;
        proc->deleteLater();
        fail(QStringLiteral("schtasks.exe could not be started"));
    });
    proc->start(QStringLiteral("schtasks.exe"),
                {QStringLiteral("/Run"), QStringLiteral("/TN"), VirtualDisplay::taskName()});
}

void VirtualDisplayJob::runHelper(const QStringList& args, bool inConsoleSession)
{
    setState(inConsoleSession ? State::Configuring : State::Applying);
    m_HelperOut.clear();
    const QString exe = QCoreApplication::applicationFilePath();

    if (inConsoleSession) {
        ConsoleProcess* cp = new ConsoleProcess(this);
        connect(cp, &ConsoleProcess::stdoutData, this,
                [this](const QByteArray& d) { m_HelperOut += d; });
        connect(cp, &ConsoleProcess::finished, this, [this, cp](int code, bool crashed) {
            cp->deleteLater();
            m_Deadline.stop();
            const auto res = VirtualDisplay::parseResult(lastLine(m_HelperOut));
            if (crashed || !res) {
                fail(QStringLiteral("The console-session helper ended without a result (exit %1)")
                         .arg(code));
                return;
            }
            handleResult(*res);
        });
        QString error;
        if (!cp->start(exe, args, &error)) {
            cp->deleteLater();
            fail(
                QStringLiteral("Could not start the helper in the console session: %1").arg(error));
            return;
        }
        m_Deadline.start(kHelperTimeoutMs);
        return;
    }

    QProcess* proc = new QProcess(this);
    connect(proc, &QProcess::readyReadStandardOutput, this,
            [this, proc]() { m_HelperOut += proc->readAllStandardOutput(); });
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        m_Deadline.stop();
        m_HelperOut += proc->readAllStandardOutput();
        const auto res = VirtualDisplay::parseResult(lastLine(m_HelperOut));
        if (status != QProcess::NormalExit || !res) {
            fail(QStringLiteral("The elevated helper ended without a result (exit %1)").arg(code));
            return;
        }
        handleResult(*res);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart) return;
        proc->deleteLater();
        fail(QStringLiteral("The helper could not be started"));
    });
    m_Deadline.start(kHelperTimeoutMs);
    proc->start(exe, args);
}

void VirtualDisplayJob::pollResult()
{
    if (m_InProcessResult) {
        if (!mw::native::vdisplay::isOnline()) return;
        m_Poll.stop();
        m_Deadline.stop();
        const VirtualDisplay::Result res = *m_InProcessResult;
        m_InProcessResult.reset();
        makeMain();
        handleResult(res);
        return;
    }
    // Read and closed before it is acted on: the fallback deletes it.
    const auto res = VirtualDisplay::readResult(VirtualDisplay::resultPath());
    if (!res) return; // not there yet, or still being written
    m_Poll.stop();
    m_Deadline.stop();
    handleResult(*res);
}

void VirtualDisplayJob::handleResult(const VirtualDisplay::Result& res)
{
    QFile::remove(VirtualDisplay::resultPath());
    if (!res.ok) {
        fail(res.error.isEmpty() ? QStringLiteral("The helper failed at stage %1").arg(res.stage)
                                 : res.error);
        return;
    }
    if (!m_NextStages.isEmpty()) {
        // One stage done; the next runs where it can — the desktop halves
        // as the console user, the node half as SYSTEM.
        const QString stage = m_NextStages.takeFirst();
        runHelper({VirtualDisplay::applyArgument(), QStringLiteral("--stage=") + stage,
                   QStringLiteral("--vdisplay-dir"),
                   QDir::toNativeSeparators(VirtualDisplay::stagingDir())},
                  stage != QLatin1String("driver"));
        return;
    }
    setState(State::Refreshing);
    // The display list moved under the engine: ask again. As a service this
    // spawns the console probe; on a desktop it is a direct call. Either way
    // NativeProbeService::changed() carries the news to ComputerManager,
    // which rebuilds the native host card.
    NativeProbeService::instance().refresh();
    succeed(res);
}
