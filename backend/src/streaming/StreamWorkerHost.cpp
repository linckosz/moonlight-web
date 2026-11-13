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

#include "StreamWorkerHost.h"
#include "CaptureKey.h"
#include "ConsoleSession.h"
#include "HidParkingTransfer.h"
#include "IceMuxHandles.h"
#include "NativeBench.h"
#include "StreamEnds.h"
#include "WorkerService.h"
#include "common/Edition.h"
#include "common/LaunchLock.h"
#include "common/LinuxCapabilities.h"
#include "common/RunFlags.h"
#include "mw/native/MouseKeys.h"
#include "network/IceMux.h"
#include "server/AppSettings.h"

#include <QCoreApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTimer>
#include <QDebug>

#include <mutex>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

StreamWorkerHost::StreamWorkerHost(QObject* parent)
    : QObject(parent)
{}

StreamWorkerHost::~StreamWorkerHost()
{
    dropMuxAgent();
    if (m_Proc && m_Proc->state() != QProcess::NotRunning) {
        // Destructor teardown: no time for the graceful dance — kill. Normal
        // paths call requestQuit()/notify*() well before destruction.
        m_Proc->kill();
        m_Proc->waitForFinished(1000);
    }
    // A console child is killed and joined by its own destructor (it is our
    // QObject child), so nothing more is needed here.
    // The worker's copies of the shared sockets go with its process: the mux
    // need not keep a replaced port for it any longer.
    releaseMuxSockets();
    if (m_HidBatch) HidParkingTransfer::settle(m_HidBatch, false);
}

void StreamWorkerHost::prepare(const QJsonObject& config, QStringList& args,
                               QByteArray& configLine) const
{
    args = QStringList{QStringLiteral("--stream-worker")};
    // Carry --dev into the child. The flag drives the application name, which
    // is what moves the settings, logs and — the part that matters here — the
    // client identity the worker presents to the host. Without it a dev run
    // would launch streams as the installed instance. (The DEV build needs
    // nothing here: the child is the same binary, its edition compiled in.)
    if (mw::edition::devFlag()) args << QStringLiteral("--dev");
    // --config travels in the config line, not in the arguments: the service
    // and the elevated task below start the worker with fixed ones.
    QJsonObject withSettings = config;
    if (!AppSettings::fileOverride().isEmpty())
        withSettings[QStringLiteral("settingsFile")] = AppSettings::fileOverride();
#if defined(Q_OS_LINUX)
    // A native worker the package's launcher starts has its home fixed
    // (startInProcess()), where it would find a settings file of nobody's: it
    // is told which one this process reads.
    else if (config[QStringLiteral("backendType")].toString() == QLatin1String("native") &&
             !mw::workerLauncher().isEmpty())
        withSettings[QStringLiteral("settingsFile")] = AppSettings().filePath();
#endif
    // Same road for verbose logs: this process's DEBUG threshold is the one
    // the worker's own lines have to pass before they are relayed here.
    if (mw::run::verbose()) withSettings[QStringLiteral("verbose")] = true;
    // And for the keyboard's settings: a worker started as SYSTEM reads the
    // settings.json of SYSTEM's profile, never the owner's, so it would type
    // by the default whatever the owner chose, and never log the keys.
    {
        const AppSettings settings;
        withSettings[QStringLiteral("keyboardLayoutFidelity")] = settings.keyboardLayoutFidelity();
        withSettings[QStringLiteral("keyboardHostLayout")] = settings.keyboardHostLayout();
        if (settings.keyboardDebug()) withSettings[QStringLiteral("keyboardDebug")] = true;
        // The admin's « Network priority » box, by the same trap: SYSTEM's
        // settings.json, which nobody writes, says marked. And the bench's
        // MW_DSCP, which a worker the service starts does not inherit.
        withSettings[QStringLiteral("packetMarking")] = settings.packetMarking();
        withSettings[QStringLiteral("iceTcpPassive")] = settings.iceTcpPassive();
        // The device the viewer's microphone plays into (settings.json
        // mic_device), by the same trap: SYSTEM's file would say auto.
        withSettings[QStringLiteral("micDevice")] = settings.micDevice();
        if (settings.debugIceDropUdp()) withSettings[QStringLiteral("debugIceDropUdp")] = true;
        const QString dscp = qEnvironmentVariable("MW_DSCP");
        if (!dscp.isEmpty()) withSettings[QStringLiteral("dscp")] = dscp;
    }
    configLine = QJsonDocument(withSettings).toJson(QJsonDocument::Compact) + "\n";
}

bool StreamWorkerHost::startAs(const QJsonObject& config, Launch how)
{
    Q_ASSERT(!m_Proc && !m_Console);
    QStringList args;
    QByteArray configLine;
    prepare(config, args, configLine);
    const bool native = config["backendType"].toString() == QLatin1String("native");
    // A config of the sign-in screen goes nowhere else (start()).
    if (config[QStringLiteral("signInScreen")].toBool(false) != (how == Launch::SignInScreen))
        return false;
    switch (how) {
    case Launch::ConsoleAsUser: return startInConsoleSession(args, configLine, how);
    case Launch::ServiceAsSystem:
        return WorkerService::available() && startInConsoleSession(args, configLine, how);
    case Launch::SignInScreen:
        return WorkerService::available() && startInConsoleSession(args, configLine, how);
    case Launch::TaskElevated:
        return ConsoleSession::elevatedWorkerAvailable() &&
               startInConsoleSession(args, configLine, how);
    case Launch::Plain: break;
    }
    return startInProcess(args, configLine, native);
}

bool StreamWorkerHost::getsSharedPorts(const QJsonObject& config)
{
    if (config[QStringLiteral("role")].toString() == QLatin1String("feed")) return false;
    // In start()'s order. The sign-in screen's worker comes through the
    // launcher service or not at all, whatever this process is.
    const bool signInScreen = config[QStringLiteral("signInScreen")].toBool(false);
    // The one launch start() and startAs() make as Launch::ConsoleAsUser:
    // shared only with the account this installation was made by.
    const bool native = config[QStringLiteral("backendType")].toString() == QLatin1String("native");
    const bool consoleAsUser = !signInScreen && native && ConsoleSession::launchesElsewhere();
    return sharesPortsFor(false, signInScreen, consoleAsUser,
                          consoleAsUser && ConsoleSession::consoleUserIsOwner());
}

bool StreamWorkerHost::start(const QJsonObject& config)
{
    Q_ASSERT(!m_Proc && !m_Console);
    QStringList args;
    QByteArray configLine;
    prepare(config, args, configLine);

    // The native engine captures the desktop, and a service has none: its
    // worker goes to the console session, as the user sitting there. Every
    // other backend talks to a host over the network and runs fine from here.
    const bool native = config["backendType"].toString() == QLatin1String("native");
    // The console's sign-in screen (RemoteDesktopReturnPolicy): through the
    // launcher service, in its sign-in screen mode, or not at all. A worker
    // with less reach, in the session Remote Desktop left, would see no
    // display — and one without the mode is never what this stream gets.
    if (config[QStringLiteral("signInScreen")].toBool(false))
        return native && WorkerService::available() &&
               startInConsoleSession(args, configLine, Launch::SignInScreen);
    if (native && ConsoleSession::launchesElsewhere())
        return startInConsoleSession(args, configLine, Launch::ConsoleAsUser);
    // On the desktop, the native worker is raised as far as the machine lets
    // it, and every step down still streams:
    //
    //   the launcher service  SYSTEM on the console desktop — the secure
    //                         desktop too: UAC, the lock screen, Ctrl+Alt+Suppr
    //   the elevated task     the user's full token — every administrator
    //                         window, but not the secure desktop
    //   a plain child         what it always was
    //
    // A failed attempt costs this one launch, never the stream: it falls to the
    // next line, which loses reach and nothing else. Both detours also switch
    // themselves off for the rest of the run once they have failed once.
    //
    // capture=wgc (plan « Gels du lien », route B): Windows.Graphics.Capture
    // exists for no SYSTEM process, so the worker asked for it runs on the
    // user's token — the elevated task, else a plain child — and the launcher
    // service is skipped, the secure desktop with it (CaptureKey.h).
    const bool userToken =
        native && mw::capturekey::asksWgc(
                      effectiveTuningSpec(config["nativeTuning"].toString()).toStdString());
    if (userToken)
        qInfo() << "[StreamWorkerHost] capture=wgc: the worker runs on the user's token"
                   " (Windows.Graphics.Capture has none for SYSTEM), the secure desktop out of"
                   " its reach";
    // A TV on plain HTTP (issue #42): a plain child, on the user's token, and
    // never one of the two detours — the launcher service reaches the secure
    // desktop, the elevated task every administrator window. The worker also
    // keeps such a stream off the secure desktop by itself (plainHttp); this
    // keeps the reach out of its hands in the first place. In service mode the
    // console launch above, as the user, already applies.
    const bool plainHttp = config[QStringLiteral("plainHttp")].toBool(false);
    if (plainHttp)
        qInfo() << "[StreamWorkerHost] plain HTTP: the worker runs as a plain child, on the"
                   " user's token";
    if (native && !userToken && !plainHttp && WorkerService::available() &&
        startInConsoleSession(args, configLine, Launch::ServiceAsSystem))
        return true;
    if (native && !plainHttp && ConsoleSession::elevatedWorkerAvailable() &&
        startInConsoleSession(args, configLine, Launch::TaskElevated))
        return true;
    return startInProcess(args, configLine, native);
}

bool StreamWorkerHost::startInProcess(const QStringList& args, const QByteArray& configLine,
                                      bool native)
{
    m_Proc = new QProcess(this);
    m_Proc->setProgram(QCoreApplication::applicationFilePath());
    m_Proc->setArguments(args);
#if defined(Q_OS_LINUX)
    // A GPU context above normal priority needs CAP_SYS_NICE, and this process
    // does not hold it: the package's launcher hands it to the native worker it
    // starts, with --stream-worker first (common/LinuxCapabilities.h); KMS
    // capture asks moonlightweb-kms instead. That worker starts with its home
    // fixed, so it is told where this process keeps its logs; its settings file
    // comes in the config line (prepare()).
    if (const QString launcher = native ? mw::workerLauncher() : QString(); !launcher.isEmpty()) {
        m_Proc->setProgram(launcher);
        m_Proc->setArguments(QStringList(args)
                             << QStringLiteral("--worker-data-dir")
                             << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    }
#else
    Q_UNUSED(native);
#endif
    // stdout carries the JSON event protocol; worker stderr joins ours so a
    // crashing worker still leaves a trace in the parent console/journal.
    m_Proc->setProcessChannelMode(QProcess::SeparateChannels);
    m_Proc->setReadChannel(QProcess::StandardOutput);

    connect(m_Proc, &QProcess::readyReadStandardOutput, this, &StreamWorkerHost::onStdout);
    connect(m_Proc, &QProcess::readyReadStandardError, this,
            [this]() { onStderrData(m_Proc->readAllStandardError()); });
    connect(m_Proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &StreamWorkerHost::onFinished);

#ifndef Q_OS_WIN
    // The UDP ports every stream shares go to this child by being left open
    // across its start, and across the launcher's own when it comes through
    // one: taken for it now, named in its config line, and let through in the
    // child alone, between its fork and its exec. Every other process this
    // one starts still gets none. A child that gets none binds a port of its
    // own, as without the mux.
    std::vector<int> acrossStart;
    const QByteArray line = withSharedSockets(configLine, Launch::Plain, false, 0, &acrossStart);
    if (!acrossStart.empty())
        m_Proc->setChildProcessModifier([acrossStart]() {
            mw::icemuxhandles::leaveOpenAcrossExec(acrossStart.data(), acrossStart.size());
        });
#endif

    {
        // Under the lock a stream worker is started under: nothing that must
        // not reach it is being made on another thread (common/LaunchLock.h)
        const std::lock_guard<std::mutex> launch(mw::launch::lock());
        m_Proc->start();
    }
    if (!m_Proc->waitForStarted(5000)) {
        qWarning() << "[StreamWorkerHost] Failed to spawn worker:" << m_Proc->errorString();
        // Nothing runs that could hold them: what was taken for it is given back
        releaseMuxSockets();
        m_Proc->deleteLater();
        m_Proc = nullptr;
        return false;
    }
#ifndef Q_OS_WIN
    // Started: the child's end of the channel is the child's alone
    if (m_MuxChannelWorkerEnd >= 0) {
        ::close(m_MuxChannelWorkerEnd);
        m_MuxChannelWorkerEnd = -1;
    }
#endif

#ifdef Q_OS_WIN
    // The UDP ports every stream shares, as this child alone can open them:
    // made for its pid, which exists only now. A child that gets none binds a
    // port of its own, as without the mux.
    m_Proc->write(withSharedSockets(configLine, Launch::Plain, false, m_Proc->processId()));
#else
    m_Proc->write(line);
#endif
    m_Launch = Launch::Plain;
    qInfo() << "[StreamWorkerHost] Worker spawned, pid=" << m_Proc->processId();
    return true;
}

// Whose process a worker is decides what it may be given: see muxHandoverFor().
// A socket in a process of another account is that account's to use.
static_assert(StreamWorkerHost::muxHandoverFor(StreamWorkerHost::Launch::ConsoleAsUser, false) ==
                  StreamWorkerHost::MuxHandover::None,
              "a console worker that is not the owner's must not get the shared sockets");
static_assert(StreamWorkerHost::muxHandoverFor(StreamWorkerHost::Launch::ConsoleAsUser, true) ==
                  StreamWorkerHost::MuxHandover::Handed,
              "a console worker on the owner's account is handed the shared sockets");
static_assert(StreamWorkerHost::muxHandoverFor(StreamWorkerHost::Launch::Plain, false) ==
                  StreamWorkerHost::MuxHandover::Handed,
              "a plain child is handed the shared sockets");
static_assert(StreamWorkerHost::muxHandoverFor(StreamWorkerHost::Launch::SignInScreen, false) ==
                  StreamWorkerHost::MuxHandover::Taken,
              "the sign-in screen's worker takes the shared sockets, as the service's other one");
static_assert(!StreamWorkerHost::tcpHandoverFor(StreamWorkerHost::Launch::ConsoleAsUser, false),
              "a console worker that is not the owner's must not be handed a connection");

QByteArray StreamWorkerHost::withSharedSockets(const QByteArray& configLine, Launch how,
                                               bool ownerAccount, qint64 pid,
                                               std::vector<int>* acrossStart)
{
    // Whatever an earlier launch of this worker was given is given back
    releaseMuxSockets();
    if (acrossStart) acrossStart->clear();
    IceMux* mux = IceMux::shared();
    if (!mux || !mux->running() || !mux->healthy()) return configLine;
    const MuxHandover handover = muxHandoverFor(how, ownerAccount);
    if (handover == MuxHandover::None) {
        // Said for each such worker, and without naming anybody.
        qInfo() << "[IceMux] shared ports: not this account — the worker binds a port of its own";
        return configLine;
    }
    const bool workerTakes = handover == MuxHandover::Taken;
    QJsonObject config = QJsonDocument::fromJson(configLine).object();
    // The guests' shared feed is an engine and a pipe: no browser reaches it.
    if (config[QStringLiteral("role")].toString() == QLatin1String("feed")) return configLine;

    std::string why;
#ifdef Q_OS_WIN
    Q_UNUSED(acrossStart);
    const IceMux::Share share =
        workerTakes ? mux->shareByHandle() : mux->shareWith(static_cast<unsigned long>(pid), &why);
    const bool usable = !share.sockets.empty() &&
                        !(workerTakes ? share.handles.empty() : share.descriptions.empty());
#else
    // A worker is this process's own child here and nothing else. The share
    // is taken before the child starts and kept from then on: none of its
    // sockets is closed, and so none of their numbers given to something
    // else, between this line and the child's exec.
    Q_UNUSED(pid);
    IceMux::Share share;
    std::vector<int> descriptors;
    if (workerTakes || !acrossStart) {
        why = "not a child this process is about to start";
    } else {
        share = mux->shareByHandle();
        descriptors = mw::icemuxhandles::descriptorsToHand(share.handles);
        if (descriptors.empty()) why = "no descriptor a worker can be started with";
    }
    const bool usable = !share.sockets.empty() && !descriptors.empty();
#endif
    if (!usable) {
        mux->release(share.lease);
        qWarning() << "[IceMux] the shared sockets cannot go to the worker:"
                   << QString::fromStdString(why) << "— it takes a socket of its own";
        return configLine;
    }
    QJsonArray ports;
    for (const uint16_t port : share.ports)
        ports.append(static_cast<int>(port));
    QJsonObject shared{{QStringLiteral("hop"), static_cast<int>(mux->hopPort())},
                       {QStringLiteral("ports"), ports}};
#ifndef Q_OS_WIN
    // The numbers the child finds them under: its own, once it has started,
    // are this process's (IceMuxHandles::inherited()).
    QJsonArray fds;
    for (const int fd : descriptors)
        fds.append(fd);
    shared[QStringLiteral("fds")] = fds;
    *acrossStart = descriptors;
#else
    if (workerTakes) {
        // The numbers of this process's own handles: the worker copies them
        // out itself, and checks what it got (streaming/IceMuxHandles).
        QJsonArray handles;
        for (const uint64_t handle : share.handles)
            handles.append(QString::number(handle));
        shared[QStringLiteral("handles")] = handles;
    } else {
        QJsonArray sockets;
        for (const auto& description : share.descriptions)
            sockets.append(
                QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(description.data()),
                                               static_cast<qsizetype>(description.size()))
                                        .toBase64()));
        shared[QStringLiteral("sockets")] = sockets;
    }
#endif
    // TCP on those ports: the server takes the connections of a stream that
    // listens in TCP, and hands each to this worker while it streams. Told to
    // a worker there is a way to hand them to, and to no other.
    m_MuxHandover = handover;
    if (config[QStringLiteral("iceMuxTcp")].toBool() && tcpHandoverFor(how, ownerAccount)) {
#ifdef Q_OS_WIN
        // A worker that takes copies each out of this process, as it does the
        // UDP sockets. One that is handed gets each described for its
        // process: held from now, while its launcher still holds it too, so
        // that what a description is made for is that process and no other.
        if (!workerTakes) m_MuxChild = IceMuxTcp::holdProcess(static_cast<unsigned long>(pid));
        m_MuxTcp = workerTakes || m_MuxChild != nullptr;
#else
        // Each crosses as one message of a channel made now, for this start
        // alone: its other end goes to the child with the sockets, and to no
        // other process this one starts.
        int mine = -1, its = -1;
        if (mw::icemuxhandles::makeChannel(&mine, &its)) {
            if (its > 2) {
                m_MuxChannel = mine;
                m_MuxChannelWorkerEnd = its;
                acrossStart->push_back(its);
                shared[QStringLiteral("channel")] = its;
                m_MuxTcp = true;
            } else {
                ::close(mine);
                ::close(its);
            }
        }
#endif
        if (m_MuxTcp) shared[QStringLiteral("tcp")] = true;
    }
    config[QStringLiteral("iceMux")] = shared;
    // Which of the mux's sockets those are: what its agents register with.
    // The mux keeps them for this worker until it is gone (onFinished()).
    for (const uint8_t id : share.sockets)
        m_MuxSockets.append(id);
    m_MuxLease = share.lease;
    return QJsonDocument(config).toJson(QJsonDocument::Compact) + "\n";
}

bool StreamWorkerHost::startInConsoleSession(const QStringList& args, const QByteArray& configLine,
                                             Launch how)
{
    m_Console = new ConsoleProcess(this);
    connect(m_Console, &ConsoleProcess::stdoutData, this, &StreamWorkerHost::onStdoutData);
    connect(m_Console, &ConsoleProcess::stderrData, this, &StreamWorkerHost::onStderrData);
    connect(m_Console, &ConsoleProcess::finished, this, &StreamWorkerHost::onChildFinished);

    QString error;
    const bool viaLauncher = how == Launch::ServiceAsSystem || how == Launch::TaskElevated ||
                             how == Launch::SignInScreen;
    if (viaLauncher) {
        // Neither a task nor a service can be handed our pipes, and neither
        // takes the arguments from here: both carry the fixed ones themselves
        // (--stream-worker, --dev), and only the pipe name travels.
        const bool started =
            how == Launch::ServiceAsSystem ? m_Console->startThroughService(&error)
            : how == Launch::SignInScreen
                ? m_Console->startThroughSignInService(&error)
                : m_Console->startThroughTask(ConsoleSession::elevatedWorkerTaskName(), &error);
        if (!started) {
            qWarning() << "[StreamWorkerHost]"
                       << (how == Launch::ServiceAsSystem ? "SYSTEM worker service"
                           : how == Launch::SignInScreen  ? "Sign-in screen worker"
                                                          : "Elevated worker task")
                       << (how == Launch::SignInScreen
                               ? "refused, and nothing else streams the sign-in screen:"
                               : "unavailable, falling back to a worker with less reach:")
                       << error;
            m_Console->deleteLater();
            m_Console = nullptr;
            return false;
        }
    } else if (!m_Console->start(QCoreApplication::applicationFilePath(), args, &error)) {
        // Said plainly, because the fallback the caller has (run the session
        // in this process) cannot work either: there is no desktop here.
        qWarning() << "[StreamWorkerHost] Native worker cannot enter the console session:" << error;
        m_Console->deleteLater();
        m_Console = nullptr;
        return false;
    }

    // The wheels kept from the last stream go with the config, to a worker
    // that can both make such devices and reach into this process for them
    // (HidParkingTransfer). Never to the guests' feed: it has no page to give
    // them to, and would hold them unanswered for the whole shared session.
    QByteArray line = configLine;
    // Never to the sign-in screen's worker: keyboard and mouse only there.
    if (how == Launch::ServiceAsSystem || how == Launch::TaskElevated) {
        QJsonObject config = QJsonDocument::fromJson(configLine).object();
        if (config[QStringLiteral("backendType")].toString() == QLatin1String("native") &&
            config[QStringLiteral("role")].toString() != QLatin1String("feed")) {
            uint64_t batch = 0;
            const QJsonObject parked = HidParkingTransfer::handOut(&batch);
            if (batch) {
                config[QStringLiteral("hidParked")] = parked;
                line = QJsonDocument(config).toJson(QJsonDocument::Compact) + "\n";
                m_HidBatch = batch;
                // A worker that never reads its config never answers: the
                // devices cannot wait unanswered for the game that long.
                QTimer::singleShot(30000, this, [this, batch]() {
                    if (m_HidBatch != batch) return;
                    m_HidBatch = 0;
                    HidParkingTransfer::settle(batch, false);
                });
            }
        }
    }
    // The UDP ports every stream shares: a worker started by the service (on
    // the desktop or on the sign-in screen) or by the task runs above this
    // process, which may not open it, and is told which to take. One in the
    // console session is handed them when the
    // token it was started with is the owner's, and gets none otherwise
    // (muxHandoverFor).
    line = withSharedSockets(line, how, m_Console->runsAsOwner(), m_Console->processId());
    m_Console->write(line);
    m_Launch = how;
    qInfo() << "[StreamWorkerHost] Worker spawned"
            << (how == Launch::ConsoleAsUser ? "in the console session as"
                                             : "through its launcher as")
            << m_Console->userName() << ", pid=" << m_Console->processId();
    return true;
}

bool StreamWorkerHost::isRunning() const
{
    if (m_Console) return m_Console->isRunning();
    return m_Proc && m_Proc->state() != QProcess::NotRunning;
}

void StreamWorkerHost::writeLine(const QByteArray& line)
{
    if (!isRunning()) return;
    if (m_Console)
        m_Console->write(line);
    else
        m_Proc->write(line);
}

void StreamWorkerHost::killChild()
{
    if (m_Console)
        m_Console->kill();
    else if (m_Proc)
        m_Proc->kill();
}

qint64 StreamWorkerHost::childPid() const
{
    if (m_Console) return m_Console->processId();
    return m_Proc ? m_Proc->processId() : 0;
}

void StreamWorkerHost::restoreMouseKeysAfterWorker()
{
    m_MouseKeysHeld = false;
#ifdef Q_OS_WIN
    // The setting belongs to the console session: a server running as a
    // service sits in session 0 and would change that one's instead.
    if (ConsoleSession::launchesElsewhere()) {
        qWarning() << "[StreamWorkerHost] The worker died holding Mouse Keys, and this process is "
                      "not in the console session — they stay on until sign-out";
        return;
    }
    auto field = [this](const char* name) {
        const qint64 v = m_MouseKeysSaved[QLatin1String(name)].toInteger(-1);
        return v >= 0 && v <= 0xFFFFFFFFLL ? static_cast<uint32_t>(v) : 0u;
    };
    mw::native::MouseKeysState saved;
    saved.flags = field("flags");
    saved.maxSpeed = field("maxSpeed");
    saved.timeToMaxSpeed = field("timeToMaxSpeed");
    saved.ctrlSpeed = field("ctrlSpeed");
    std::string how;
    mw::native::restoreMouseKeys(saved, how);
    qInfo() << "[StreamWorkerHost] The worker died holding Mouse Keys:" << how.c_str();
#endif
}

void StreamWorkerHost::sendCommand(const char* cmd)
{
    writeLine(QByteArray("{\"cmd\":\"") + cmd + "\"}\n");
}

void StreamWorkerHost::sendJson(const QJsonObject& msg)
{
    // One line, no newlines inside: the worker's pump reads with std::getline.
    writeLine(QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n");
}

void StreamWorkerHost::setInputPolicy(bool gamepad, bool keyboardMouse, bool hid)
{
    // Logged at every hop it makes, because a policy that quietly fails to
    // arrive is indistinguishable from one that was never sent, and the thing
    // it governs is whether a guest can drive the machine.
    qInfo() << "[StreamWorkerHost] policy → worker: gamepad=" << gamepad
            << "keyboardMouse=" << keyboardMouse << "hid=" << hid << "running=" << isRunning();
    // Fire-and-forget, unlike the teardown commands: nothing here kills the
    // worker, and a worker that has already exited simply has no policy left to
    // change.
    sendJson(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("setPolicy")},
                         {QStringLiteral("gamepad"), gamepad},
                         {QStringLiteral("keyboardMouse"), keyboardMouse},
                         {QStringLiteral("hid"), hid}});
}

void StreamWorkerHost::requestQuit()
{
    if (!isRunning()) return;
    m_StopAsked = true;
    sendCommand("quit");
    // The worker gives its relay thread ~3s to unwind; kill shortly after in
    // case it wedged (moonlight teardown can hang on a dead host).
    QTimer::singleShot(5000, this, [this]() {
        if (isRunning()) {
            qWarning() << "[StreamWorkerHost] Worker did not exit after quit — killing";
            killChild();
        }
    });
}

void StreamWorkerHost::notifyTakenOver()
{
    m_StopAsked = true;
    sendCommand("takenOver");
    QTimer::singleShot(5000, this, [this]() {
        if (isRunning()) killChild();
    });
}

void StreamWorkerHost::notifyRevoked()
{
    m_StopAsked = true;
    sendCommand("revoked");
    QTimer::singleShot(5000, this, [this]() {
        if (isRunning()) killChild();
    });
}

void StreamWorkerHost::notifySessionEnded()
{
    m_StopAsked = true;
    sendCommand("sessionEnded");
    QTimer::singleShot(5000, this, [this]() {
        if (isRunning()) killChild();
    });
}

void StreamWorkerHost::onStderrData(const QByteArray& data)
{
    // Relay worker stderr lines into our log (prefixed).
    for (const QByteArray& line : data.split('\n'))
        if (!line.trimmed().isEmpty()) qInfo() << "[StreamWorker/child]" << line.constData();
}

void StreamWorkerHost::onStdout()
{
    onStdoutData(m_Proc->readAllStandardOutput());
}

void StreamWorkerHost::onChildFinished(int exitCode, bool crashed)
{
    onFinished(exitCode, crashed ? QProcess::CrashExit : QProcess::NormalExit);
}

namespace {
/// audioFocusApp(): written and read on the main thread (the workers' stdout
/// and the HTTP routes both run there).
QString& audioFocusSlot()
{
    static QString app;
    return app;
}

/// The workers whose agent expects a TCP connection on the shared ports, by
/// that agent's number in the mux. On the main thread, like the above.
QHash<quint32, StreamWorkerHost*>& muxTcpHosts()
{
    static QHash<quint32, StreamWorkerHost*> hosts;
    return hosts;
}
} // namespace

QString StreamWorkerHost::audioFocusApp()
{
    return audioFocusSlot();
}

void StreamWorkerHost::onStdoutData(const QByteArray& data)
{
    m_Buf += data;
    int nl;
    while ((nl = m_Buf.indexOf('\n')) >= 0) {
        const QByteArray line = m_Buf.left(nl);
        m_Buf.remove(0, nl + 1);
        const QJsonObject event = QJsonDocument::fromJson(line).object();
        const QString type = event["event"].toString();
        if (type == QLatin1String("response")) {
            if (!m_ResponseEmitted) {
                m_ResponseEmitted = true;
                emit responseReady(event["code"].toInt(500), event["body"].toObject());
            }
        } else if (type == QLatin1String("ended")) {
            if (!m_EndedEmitted) {
                m_EndedEmitted = true;
                emit ended();
            }
        } else if (type == QLatin1String("coopSession")) {
            const QString id = event["sessionId"].toString();
            if (!id.isEmpty()) emit coopSessionResolved(id);
        } else if (type == QLatin1String("hostIpTtl")) {
            const int ttl = event["ttl"].toInt();
            if (ttl > 0) emit hostIpTtlObserved(ttl);
        } else if (type == QLatin1String("portalGrant")) {
            const QString token = event["token"].toString();
            if (!token.isEmpty()) emit portalGrantReceived(token);
        } else if (type == QLatin1String("captureHold")) {
            if (event["display_mhz"].toInt() > 0) emit captureHoldReceived(event);
        } else if (type == QLatin1String("hidAdopted")) {
            // The worker holds its copies of the kept devices: ours go.
            if (m_HidBatch) HidParkingTransfer::settle(m_HidBatch, true);
            m_HidBatch = 0;
        } else if (type == QLatin1String("hidParked")) {
            // The stream is over, its wheels stay: their handles are ours now.
            HidParkingTransfer::takeIn(event);
        } else if (type == QLatin1String("audioFocus")) {
            audioFocusSlot() = event["app"].toString().left(260);
            m_ReportedAudioFocus = true;
        } else if (type == QLatin1String("iceMux")) {
            onMuxEvent(event);
        } else if (type == QLatin1String("mouseKeys")) {
            m_MouseKeysHeld = event["on"].toBool();
            m_MouseKeysSaved = m_MouseKeysHeld ? event["saved"].toObject() : QJsonObject();
        } else if (!line.trimmed().isEmpty()) {
            qWarning() << "[StreamWorkerHost] Unknown worker event:" << line.constData();
        }
    }
}

void StreamWorkerHost::dropMuxAgent()
{
    if (!m_MuxFeed) return;
    // No connection for it from here on: removed, an agent expects none
    muxTcpHosts().remove(m_MuxFeed);
    if (IceMux* mux = IceMux::shared()) mux->removeAgent(m_MuxFeed);
    m_MuxFeed = 0;
    m_MuxFeedPort = 0;
}

quint32 StreamWorkerHost::nextMuxOwner()
{
    // On the main thread, where every one of these objects is made
    static quint32 last = 0;
    if (++last == 0) last = 1;
    return last;
}

void StreamWorkerHost::closeMuxChannel()
{
#ifdef Q_OS_WIN
    if (m_MuxChild) IceMuxTcp::letGoOfProcess(m_MuxChild);
    m_MuxChild = nullptr;
#else
    if (m_MuxChannel >= 0) ::close(m_MuxChannel);
    if (m_MuxChannelWorkerEnd >= 0) ::close(m_MuxChannelWorkerEnd);
    m_MuxChannel = -1;
    m_MuxChannelWorkerEnd = -1;
#endif
}

void StreamWorkerHost::handMuxConnection(quint32 feed, quint32 number)
{
    StreamWorkerHost* host = muxTcpHosts().value(feed, nullptr);
    if (host && host->m_MuxFeed == feed) {
        host->handConnection(number);
        return;
    }
    // Its agent is gone, or connected, since the connection named it
    if (IceMux* mux = IceMux::shared()) mux->tcp().letGo(number);
}

void StreamWorkerHost::handConnection(quint32 number)
{
    IceMux* mux = IceMux::shared();
    if (!mux) return;
    const quint32 feed = m_MuxFeed;
    bool handed = false;
#ifdef Q_OS_WIN
    // In a command, with the agent it is for and its first frame: described
    // for this worker's process, or named for the worker to copy
    QJsonObject command{{QStringLiteral("cmd"), QStringLiteral("iceMuxTcp")},
                        {QStringLiteral("n"), static_cast<double>(number)},
                        {QStringLiteral("feed"), static_cast<int>(m_MuxFeedPort)}};
    const auto base64 = [](const std::vector<uint8_t>& bytes) {
        return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(bytes.data()),
                                              static_cast<qsizetype>(bytes.size()))
                                       .toBase64());
    };
    std::vector<uint8_t> frame;
    if (m_MuxTcp && m_MuxHandover == MuxHandover::Handed) {
        std::vector<uint8_t> description;
        handed = mux->tcp().describeFor(feed, number, m_MuxChild, &description, &frame);
        if (handed) command[QStringLiteral("socket")] = base64(description);
    } else if (m_MuxTcp && m_MuxHandover == MuxHandover::Taken) {
        // Named: kept from here on until this worker answers for it or is
        // gone, whatever becomes of its agent. With the connection's two
        // ports, for the worker to hold its copy against.
        uint64_t handle = 0;
        uint16_t local = 0, remote = 0;
        handed = mux->tcp().nameFor(feed, number, &handle, &frame, &local, &remote);
        if (handed) {
            command[QStringLiteral("handle")] = QString::number(handle);
            command[QStringLiteral("lport")] = static_cast<int>(local);
            command[QStringLiteral("rport")] = static_cast<int>(remote);
        }
    }
    if (handed) {
        command[QStringLiteral("frame")] = base64(frame);
        sendJson(command);
    }
#else
    // As one message of this worker's channel: the connection itself
    const int channel = m_MuxChannel;
    const quint32 agent = m_MuxFeedPort;
    handed = m_MuxTcp && channel >= 0 &&
             mux->tcp().use(feed, number,
                            [channel, agent](uintptr_t socket, const uint8_t* frame, size_t size) {
                                return mw::icemuxhandles::sendConnection(
                                    channel, static_cast<int>(socket), agent,
                                    reinterpret_cast<const char*>(frame), size);
                            });
#endif
    // Not handed: this process's end goes at once. Handed: when the worker
    // says it has its own; or after two seconds, unless it was named
    // (IceMuxTcp).
    if (!handed) mux->tcp().letGo(number);
}

void StreamWorkerHost::releaseMuxSockets()
{
    dropMuxAgent();
    // No agent of this worker from here on, whatever it still says
    m_MuxSockets.clear();
    // Nor any way to hand it a connection: what the mux kept for it, named
    // or not, is let go, since no process is left to copy it
    m_MuxTcp = false;
    m_MuxHandover = MuxHandover::None;
    closeMuxChannel();
    if (IceMux* mux = IceMux::shared()) mux->tcp().release(m_MuxOwner);
    if (!m_MuxLease) return;
    if (IceMux* mux = IceMux::shared()) mux->release(m_MuxLease);
    m_MuxLease = 0;
}

void StreamWorkerHost::onMuxEvent(const QJsonObject& event)
{
    IceMux* mux = IceMux::shared();
    if (!mux || !mux->running()) return;
    const QString op = event["op"].toString();
    if (op == QLatin1String("agent")) {
        // One agent at a time per worker: a new one replaces the last.
        dropMuxAgent();
        const int feedPort = event["feedPort"].toInt();
        if (feedPort <= 0 || feedPort > 65535) return;
        // On the sockets this worker was handed, and no other: a worker that
        // was handed none has no agent here, whatever it says.
        const std::vector<uint8_t> sockets(m_MuxSockets.cbegin(), m_MuxSockets.cend());
        m_MuxFeed = mux->addAgent(event["ufrag"].toString().toStdString(), sockets);
        if (!m_MuxFeed) {
            qWarning() << "[IceMux] the worker's ICE agent is refused: no datagram will reach it";
            return;
        }
        mux->setFeedPort(m_MuxFeed, static_cast<uint16_t>(feedPort));
        qInfo() << "[IceMux] agent" << m_MuxFeed << "registered for the worker";
        // One that listens in TCP: the server listens for it from now on, on
        // the ports and under the fragment it has just been registered with.
        // Only for a worker that was told it could be handed what comes.
        if (event["tcp"].toBool() && m_MuxTcp) {
            m_MuxFeedPort = static_cast<quint16>(feedPort);
            if (mux->expectTcp(m_MuxFeed, m_MuxOwner)) {
                muxTcpHosts().insert(m_MuxFeed, this);
                qInfo() << "[IceMux] agent" << m_MuxFeed
                        << "expects a TCP connection on the shared port(s)";
            } else {
                qWarning() << "[IceMux] no shared port can be listened on in TCP for agent"
                           << m_MuxFeed << "— it is reached over UDP alone";
            }
        }
    } else if (op == QLatin1String("confirmed")) {
        if (!m_MuxFeed) return;
        // Connected: it expects no connection any more, whichever way it is
        muxTcpHosts().remove(m_MuxFeed);
        mux->tcp().close(m_MuxFeed);
        const int port = event["port"].toInt();
        if (port <= 0 || port > 65535) return;
        mux->confirm(m_MuxFeed, event["address"].toString().toStdString(),
                     static_cast<uint16_t>(port));
    } else if (op == QLatin1String("tcpGot")) {
        // The worker has its own end of a connection it was handed, or will
        // not have it: either way this process lets go of its own. A worker
        // answers for what it was handed and nothing of another's, and may
        // do so after its agent is gone.
        const double told = event["n"].toDouble();
        mux->tcp().answered(m_MuxOwner,
                            told >= 1 && told <= 4294967295.0 ? static_cast<uint32_t>(told) : 0u);
    } else if (op == QLatin1String("gone")) {
        dropMuxAgent();
    }
}

void StreamWorkerHost::onFinished(int exitCode, QProcess::ExitStatus status)
{
    qInfo() << "[StreamWorkerHost] Worker finished, exitCode=" << exitCode << "status=" << status;
    // Its agent goes, and with its process its copies of the shared sockets:
    // the mux need not keep a replaced port for it any longer.
    releaseMuxSockets();
    if (m_ReportedAudioFocus) {
        audioFocusSlot().clear();
        m_ReportedAudioFocus = false;
    }
    // Gone without taking the kept devices: they answer the game here again.
    if (m_HidBatch) HidParkingTransfer::settle(m_HidBatch, false);
    m_HidBatch = 0;
    // Gone holding Mouse Keys (killed, crashed): they are put back here.
    if (m_MouseKeysHeld) restoreMouseKeysAfterWorker();
    // A worker that dies without answering still owes the browser a reply, and
    // its session is over either way.
    if (!m_ResponseEmitted) {
        m_ResponseEmitted = true;
        emit responseReady(502,
                           QJsonObject{{QStringLiteral("error"),
                                        QStringLiteral("stream worker exited before responding")}});
    }
    if (!m_EndedEmitted) {
        m_EndedEmitted = true;
        m_Crashed = StreamEnds::stateAfterExit(false, m_StopAsked) == StreamEnds::State::Crashed;
        if (m_Crashed)
            qWarning() << "[StreamWorkerHost] The worker died without ending its session — crashed";
        emit ended();
    }
    emit exited();
}
