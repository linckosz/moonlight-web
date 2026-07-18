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
 */
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

    bool isRunning() const;

signals:
    /// The worker's /start HTTP reply (exactly once — synthesized 502 when the
    /// worker dies without answering).
    void responseReady(int code, QJsonObject body);
    /// The session is over (worker event or process exit) — exactly once.
    void ended();
    /// The child process fully exited (ports are certainly free) — exactly
    /// once, always after ended(). Serialization barrier for slot reuse.
    void exited();

private:
    void onStdout();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void sendCommand(const char* cmd);

    QProcess* m_Proc = nullptr;
    QByteArray m_Buf;
    bool m_ResponseEmitted = false;
    bool m_EndedEmitted = false;
};
