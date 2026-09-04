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
#include "ConsoleSession.h"

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
}

bool StreamWorkerHost::start(const QJsonObject& config)
{
    Q_ASSERT(!m_Proc && !m_Console);
    QStringList args{QStringLiteral("--stream-worker")};
    // Carry --dev into the child. The flag drives the application name, which
    // is what moves the settings, logs and — the part that matters here — the
    // client identity the worker presents to the host. Without it a dev run
    // would launch streams as the installed instance.
    if (QCoreApplication::applicationName().endsWith(QStringLiteral("-dev")))
        args << QStringLiteral("--dev");
    const QByteArray configLine = QJsonDocument(config).toJson(QJsonDocument::Compact) + "\n";

    // The native engine captures the desktop, and a service has none: its
    // worker goes to the console session, as the user sitting there. Every
    // other backend talks to a host over the network and runs fine from here.
    const bool native = config["backendType"].toString() == QLatin1String("native");
    if (native && ConsoleSession::launchesElsewhere())
        return startInConsoleSession(args, configLine);
    return startInProcess(args, configLine);
}

bool StreamWorkerHost::startInProcess(const QStringList& args, const QByteArray& configLine)
{
    m_Proc = new QProcess(this);
    m_Proc->setProgram(QCoreApplication::applicationFilePath());
    m_Proc->setArguments(args);
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
    qInfo() << "[StreamWorkerHost] Worker spawned, pid=" << m_Proc->processId();
    return true;
}

bool StreamWorkerHost::startInConsoleSession(const QStringList& args, const QByteArray& configLine)
{
    m_Console = new ConsoleProcess(this);
    connect(m_Console, &ConsoleProcess::stdoutData, this, &StreamWorkerHost::onStdoutData);
    connect(m_Console, &ConsoleProcess::stderrData, this, &StreamWorkerHost::onStderrData);
    connect(m_Console, &ConsoleProcess::finished, this, &StreamWorkerHost::onChildFinished);

    QString error;
    if (!m_Console->start(QCoreApplication::applicationFilePath(), args, &error)) {
        // Said plainly, because the fallback the caller has (run the session
        // in this process) cannot work either: there is no desktop here.
        qWarning() << "[StreamWorkerHost] Native worker cannot enter the console session:" << error;
        m_Console->deleteLater();
        m_Console = nullptr;
        return false;
    }

    m_Console->write(configLine);
    qInfo() << "[StreamWorkerHost] Worker spawned in the console session as"
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

void StreamWorkerHost::sendCommand(const char* cmd)
{
    writeLine(QByteArray("{\"cmd\":\"") + cmd + "\"}\n");
}

void StreamWorkerHost::sendJson(const QJsonObject& msg)
{
    // One line, no newlines inside: the worker's pump reads with std::getline.
    writeLine(QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n");
}

void StreamWorkerHost::setInputPolicy(bool gamepad, bool keyboardMouse)
{
    // Logged at every hop it makes, because a policy that quietly fails to
    // arrive is indistinguishable from one that was never sent, and the thing
    // it governs is whether a guest can drive the machine.
    qInfo() << "[StreamWorkerHost] policy → worker: gamepad=" << gamepad
            << "keyboardMouse=" << keyboardMouse << "running=" << isRunning();
    // Fire-and-forget, unlike the teardown commands: nothing here kills the
    // worker, and a worker that has already exited simply has no policy left to
    // change.
    sendJson(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("setPolicy")},
                         {QStringLiteral("gamepad"), gamepad},
                         {QStringLiteral("keyboardMouse"), keyboardMouse}});
}

void StreamWorkerHost::requestQuit()
{
    if (!isRunning()) return;
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
    sendCommand("takenOver");
    QTimer::singleShot(5000, this, [this]() {
        if (isRunning()) killChild();
    });
}

void StreamWorkerHost::notifyRevoked()
{
    sendCommand("revoked");
    QTimer::singleShot(5000, this, [this]() {
        if (isRunning()) killChild();
    });
}

void StreamWorkerHost::notifySessionEnded()
{
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
        } else if (!line.trimmed().isEmpty()) {
            qWarning() << "[StreamWorkerHost] Unknown worker event:" << line.constData();
        }
    }
}

void StreamWorkerHost::onFinished(int exitCode, QProcess::ExitStatus status)
{
    qInfo() << "[StreamWorkerHost] Worker finished, exitCode=" << exitCode << "status=" << status;
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
        emit ended();
    }
    emit exited();
}
