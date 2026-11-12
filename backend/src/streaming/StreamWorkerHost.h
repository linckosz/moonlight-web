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
#include <QList>

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
 * On Linux a native worker is the one child handed CAP_SYS_NICE (GPU priority);
 * see common/LinuxCapabilities.h.
 */
class ConsoleProcess;

class StreamWorkerHost : public QObject
{
    Q_OBJECT

public:
    explicit StreamWorkerHost(QObject* parent = nullptr);
    ~StreamWorkerHost() override;

    /// How far the native worker is raised. start() tries them top down, each
    /// step reaching less of the desktop than the one above and none of them
    /// changing what the stream itself does.
    enum class Launch
    {
        /// A plain child of this process, as this process.
        Plain,
        /// From a service, into the console session, as the user sitting there.
        ConsoleAsUser,
        /// From the desktop, through the elevated scheduled task: the user's
        /// full token, so administrator windows take input (§30, level 1).
        TaskElevated,
        /// From the desktop, through the LocalSystem launcher service: SYSTEM
        /// on the console desktop, so the secure desktop does too (level 2).
        ServiceAsSystem,
        /// From the desktop Remote Desktop left, through the launcher service:
        /// SYSTEM on the console's sign-in screen, in sign-in screen mode
        /// (WorkerService::requestSignInWorker). Only for a config that asks
        /// for it (signInScreen), and nothing below it is ever tried.
        SignInScreen,
    };

    /// How a worker gets the UDP ports every stream shares (network/IceMux),
    /// if it gets them at all.
    enum class MuxHandover
    {
        /// It gets none, and binds a port of its own.
        None,
        /// This process hands them over, described for that process alone.
        Handed,
        /// The worker copies them out of this process (streaming/IceMuxHandles).
        Taken,
    };

    /// By the way the worker is started, which says whose process it is.
    ///
    /// A plain child is this process's own account, at its own level: handed.
    /// A worker of the launcher service or of the elevated task is SYSTEM or
    /// this account elevated, above this process, which may not open it:
    /// taken. A worker put in the console session runs as the user sitting
    /// there, who may be another account than the one this installation was
    /// made by, and a socket in a process of theirs is theirs to read and to
    /// send from, whatever the worker itself does with it. So that worker is
    /// handed the sockets only when `ownerAccount` says the token it was
    /// started with is the owner's (ConsoleProcess::runsAsOwner), and gets
    /// none otherwise. The other launches do not depend on it.
    static constexpr MuxHandover muxHandoverFor(Launch how, bool ownerAccount)
    {
        switch (how) {
        case Launch::Plain: return MuxHandover::Handed;
        case Launch::TaskElevated:
        case Launch::ServiceAsSystem: return MuxHandover::Taken;
        case Launch::ConsoleAsUser: return ownerAccount ? MuxHandover::Handed : MuxHandover::None;
        }
        return MuxHandover::None;
    }

    /// Whether a worker started for `config` is one that gets the server's
    /// shared ports when the server has some to give: every one but the
    /// guests' shared feed, which no browser reaches, and a native worker of
    /// a server that is a service when whoever sits at the console is not
    /// the account the installation was made by (muxHandoverFor). What the
    /// server asks itself before it decides whether that stream needs a
    /// router hole of its own; the launch asks again, of its own token.
    static bool getsSharedPorts(const QJsonObject& config);

    /// Spawn the worker and hand it the session config. Returns false when the
    /// process could not be started (caller falls back to in-process mode).
    bool start(const QJsonObject& config);

    /// Spawn it exactly as @p how, or not at all. The guests' shared feed
    /// lives on a pipe only its own user may open, so a guest's worker has to
    /// run as the feed's does — a guest that could only start some other way
    /// encodes on its own instead.
    bool startAs(const QJsonObject& config, Launch how);

    /// The way the running child was started (Plain until one was).
    Launch launch() const { return m_Launch; }

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
    void setInputPolicy(bool gamepad, bool keyboardMouse, bool hid);

    /// A command for the worker's own loop, as it is (the guests' feed:
    /// {"cmd":"codec","codec":"h264"}). Fire-and-forget, like the policy.
    void sendControl(const QJsonObject& command) { sendJson(command); }

    bool isRunning() const;

    /// Whether the child went without saying its session was over and without
    /// having been asked to stop (StreamEnds::stateAfterExit): it crashed, or
    /// something outside killed it. Settled before ended() is emitted.
    bool crashed() const { return m_Crashed; }

    /// The id this stream's page knows it by (StreamEnds), set by the
    /// supervisor that starts it.
    void setStreamId(const QString& id) { m_StreamId = id; }
    const QString& streamId() const { return m_StreamId; }

    /// The app whose sound a running stream carries under "Stream audio from"
    /// ("huntgame.exe"), as its worker last reported it (an
    /// `audioFocus` event); empty when no worker reports one. For Settings.
    static QString audioFocusApp();

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
    /// The desktop portal granted this installation a consent to remember, so
    /// the next session opens without a dialog. At most once per worker, and
    /// only on a machine that captures through the portal (Linux, no capability
    /// to read the scanout — an AppImage). Persisted by the supervisor: the
    /// worker has no settings file of its own.
    void portalGrantReceived(const QString& token);
    /// The capture's verdict on the display it held over the session (the
    /// native engine's CaptureHold: `held`, `display_mhz`, `gpu`, `why` and the
    /// figures), for `vddmatch`. At most once, just before ended(), and only
    /// when there was enough dense content to judge. Persisted by the
    /// supervisor, like the portal's grant.
    void captureHoldReceived(const QJsonObject& verdict);
    /// The child process fully exited (ports are certainly free) — exactly
    /// once, always after ended(). Serialization barrier for slot reuse.
    void exited();

private:
    /// The arguments and the configuration line every way of starting shares.
    void prepare(const QJsonObject& config, QStringList& args, QByteArray& configLine) const;

    bool startInProcess(const QStringList& args, const QByteArray& configLine, bool native);
    /// Through ConsoleProcess, in one of the three ways above.
    bool startInConsoleSession(const QStringList& args, const QByteArray& configLine, Launch how);
    /// The configuration line with the UDP ports every stream shares added
    /// ("iceMux"), for a worker that streams to a browser while the server
    /// shares ports (network/IceMux), the way muxHandoverFor() says for the
    /// worker started as `how`, on the owner's account or not: described for
    /// its process `pid` alone, or named for it to copy out of this process
    /// itself. The line as it was when the worker gets none, or nothing can
    /// be shared: it binds a port of its own.
    QByteArray withSharedSockets(const QByteArray& configLine, Launch how, bool ownerAccount,
                                 qint64 pid);
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
    /// Puts back the Mouse Keys a worker died holding, from this process when
    /// it sits in the console session — see m_MouseKeysHeld.
    void restoreMouseKeysAfterWorker();

    QProcess* m_Proc = nullptr;
    ConsoleProcess* m_Console = nullptr;
    Launch m_Launch = Launch::Plain;
    QByteArray m_Buf;
    bool m_ResponseEmitted = false;
    bool m_EndedEmitted = false;
    /// A quit or one of the notices was sent: whatever follows is no crash.
    bool m_StopAsked = false;
    bool m_Crashed = false;
    QString m_StreamId;
    /// The kept wheels handed to this worker (HidParkingTransfer), until it
    /// says it took them; 0 when none are out.
    uint64_t m_HidBatch = 0;
    /// The worker's ICE agent on the shared UDP ports (network/IceMux), by its
    /// feed number there; 0 when it has none.
    uint32_t m_MuxFeed = 0;
    /// The mux's sockets this worker was handed, in the order it was given
    /// them: the ones its agents send on. Empty when it was handed none.
    QList<quint8> m_MuxSockets;
    /// The mux's lease on those sockets (IceMux::Share), from the moment the
    /// worker is given them until it is gone: the mux keeps them bound and
    /// read meanwhile, whatever port replaces them. 0 when it was handed none.
    uint32_t m_MuxLease = 0;
    void onMuxEvent(const QJsonObject& event);
    void dropMuxAgent();
    /// The worker is gone, or is about to be started again: its agent goes,
    /// and what the mux kept for it is given back.
    void releaseMuxSockets();
    /// Mouse Keys the worker turned on and has not let go of yet, with what to
    /// put back if it dies holding them (a `mouseKeys` event; Windows only).
    bool m_MouseKeysHeld = false;
    QJsonObject m_MouseKeysSaved;
    /// This worker reported the app in front (audioFocusApp): cleared when it
    /// exits, so a finished stream never shows a stale name.
    bool m_ReportedAudioFocus = false;
};
