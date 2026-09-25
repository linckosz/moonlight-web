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

#include "backend/VirtualDisplay.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <optional>

/**
 * @brief Turning "MoonlightWeb Virtual Display" on and off, from a stream's
 * start to the display being there — and back.
 *
 * One operation at a time, fully asynchronous on the Qt main thread: write the
 * request → reach the elevated helper (see VirtualDisplay.h for the three
 * ways) → wait for its result → re-probe the engine → persist. Callers pass a
 * callback; several streams asking for the same thing at once share one
 * operation, and a request that arrives while another runs waits its turn.
 *
 * The off switch is deferred (releaseSoon): a page reload or a quality
 * switch ends one worker and starts another within a second, and toggling a
 * display — moving every window twice — for that would be worse than
 * leaving it on a moment longer.
 */
class VirtualDisplayJob : public QObject
{
    Q_OBJECT

public:
    using Callback = std::function<void(bool ok, const QString& error)>;

    static VirtualDisplayJob& instance();

    enum class State
    {
        Idle,
        Elevating,
        Applying,
        Configuring,
        Refreshing,
        Done,
        Failed
    };

    /// Turn the display on (enable, mode, primary). Answers at once when it
    /// is already on IN THAT MODE. Cancels a pending releaseSoon().
    ///
    /// @p width × @p height is the size the client asked the display to have
    /// ("Match my screen"); 0×0 takes whatever mode is there, which is what
    /// every other resolution choice does. @p refresh is the rate the
    /// client's own screen runs at, which the display takes whatever the
    /// resolution choice; 0 is the default rate. A display already on in
    /// another mode is put through the operation again — the driver's mode
    /// list is rewritten and the node restarted — because a mode that is not
    /// the client's is the one thing this choice cannot live with.
    ///
    /// @p hdr: the viewer asked for HDR. macOS makes its display able to
    /// show it (EDR) only then — an SDR stream off a display declared in
    /// BT.2020 would come out washed. Windows ignores it: its display is SDR.
    void activate(int width, int height, int refresh, bool hdr, Callback cb);
    void activate(Callback cb) { activate(0, 0, 0, false, std::move(cb)); }

    /// Turn it off (previous primary back, disable). @p cb may be null.
    void deactivate(Callback cb);

    /// Turn it off in a few seconds unless activate() comes first.
    void releaseSoon();

    /// Asked when that grace runs out: true while a stream still shows the
    /// display, which then stays on. A take-over asks for the display
    /// (cancelling the grace) BEFORE the stream it replaces is torn down, and
    /// that teardown starts the grace again — under the new stream.
    void setInUse(std::function<bool()> inUse) { m_InUse = std::move(inUse); }

    bool running() const;

    /// {state, action, error?, started_at, finished_at?, display?}
    QJsonObject statusJson() const;

private:
    explicit VirtualDisplayJob(QObject* parent = nullptr);

    struct Pending
    {
        VirtualDisplay::Request::Action action;
        int width = 0;
        int height = 0;
        int refresh = 0;
        bool hdr = false;
        Callback cb;
    };

    void enqueue(VirtualDisplay::Request::Action action, int width, int height, int refresh,
                 bool hdr, Callback cb);
    void startNext();
    void setState(State s);
    void fail(const QString& error);
    void succeed(const VirtualDisplay::Result& res);
    void settle(bool ok, const QString& error);

    void dispatch();
    void applyInProcess();
    void makeMain();
    void runHelper(const QStringList& args, bool inConsoleSession);
    void runTask();
    void pollResult();
    void handleResult(const VirtualDisplay::Result& res);

    State m_State = State::Idle;
    VirtualDisplay::Request m_Request;
    QList<Callback> m_Callbacks; ///< who asked for the running operation
    QList<Pending> m_Queue;      ///< what comes after it
    QString m_Error;
    QString m_Display;
    /// The mode the display was last turned on in by this process, so a
    /// second viewer asking for the same one joins instead of switching it.
    int m_ActiveWidth = 0;
    int m_ActiveHeight = 0;
    int m_ActiveRefresh = 0;
    bool m_ActiveHdr = false;
    QDateTime m_StartedAt;
    QDateTime m_FinishedAt;

    QTimer m_Poll;
    QTimer m_Deadline;
    QTimer m_Release;
    std::function<bool()> m_InUse;
    QByteArray m_HelperOut;
    // The service path runs the node stage as SYSTEM and the desktop stages
    // in the console session, one child each; these are the ones still to run.
    QStringList m_NextStages;
    // macOS: the request was applied in this process and the poll is waiting
    // for the OS to list the display; this is the result to deliver then.
    std::optional<VirtualDisplay::Result> m_InProcessResult;
};
