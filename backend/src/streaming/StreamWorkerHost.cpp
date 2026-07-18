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
}

bool StreamWorkerHost::start(const QJsonObject& config)
{
    Q_ASSERT(!m_Proc);
    m_Proc = new QProcess(this);
    m_Proc->setProgram(QCoreApplication::applicationFilePath());
    m_Proc->setArguments({QStringLiteral("--stream-worker")});
    // stdout carries the JSON event protocol; worker stderr joins ours so a
    // crashing worker still leaves a trace in the parent console/journal.
    m_Proc->setProcessChannelMode(QProcess::SeparateChannels);
    m_Proc->setReadChannel(QProcess::StandardOutput);

    connect(m_Proc, &QProcess::readyReadStandardOutput, this, &StreamWorkerHost::onStdout);
    connect(m_Proc, &QProcess::readyReadStandardError, this, [this]() {
        // Relay worker stderr lines into our log (prefixed).
        const QByteArray err = m_Proc->readAllStandardError();
        for (const QByteArray& line : err.split('\n'))
            if (!line.trimmed().isEmpty()) qInfo() << "[StreamWorker/child]" << line.constData();
    });
    connect(m_Proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &StreamWorkerHost::onFinished);

    m_Proc->start();
    if (!m_Proc->waitForStarted(5000)) {
        qWarning() << "[StreamWorkerHost] Failed to spawn worker:" << m_Proc->errorString();
        m_Proc->deleteLater();
        m_Proc = nullptr;
        return false;
    }

    const QByteArray line = QJsonDocument(config).toJson(QJsonDocument::Compact) + "\n";
    m_Proc->write(line);
    qInfo() << "[StreamWorkerHost] Worker spawned, pid=" << m_Proc->processId();
    return true;
}

bool StreamWorkerHost::isRunning() const
{
    return m_Proc && m_Proc->state() != QProcess::NotRunning;
}

void StreamWorkerHost::sendCommand(const char* cmd)
{
    if (!isRunning()) return;
    m_Proc->write(QByteArray("{\"cmd\":\"") + cmd + "\"}\n");
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
            m_Proc->kill();
        }
    });
}

void StreamWorkerHost::notifyTakenOver()
{
    sendCommand("takenOver");
    QTimer::singleShot(5000, this, [this]() {
        if (isRunning()) m_Proc->kill();
    });
}

void StreamWorkerHost::notifyRevoked()
{
    sendCommand("revoked");
    QTimer::singleShot(5000, this, [this]() {
        if (isRunning()) m_Proc->kill();
    });
}

void StreamWorkerHost::onStdout()
{
    m_Buf += m_Proc->readAllStandardOutput();
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
        } else if (!line.trimmed().isEmpty()) {
            qWarning() << "[StreamWorkerHost] Unknown worker event:" << line.constData();
        }
    }
}

void StreamWorkerHost::onFinished(int exitCode, QProcess::ExitStatus status)
{
    qInfo() << "[StreamWorkerHost] Worker finished, exitCode=" << exitCode
            << "status=" << status;
    // A worker that dies without answering still owes the browser a reply, and
    // its session is over either way.
    if (!m_ResponseEmitted) {
        m_ResponseEmitted = true;
        emit responseReady(
            502, QJsonObject{{QStringLiteral("error"),
                              QStringLiteral("stream worker exited before responding")}});
    }
    if (!m_EndedEmitted) {
        m_EndedEmitted = true;
        emit ended();
    }
    emit exited();
}
