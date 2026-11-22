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
#include <QTimer>

#include <functional>

/// The admin page's box "One shared port for streams" is applied by a new
/// process: the streams' shared ports are bound when the server starts, and
/// their router holes with them. So a write of it that makes the file differ
/// from what runs asks for a restart, done as soon as nothing would be cut by
/// it: no stream (running, being set up, or a guest's), no pairing (a host's
/// PIN, a device's QR code, a TV's), no install step, no update.
///
/// Only that write asks (asked(), from POST /api/admin/settings). One restart
/// at most is waited for. Before it is done, the wish is dropped and the file
/// read again: a box ticked back meanwhile asks for nothing. Nothing of it is
/// kept on disk, so the new process never restarts for it again, even when its
/// shared ports cannot be bound.
class SharedPortsRestart : public QObject
{
    Q_OBJECT
public:
    enum class Step
    {
        Nothing, ///< no wish, or the file is what runs
        Wait,    ///< something would be cut: asked again later
        Restart,
    };
    /// The rule, free of the server.
    static Step decide(bool wished, bool fileWantsShared, bool sharedRun, bool busy);

    struct Probes
    {
        /// What the file asks now (AppSettings::iceUdpMux)
        std::function<bool()> fileWantsShared;
        /// Whether this process runs the shared ports (IceMux::shared)
        std::function<bool()> sharedRun;
        /// Whether a restart now would cut something (see above)
        std::function<bool()> busy;
        /// Restart the server (mw::restart::now); false when it could not
        std::function<bool()> restart;
    };
    explicit SharedPortsRestart(Probes probes, int lookEveryMs = 5000, QObject* parent = nullptr);

    /// The one this server has, or none (tests, other processes).
    static SharedPortsRestart* shared() { return s_Shared; }
    static void setShared(SharedPortsRestart* one) { s_Shared = one; }

    /// The box was just written. Looks at once, then every few seconds while
    /// it has to wait.
    void asked();
    /// A restart is waited for.
    bool pending() const { return m_Wished; }

private:
    void look();

    Probes m_Probes;
    QTimer m_Timer;
    bool m_Wished = false;
    bool m_SaidWaiting = false;
    static SharedPortsRestart* s_Shared;
};
