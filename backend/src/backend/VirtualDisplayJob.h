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
#include <QElapsedTimer>
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
    /// every other resolution choice does. @p refreshMilliHz is the rate the
    /// stream chose for the display (main.cpp: 240 Hz on Windows, or the
    /// client's screen matched under `vddmatch`), which the display takes
    /// whatever the resolution choice; 0 is the default rate. A display already on in
    /// another mode is put through the operation again — the driver's mode
    /// list is rewritten and the node restarted — because a mode that is not
    /// the client's is the one thing this choice cannot live with.
    ///
    /// @p hdr: the viewer asked for HDR. macOS makes its display able to
    /// show it (EDR) only then — an SDR stream off a display declared in
    /// BT.2020 would come out washed. Windows ignores it: its display is SDR.
    ///
    /// The owner's. @p othersOff: this stream wants the other displays off
    /// (VirtualDisplay::ownerWantsOthersOff: the admin allows it and the
    /// viewer chose it). They go off when this runs an operation and no guest
    /// is on the display (othersGoOff). On already in that mode, it does not
    /// run one and they stay as they are.
    void activate(int width, int height, int refreshMilliHz, bool hdr, bool othersOff, Callback cb);
    void activate(Callback cb) { activate(0, 0, 0, false, false, std::move(cb)); }

    /// For an invited guest, who arrives with no owner stream on it: turn the
    /// display on, in that mode, when it is off. On already — whatever its
    /// mode — or on its way on, it is left as it is and answers once it is
    /// there: another guest may be watching it, and a new mode would take it
    /// from them for as long as the driver restarts. SDR (Windows' display
    /// is, and a guest's stream is). Cancels a pending releaseSoon(). Never
    /// turns the other displays off (VirtualDisplay::othersGoOff); left off by
    /// an owner who is gone, they come back on before this answers.
    void activateIfOff(int width, int height, int refreshMilliHz, Callback cb);

    /// Turn it off (previous primary back, disable). @p cb may be null.
    void deactivate(Callback cb);

    /// Turn it off in a few seconds unless activate() comes first. A longer
    /// @p graceMs holds it for a stream that is coming back (its worker
    /// crashed, and the page starts it again); a pending longer grace is never
    /// cut short by a later call.
    void releaseSoon(int graceMs = 0);

    /// Asked when that grace runs out: true while a stream still shows the
    /// display, which then stays on. A take-over asks for the display
    /// (cancelling the grace) BEFORE the stream it replaces is torn down, and
    /// that teardown starts the grace again — under the new stream.
    void setInUse(std::function<bool()> inUse) { m_InUse = std::move(inUse); }

    /// Which side streams the display. With the owner gone and a guest on it,
    /// the other displays come back on when that grace runs out or a guest
    /// arrives; with a guest on it, the owner's stream leaves them on.
    void setOwnerOn(std::function<bool()> ownerOn) { m_OwnerOn = std::move(ownerOn); }
    void setGuestOn(std::function<bool()> guestOn) { m_GuestOn = std::move(guestOn); }

    /// Asked when the display is about to go off for good (no stream left
    /// once the grace ran out; on Linux, at the stream's end): closes what
    /// was opened on it, so it does not land on a physical screen instead
    /// (HostAppLauncher: Steam leaves Big Picture). True when it closed
    /// something — the display then waits kCloseSettleMs before going off,
    /// for that window to be gone.
    void setBeforeRelease(std::function<bool()> beforeRelease)
    {
        m_BeforeRelease = std::move(beforeRelease);
    }
    static constexpr int kCloseSettleMs = 3000;

    bool running() const;

    /// The rate, in millihertz, this process last turned the display on at
    /// and that it is still on at; 0 when it is off or that rate is unknown.
    int activeRefreshMilliHz() const { return m_ActiveRefresh; }

    /// {state, action, error?, started_at, finished_at?, display?}
    QJsonObject statusJson() const;

private:
    explicit VirtualDisplayJob(QObject* parent = nullptr);

    struct Pending
    {
        VirtualDisplay::Request::Action action;
        int width = 0;
        int height = 0;
        int refreshMilliHz = 0;
        bool hdr = false;
        bool exclusive = false; ///< VirtualDisplay::Request::exclusive
        Callback cb;
    };

    void activateAs(VirtualDisplay::Asker asker, int width, int height, int refreshMilliHz,
                    bool hdr, bool othersOff, Callback cb);
    /// The other displays are off and the owner's stream that wanted them so
    /// is gone (VirtualDisplay::othersComeBack).
    bool othersDueBack() const;
    void enqueue(VirtualDisplay::Request::Action action, int width, int height, int refreshMilliHz,
                 bool hdr, bool exclusive, Callback cb);
    void startNext();
    void setState(State s);
    void fail(const QString& error);
    void succeed(const VirtualDisplay::Result& res);
    /// Windows: the guard that brings the other displays back if this
    /// process dies during an exclusive stream (VirtualDisplayApply::guard).
    void startGuard();
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
    int m_ActiveRefresh = 0; ///< millihertz
    bool m_ActiveHdr = false;
    QDateTime m_StartedAt;
    QDateTime m_FinishedAt;

    QTimer m_Poll;
    QTimer m_Deadline;
    QTimer m_Release;
    /// Since when runTask() waits for the task's previous run to end.
    QElapsedTimer m_TaskBusySince;
    std::function<bool()> m_InUse;
    std::function<bool()> m_OwnerOn;
    std::function<bool()> m_GuestOn;
    std::function<bool()> m_BeforeRelease;
    QByteArray m_HelperOut;
    // The service path runs the node stage as SYSTEM and the desktop stages
    // in the console session, one child each; these are the ones still to run.
    QStringList m_NextStages;
    // macOS: the request was applied in this process and the poll is waiting
    // for the OS to list the display; this is the result to deliver then.
    std::optional<VirtualDisplay::Result> m_InProcessResult;
    /// The mode the caller asked for, while fail() walks the request towards
    /// the default one (VirtualDisplay::fallbackTowardsDefault).
    std::optional<VirtualDisplay::Request> m_AskedBeforeFallback;
};
