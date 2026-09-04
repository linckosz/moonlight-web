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

#pragma once

#include <QObject>
#include <QProcess>
#include <QJsonObject>

/**
 * Parent-side handle for one `MoonlightWeb --stream-worker` child process.
 *
 * Each worker hosts ONE streaming session (Sunshine launch + relay graph +
 * moonlight-common-c). Running sessions in child processes is what allows two
 * concurrent streams despite moonlight-common-c being a process-global
 * singleton (and gives crash isolation for free).
 *
 * Protocol: see worker/StreamWorkerMain.h (JSON lines over stdin/stdout;
 * worker logging goes to its own log file, stderr is forwarded).
 *
 * Two launchers, one protocol. A GameStream/Wolf/MultiSeat worker is a plain
 * QProcess child of this process. A NATIVE worker started by the Windows
 * service is launched into the console session as the logged-on user instead
 * (ConsoleProcess — session 0 has no desktop to capture), and speaks the same
 * lines over the same three pipes; nothing past start() knows the difference.
 */
class ConsoleProcess;

class StreamWorkerHost : public QObject
{
    Q_OBJECT

public:
    explicit StreamWorkerHost(QObject* parent = nullptr);
    ~StreamWorkerHost() override;

    /// Spawn the worker and hand it the session config. Returns false when the
    /// process could not be started (caller falls back to in-process mode).
    bool start(const QJsonObject& config);

    /// Graceful local teardown ({"cmd":"quit"}), hard-kill after 5s.
    void requestQuit();
    /// Notify the browser it was taken over, then teardown (worker side).
    void notifyTakenOver();
    /// Notify the browser its device was revoked, then teardown (worker side).
    void notifyRevoked();
    /// Notify an invited player that the owner ended the shared session, then
    /// teardown (worker side).
    void notifySessionEnded();

    /// Swap the input policy under a running stream. The owner moved this
    /// player's permissions on the sharing board; the worker applies them to the
    /// live relays and releases whatever the old policy had held down.
    void setInputPolicy(bool gamepad, bool keyboardMouse);

    bool isRunning() const;

signals:
    /// The worker's /start HTTP reply (exactly once — synthesized 502 when the
    /// worker dies without answering).
    void responseReady(int code, QJsonObject body);
    /// The session is over (worker event or process exit) — exactly once.
    void ended();
    /// A co-op backend (Wolf) named the session this worker launched. Recorded
    /// by the supervisor so the session can be closed host-side afterwards: the
    /// worker cannot be trusted to do it, since the case that leaks is exactly
    /// the one where it died without a teardown. At most once, and only on a
    /// backend with native co-op.
    void coopSessionResolved(const QString& sessionId);
    // The IP TTL a host's own packets arrived with — the only thing on the wire
    // that names its OS family. See HostOsProbe.h for what it decides.
    void hostIpTtlObserved(int ttl);
    /// The child process fully exited (ports are certainly free) — exactly
    /// once, always after ended(). Serialization barrier for slot reuse.
    void exited();

private:
    bool startInProcess(const QStringList& args, const QByteArray& configLine);
    bool startInConsoleSession(const QStringList& args, const QByteArray& configLine);
    void onStdoutData(const QByteArray& data);
    void onStderrData(const QByteArray& data);
    void onStdout();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onChildFinished(int exitCode, bool crashed);
    void sendCommand(const char* cmd);
    void sendJson(const QJsonObject& msg);
    void writeLine(const QByteArray& line);
    void killChild();
    qint64 childPid() const;

    QProcess* m_Proc = nullptr;
    ConsoleProcess* m_Console = nullptr;
    QByteArray m_Buf;
    bool m_ResponseEmitted = false;
    bool m_EndedEmitted = false;
};
