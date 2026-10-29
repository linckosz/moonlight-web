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
#include "NativeBench.h"
#include "StreamEnds.h"
#include "WorkerService.h"
#include "common/Edition.h"
#include "common/LinuxCapabilities.h"
#include "common/RunFlags.h"
#include "mw/native/MouseKeys.h"
#include "server/AppSettings.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QTimer>
#include <QDebug>

StreamWorkerHost::StreamWorkerHost(QObject* parent)
    : QObject(parent)
{}

StreamWorkerHost::~StreamWorkerHost()
{
    if (m_Proc && m_Proc->state() != QProcess::NotRunning) {
        // Destructor teardown: no time for the graceful dance — kill. Normal
        // paths call requestQuit()/notify*() well before destruction.
        m_Proc->kill();
        m_Proc->waitForFinished(1000);
    }
    // A console child is killed and joined by its own destructor (it is our
    // QObject child), so nothing more is needed here.
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
    switch (how) {
    case Launch::ConsoleAsUser: return startInConsoleSession(args, configLine, how);
    case Launch::ServiceAsSystem:
        return WorkerService::available() && startInConsoleSession(args, configLine, how);
    case Launch::TaskElevated:
        return ConsoleSession::elevatedWorkerAvailable() &&
               startInConsoleSession(args, configLine, how);
    case Launch::Plain: break;
    }
    return startInProcess(args, configLine, native);
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
    if (native && !userToken && WorkerService::available() &&
        startInConsoleSession(args, configLine, Launch::ServiceAsSystem))
        return true;
    if (native && ConsoleSession::elevatedWorkerAvailable() &&
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
    // KMS capture needs CAP_SYS_ADMIN, a GPU context above normal priority
    // CAP_SYS_NICE. This process keeps both permitted but hands them to nothing
    // it spawns (common/LinuxCapabilities.h) — except this one child, the only
    // one that captures and converts. Runs in the forked child before exec.
    if (native) m_Proc->setChildProcessModifier([] { mw::raiseStreamCapabilitiesForChild(); });
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

    m_Proc->start();
    if (!m_Proc->waitForStarted(5000)) {
        qWarning() << "[StreamWorkerHost] Failed to spawn worker:" << m_Proc->errorString();
        m_Proc->deleteLater();
        m_Proc = nullptr;
        return false;
    }

    m_Proc->write(configLine);
    m_Launch = Launch::Plain;
    qInfo() << "[StreamWorkerHost] Worker spawned, pid=" << m_Proc->processId();
    return true;
}

bool StreamWorkerHost::startInConsoleSession(const QStringList& args, const QByteArray& configLine,
                                             Launch how)
{
    m_Console = new ConsoleProcess(this);
    connect(m_Console, &ConsoleProcess::stdoutData, this, &StreamWorkerHost::onStdoutData);
    connect(m_Console, &ConsoleProcess::stderrData, this, &StreamWorkerHost::onStderrData);
    connect(m_Console, &ConsoleProcess::finished, this, &StreamWorkerHost::onChildFinished);

    QString error;
    if (how == Launch::ServiceAsSystem || how == Launch::TaskElevated) {
        // Neither a task nor a service can be handed our pipes, and neither
        // takes the arguments from here: both carry the fixed ones themselves
        // (--stream-worker, --dev), and only the pipe name travels.
        const bool started =
            how == Launch::ServiceAsSystem
                ? m_Console->startThroughService(&error)
                : m_Console->startThroughTask(ConsoleSession::elevatedWorkerTaskName(), &error);
        if (!started) {
            qWarning() << "[StreamWorkerHost]"
                       << (how == Launch::ServiceAsSystem ? "SYSTEM worker service"
                                                          : "Elevated worker task")
                       << "unavailable, falling back to a worker with less reach:" << error;
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
        } else if (type == QLatin1String("mouseKeys")) {
            m_MouseKeysHeld = event["on"].toBool();
            m_MouseKeysSaved = m_MouseKeysHeld ? event["saved"].toObject() : QJsonObject();
        } else if (!line.trimmed().isEmpty()) {
            qWarning() << "[StreamWorkerHost] Unknown worker event:" << line.constData();
        }
    }
}

void StreamWorkerHost::onFinished(int exitCode, QProcess::ExitStatus status)
{
    qInfo() << "[StreamWorkerHost] Worker finished, exitCode=" << exitCode << "status=" << status;
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
