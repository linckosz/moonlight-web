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

#include "NativeProbeService.h"

#include "NativeCapabilitiesJson.h"
#include "../../common/Logger.h"
#include "mw/native/NativeHost.h"

#include <QCoreApplication>
#include <QJsonDocument>

#include <cstdio>

namespace {

/// How long a snapshot is handed out before someone asking triggers a new
/// probe. A display plugged in shows up within this after the next host-list
/// request; a probe process every few seconds while a page polls would not be
/// free, so it is not shorter.
constexpr int kSnapshotTtlMs = 20000;

/// The console probe opens a D3D device per GPU and asks each encoder what it
/// can do — a second or two on a slow machine, never fifteen. Past this the
/// child is killed and the answer is "probe failed".
constexpr int kProbeTimeoutMs = 15000;

/// How often the console session is looked at (no process spawned): a logoff
/// takes the host card down within this, a logon puts it back.
constexpr int kWatchIntervalMs = 5000;

mw::native::Capabilities pending(const QString& why)
{
    mw::native::Capabilities caps;
    caps.available = false;
    caps.reason = mw::native::Unavailability::NoInteractiveSession;
    caps.diagnostic = why.toStdString();
    return caps;
}

} // namespace

NativeProbeService& NativeProbeService::instance()
{
    static NativeProbeService s_Instance;
    return s_Instance;
}

NativeProbeService::NativeProbeService()
    : m_Remote(ConsoleSession::launchesElsewhere())
{
    if (!m_Remote) return;

    m_Caps = pending(QStringLiteral("waiting for the console session probe"));

    m_ProbeTimeout.setSingleShot(true);
    m_ProbeTimeout.setInterval(kProbeTimeoutMs);
    connect(&m_ProbeTimeout, &QTimer::timeout, this, &NativeProbeService::probeTimedOut);

    m_Watch.setInterval(kWatchIntervalMs);
    connect(&m_Watch, &QTimer::timeout, this, &NativeProbeService::onWatchTick);
    m_Watch.start();

    Logger::info(QStringLiteral("[native] this process has no desktop (session 0) — the engine "
                                "is probed and run in the console session"));
    refresh();
}

mw::native::Capabilities NativeProbeService::snapshot()
{
    if (!m_Remote) return mw::native::NativeHost::probe();
    if (!m_HaveResult || m_Age.elapsed() > kSnapshotTtlMs) refresh();
    return m_Caps;
}

qint64 NativeProbeService::snapshotAgeMs() const
{
    if (!m_Remote) return 0;
    return m_HaveResult ? m_Age.elapsed() : -1;
}

void NativeProbeService::refresh()
{
    if (!m_Remote || m_Probe) return;

    m_Console = ConsoleSession::query();
    if (!m_Console.userPresent) {
        apply(pending(m_Console.reason));
        return;
    }

    m_Probe = new ConsoleProcess(this);
    m_ProbeOut.clear();
    m_ProbeErr.clear();
    connect(m_Probe, &ConsoleProcess::stdoutData, this,
            [this](const QByteArray& data) { m_ProbeOut += data; });
    connect(m_Probe, &ConsoleProcess::stderrData, this,
            [this](const QByteArray& data) { m_ProbeErr += data; });
    connect(m_Probe, &ConsoleProcess::finished, this, &NativeProbeService::probeFinished);

    QString error;
    if (!m_Probe->start(QCoreApplication::applicationFilePath(), {QStringLiteral("--native-probe")},
                        &error)) {
        Logger::warning(QStringLiteral("[native] console probe could not start: %1").arg(error));
        m_Probe->deleteLater();
        m_Probe = nullptr;
        mw::native::Capabilities caps = pending(error);
        caps.reason = mw::native::Unavailability::ProbeFailed;
        apply(caps);
        return;
    }
    m_ProbeTimeout.start();
}

void NativeProbeService::probeFinished(int exitCode, bool crashed)
{
    m_ProbeTimeout.stop();
    ConsoleProcess* probe = m_Probe;
    m_Probe = nullptr;
    if (probe) probe->deleteLater();

    // The engine's own account of what it found, kept at debug: on success it
    // is the same "[native] available: …" line the desktop build logs, and on
    // failure it is the only clue there is.
    for (const QByteArray& line : m_ProbeErr.split('\n')) {
        if (!line.trimmed().isEmpty())
            Logger::debug(QStringLiteral("[native-probe] %1").arg(QString::fromUtf8(line)));
    }

    mw::native::Capabilities caps;
    const QJsonDocument doc = QJsonDocument::fromJson(m_ProbeOut);
    if (crashed || !doc.isObject() || !NativeCapabilitiesJson::fromJson(doc.object(), caps)) {
        caps = mw::native::Capabilities{};
        caps.available = false;
        caps.reason = mw::native::Unavailability::ProbeFailed;
        caps.diagnostic =
            crashed ? QStringLiteral("the console probe crashed (0x%1)")
                          .arg(static_cast<quint32>(exitCode), 8, 16, QLatin1Char('0'))
                          .toStdString()
                    : QStringLiteral("the console probe answered nothing readable (exit %1)")
                          .arg(exitCode)
                          .toStdString();
        Logger::warning(QStringLiteral("[native] %1").arg(QString::fromStdString(caps.diagnostic)));
    }
    apply(caps);
}

void NativeProbeService::probeTimedOut()
{
    if (!m_Probe) return;
    Logger::warning(QStringLiteral("[native] console probe did not answer in %1 s — killed")
                        .arg(kProbeTimeoutMs / 1000));
    m_Probe->kill();
    // finished() follows the kill and reports the failure through probeFinished.
}

void NativeProbeService::onWatchTick()
{
    if (m_Probe) return;
    const ConsoleSession::Info now = ConsoleSession::query();
    const bool moved = now.userPresent != m_Console.userPresent ||
                       now.sessionId != m_Console.sessionId || now.userName != m_Console.userName;
    if (!moved) return;
    Logger::info(QStringLiteral("[native] console session changed: %1")
                     .arg(now.userPresent ? QStringLiteral("%1 logged on (session %2)")
                                                .arg(now.userName)
                                                .arg(now.sessionId)
                                          : now.reason));
    refresh();
}

void NativeProbeService::apply(const mw::native::Capabilities& caps)
{
    const bool first = !m_HaveResult;
    const bool differs =
        first || NativeCapabilitiesJson::toJson(caps) != NativeCapabilitiesJson::toJson(m_Caps);
    m_Caps = caps;
    m_HaveResult = true;
    m_Age.restart();
    if (!differs) return;

    if (caps.available) {
        Logger::info(QStringLiteral("[native] console probe: available — %1 display(s), %2 GPU(s), "
                                    "as %3")
                         .arg(caps.displays.size())
                         .arg(caps.gpus.size())
                         .arg(m_Console.userName));
    } else {
        Logger::info(QStringLiteral("[native] console probe: unavailable — %1")
                         .arg(QString::fromStdString(caps.diagnostic)));
    }
    emit changed();
}

int NativeProbeService::runProbeCommand()
{
    // stdout is the answer; everything the engine has to say goes to stderr,
    // where the parent picks it up for its own log.
    mw::native::NativeHost::setLogSink([](int level, const std::string& message) {
        static const char* const kNames[] = {"debug", "info", "warn", "error"};
        const char* name = (level >= 0 && level <= 3) ? kNames[level] : "?";
        std::fprintf(stderr, "[%s] %s\n", name, message.c_str());
    });

    const mw::native::Capabilities caps = mw::native::NativeHost::probe();
    const QByteArray json =
        QJsonDocument(NativeCapabilitiesJson::toJson(caps)).toJson(QJsonDocument::Compact) + "\n";
    std::fwrite(json.constData(), 1, static_cast<size_t>(json.size()), stdout);
    std::fflush(stdout);
    return 0;
}
