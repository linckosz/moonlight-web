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

#include "../../streaming/ConsoleSession.h"
#include "mw/native/Capabilities.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

class ConsoleProcess;

/**
 * What the native engine can do on this machine — answered from wherever the
 * desktop is.
 *
 * On a desktop (a dev instance, a manual launch) this is NativeHost::probe(),
 * called every time: it is cheap, and a fresh answer is what lets a display
 * that was just unplugged disappear from the list before someone clicks it.
 *
 * As a service, the probe cannot run here (session 0 has no desktop — see
 * ConsoleSession.h), so it runs in the console session as `MoonlightWeb
 * --native-probe`, a child that prints one JSON object and exits. A process
 * launch is not something to do on every host-list request, hence the
 * snapshot: the last answer is kept, handed out immediately, and renewed in
 * the background when it is older than a few seconds and someone asks, when
 * the console user changes (logon, logoff — watched every few seconds without
 * spawning anything), and at startup. changed() fires when the answer moved,
 * which is what puts the host card up when the user logs on and takes it down
 * when they log off.
 *
 * Before the first answer arrives the snapshot says "unavailable, waiting for
 * the console probe": a host that appears a second after the service starts is
 * fine; a host that is offered and fails is not.
 */
class NativeProbeService : public QObject
{
    Q_OBJECT

public:
    static NativeProbeService& instance();

    /// The current answer. Never blocks. As a service, a stale snapshot is
    /// renewed in the background and the previous one returned meanwhile.
    mw::native::Capabilities snapshot();

    /// True when the desktop is in another session: answers are snapshots of a
    /// probe process, not live calls.
    bool remote() const { return m_Remote; }

    /// Ask again now (no-op while a probe is in flight, or on a desktop).
    void refresh();

    /// What was last seen of the console session (remote only).
    ConsoleSession::Info console() const { return m_Console; }

    /// How old the snapshot is, in ms; -1 before the first answer.
    qint64 snapshotAgeMs() const;

    /// `MoonlightWeb --native-probe`: run the probe here, print it as JSON on
    /// stdout, exit 0. The engine's own log goes to stderr.
    static int runProbeCommand();

signals:
    /// The snapshot changed in a way the host list can see (availability, a
    /// display, a GPU). Remote only.
    void changed();

private:
    NativeProbeService();

    void onWatchTick();
    void apply(const mw::native::Capabilities& caps);
    void probeFinished(int exitCode, bool crashed);
    void probeTimedOut();

    bool m_Remote = false;
    bool m_HaveResult = false;
    mw::native::Capabilities m_Caps;
    QElapsedTimer m_Age;
    ConsoleSession::Info m_Console;

    ConsoleProcess* m_Probe = nullptr;
    QByteArray m_ProbeOut;
    QByteArray m_ProbeErr;
    QTimer m_ProbeTimeout;
    QTimer m_Watch;
};
