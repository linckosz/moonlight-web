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

#include "FrameSentSink.h"
#include "IMediaEngine.h"

#include "mw/native/FpsStep.h"
#include "mw/native/HidPassthrough.h"
#include "mw/native/LinkFeedback.h"
#include "mw/native/SessionConfig.h"
#include "mw/native/StageStats.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace mw::native {
class Session;
struct CursorUpdate;
struct EncodedFrame;
namespace feed {
struct Header;
} // namespace feed
} // namespace mw::native

class FeedPublisher;
class FeedSubscriber;
class InputWatchdog;

/**
 * @brief The media engine backed by this machine's own screen.
 *
 * The Qt-side adapter over `backend/native-host/`. It is deliberately thin: the
 * engine below knows nothing of Qt, and everything above it — the three relays,
 * the WSS fallback, the input watchdog — already speaks to IMediaEngine. So all
 * this class does is translate, and it must not accumulate policy of its own.
 *
 * ── What it does NOT do, and why ────────────────────────────────────────────
 *
 * No queue between the engine and the relay. The engine hands over a frame on
 * its capture thread and the relay sends it before returning, which is exactly
 * the arrangement the GameStream path spends a thread hop to approximate. That
 * hop — worker thread → queued Qt signal → relay thread — is one of the costs
 * this whole module exists to remove, so re-adding it here would be self-
 * defeating.
 *
 * ── Metrics ─────────────────────────────────────────────────────────────────
 *
 * `hostRttMs()` is 0 and `hostIpTtl()` is 0: there is no host to reach and no
 * datagram to inspect. Reporting a fabricated number would be worse than an
 * absent one, and the stats overlay already handles a zero.
 *
 * `takeHostProcessingLatencyMs()` is the one metric that gets BETTER: on the
 * GameStream path it is whatever the remote host chose to report, while here it
 * is measured — the real present→encoded time of every frame in the window.
 *
 * And it goes further: every frame carries its stamps from the display's
 * present to the encoder's output, and the sender thread reports when the last
 * fragment left (FrameSentSink). Six stages, mean and tail, in the stats
 * message every window and in the log once at the end of the session. Nothing
 * on this path is optimized on a guess.
 */
class NativeMediaEngine : public IMediaEngine, public FrameSentSink
{
    Q_OBJECT

public:
    /// Everything a native session needs. Small on purpose: the only genuinely
    /// user-chosen field is the display (§13 of the mission).
    struct StartParams
    {
        int displayId = -1;
        int width = 0;  ///< 0 = the display's native width
        int height = 0; ///< 0 = the display's native height
        int fps = 0;    ///< 0 = the display's own refresh rate
        /// A rate the session may not exceed, alignment included (0 = none) —
        /// see SessionConfig::maxFps.
        int maxFps = 0;
        int bitrateKbps = 20000;
        /// VIDEO_FORMAT_* bitmask of what the browser can decode.
        int clientVideoFormats = 0;
        bool hdr = false;
        bool yuv444 = false;
        /// Encode with intra-refresh. Only worth asking when the receiver will
        /// decode through a gap — see rideOutLoss in MediaDescriptor.h.
        bool intraRefresh = false;
        /// Heal a lost frame by a reference repair (long-term references,
        /// NVENC's invalidated DPB). False when the client's decoder falls
        /// silent under them — see refInvalidation in MediaDescriptor.h.
        bool refInvalidation = true;
        /// The client cuts each decoded frame to the announced size — see
        /// SessionConfig::clientCropsToFrame.
        bool cropsToFrame = false;
        /// The client reads MWSR's sharpness by zone (`mwsr_map` at /start):
        /// the frames carry their grid — see SessionConfig::mwsrZones.
        bool mwsrZones = false;
        /// Rebuild at the display's new shape when its mode changes under the
        /// session — see SessionConfig::followDisplayShape.
        bool followDisplayShape = false;
        /// width × height is a box to fit (the display's shape inside it),
        /// and whether the frame may then be larger than the display — see
        /// SessionConfig::fitRequestedBox and allowUpscale.
        bool fitRequestedBox = false;
        bool allowUpscale = false;
        /// Put the display in the requested mode for the session — see
        /// SessionConfig::matchClientDisplay.
        bool matchClientDisplay = false;
        /// The box fitted instead when that mode cannot be set — see
        /// SessionConfig::fallbackWidth.
        int fallbackWidth = 0;
        int fallbackHeight = 0;
        /// The client's screen, as the browser measured it: refresh in
        /// millihertz (0 = unknown) and whether it paints on vsync. A vsync
        /// client gets a cadence that divides its refresh — see
        /// SessionConfig::clientRefreshMilliHz.
        int clientRefreshMilliHz = 0;
        bool clientVsync = false;
        /// And whether its tearing is real — see
        /// SessionConfig::clientTearingReal.
        bool clientTearingReal = false;
        /// The viewer administers MoonlightWeb on this machine (reached it
        /// from the host itself, or unlocked the admin password). Anyone
        /// else is kept out of windows that run as administrator — see
        /// SessionConfig::allowElevatedInput.
        bool viewerAdmin = true;
        /// Silence the host's speakers while the session runs — the same
        /// stream setting GameStream hosts receive as localAudioPlayMode.
        bool muteHostAudio = true;
        /// The desktop portal consent this machine was granted last time, if
        /// any — see SessionConfig::portalRestoreToken. Empty means the user
        /// will be asked; whatever comes back arrives as portalGrantReceived().
        /// Ignored on every route but the Linux portal one.
        QString portalRestoreToken;
        /// The owner's stream on the virtual display card: the monitor the
        /// session makes (Linux) becomes the desktop's primary while it
        /// streams — see SessionConfig::virtualPrimary. Never a guest's.
        bool virtualPrimary = false;
        /// One of the owner's apps in its own gamescope (Linux): its name and
        /// command, from the settings — see SessionConfig::gamescopeApp. Empty
        /// on every other card.
        QString gamescopeApp;
        QString gamescopeCommand;
        /// The chain a Windows session carries its pictures on, as the admin
        /// chose it (Advanced) — see SessionConfig::videoPipeline. Auto unless
        /// a choice was made; ignored off Windows.
        mw::native::VideoPipeline videoPipeline = mw::native::VideoPipeline::Auto;
        /// The bench's keys from the settings file, as the server read them at
        /// /start (AppSettings::nativeTuning): the way to a session the
        /// environment cannot reach. Taken only when MW_NATIVE_TUNING is absent
        /// from this process's own; empty unless someone added it by hand.
        QString tuningSpec;

        // ── The guests' shared feed (plan « flux commun des invités ») ─────
        //
        // A native host's guests watch ONE stream, encoded once by the feed
        // worker. That worker's engine publishes every frame to its pipe; each
        // guest's engine takes its pictures from that pipe instead of a
        // capture, and its session only injects input and captures audio.

        /// The feed worker's: where every encoded frame goes, beside the
        /// relay if there is one, and what the session is (its `info`). Not
        /// owned; must outlive the session.
        FeedPublisher* feedPublisher = nullptr;
        /// A guest worker's: the feed's pipe and the token it asks first.
        /// Non-empty turns this engine into a subscriber.
        QString feedPipe;
        QByteArray feedToken;
        /// The guest's slot on the share board, for the feed's log.
        int feedSlot = -1;
        /// SessionConfig::intraRefreshRequired and ::governorFloorPercent —
        /// the feed's own two.
        bool intraRefreshRequired = false;
        int governorFloorPercent = 20;
        /// Capture the host's audio. The feed worker does not: each guest's
        /// own worker captures it for its browser.
        bool captureAudio = true;
    };

    /// One encoded frame, borrowed: `data` is the encoder's own output buffer
    /// and is valid only until the sink returns. Whatever must outlive the call
    /// (a keyframe kept for a channel that is not open yet) is copied by the
    /// sink, explicitly — nothing here is copied on its behalf.
    struct FrameView
    {
        const uint8_t* data = nullptr;
        size_t size = 0;
        bool keyframe = false;
        uint32_t frameNumber = 0;
        /// µs since the session's first frame — the same relative stamp the
        /// videoFrameReady signal carries.
        int64_t presentationTimeUs = 0;
        /// MWSR's zones of the picture, borrowed like `data` — see
        /// mw::native::EncodedFrame::mwsrZones. Null when it has none. Only
        /// this path carries them: the signal's listeners (the WebSocket
        /// fallback, the media track) and the guests' feed send none.
        const uint8_t* mwsrZones = nullptr;
        int mwsrZonesX = 0;
        int mwsrZonesY = 0;
        float mwsrScale = 0.0f;
    };
    using FrameSink = std::function<void(const FrameView&)>;

    explicit NativeMediaEngine(QObject* parent = nullptr);
    ~NativeMediaEngine() override;

    /// Hand every encoded frame to @p sink, synchronously on the capture
    /// thread, INSTEAD of copying it into a QByteArray and emitting
    /// videoFrameReady. This is the zero-copy path: the relay fragments
    /// straight out of the encoder's buffer.
    ///
    /// Only one consumer can have it, and it takes the signal away from every
    /// other listener — so it is for the relay that owns the session's video,
    /// and it must be cleared (nullptr) before that relay stops caring, at
    /// which point the signal path resumes (the WebSocket fallback relies on
    /// exactly this). Clearing blocks until a frame in flight has left the
    /// sink, so once it returns the sink is never called again. Safe at any
    /// time, from any thread — but never from inside the sink itself.
    void setDirectFrameSink(FrameSink sink);

    /// Build and start the pipeline. Not part of IMediaEngine, for the same
    /// reason MoonlightShim::startConnection is not: the parameters are this
    /// engine's own.
    ///
    /// Emits connectionStarted() on success and connectionFailed() otherwise,
    /// so the caller can treat both engines identically from there on.
    void startCapture(const StartParams& params);

    // ── IMediaEngine ────────────────────────────────────────────────────────

    void stopConnection() override;
    void interruptConnection() override;
    bool isConnected() const override { return m_Connected.load(std::memory_order_acquire); }

    void requestIdrFrame() override;
    void videoFrameDelivered() override;
    bool takeWorkerDroppedDelta() override;
    int64_t workerDropCount() const override;
    int pendingVideoFrames() const override;
    int negotiatedVideoFormat() const override;

    int audioSamplesPerFrame() const override;

    void sendKeyEvent(short keyCode, bool down, char modifiers, char flags,
                      bool hold = false) override;
    void sendUtf8Text(const QString& text) override;
    void sendSecureAttention() override;
    void sendKeyChar(const QString& ch, bool down, char modifiers = 0,
                     const CharKeyHints& hints = {}) override;
    void sendMouseMove(short deltaX, short deltaY) override;
    void sendMousePosition(short x, short y, short referenceWidth, short referenceHeight) override;
    void sendMouseButton(bool down, int button, bool hold = false) override;
    void sendMouseScroll(short scrollAmount) override;
    void sendMouseHScroll(short scrollAmount) override;
    void sendControllerArrival(uint8_t controllerNumber, uint16_t activeGamepadMask, uint8_t type,
                               bool hasRumble) override;
    void sendControllerState(short controllerNumber, short activeGamepadMask, int buttonFlags,
                             unsigned char leftTrigger, unsigned char rightTrigger,
                             short leftStickX, short leftStickY, short rightStickX,
                             short rightStickY) override;
    void sendControllerRemoval(uint8_t controllerNumber, uint16_t activeGamepadMask) override;
    /// The HID passthrough: devices the page reads through WebHID, recreated
    /// here by mw::native::HidPassthrough (made at the first attach).
    QString hidUnavailableReason() const override;
    QString hidAttach(int slot, const QJsonObject& message) override;
    void hidAttached(int slot) override;
    void hidInput(const uint8_t* frame, size_t size) override;
    void hidDetach(int slot) override;
    bool hidForceFeedback() const override;
    void hidReply(int slot, int reportId, const QByteArray& data) override;
    bool hidRelaysHidpp() const override;
    /// Kept at the browser's own numbering. A native session owns its virtual
    /// pads — one table per worker, see VigemGamepad — so concurrent sessions
    /// never meet on one pad; the shift that keeps GameStream sessions apart
    /// only pushed a guest's second pad past that table, and sent its
    /// vibration back under a number its page does not have.
    void setControllerOffset(int) override {}
    void syncLockKeys(bool numLock, bool capsLock, bool scrollLock) override;
    void syncHeldInputs(const QVector<HeldKey>& keys, quint32 buttonMask,
                        bool buttonsHold) override;
    void releaseHeldInputs(bool includeHold) override;
    qint64 noteClientAlive() override;

    double takeHostProcessingLatencyMs() override;
    int64_t frameSubmitTimeUs() const override;
    int64_t framePresentationTimeUs() const override;
    int64_t firstFrameArrivalSteadyMs() const override;
    FrameSentSink* frameSentSink() override { return this; }
    QJsonObject takeStageStats() override;
    QJsonObject captureHoldReport() const override { return m_CaptureHold; }

    // ── FrameSentSink ───────────────────────────────────────────────────────
    void frameSent(uint32_t frameNumber, int64_t firstByteUs, int64_t lastByteUs) override;

    /// Whether the engine draws the mouse pointer into the picture (gaming mode,
    /// and every touch screen) or reports its shape for the browser to draw
    /// (desktop). @p cursorFramePx is how wide the drawn pointer should be, in
    /// frame pixels, or 0 for its natural size — see Session. Safe at any time.
    void setCompositeCursor(bool composite, int cursorFramePx);

    /// How fast the client wants frames to keep coming while nothing on the
    /// host's screen moves — see Session::setFrameFloorFps. 0 leaves the engine
    /// its own liveness floor.
    ///
    /// Remembered as well as forwarded: the input channel opens before the
    /// capture does, so the client's first word on the subject usually arrives
    /// with no session to tell. It is deduplicated on the client, which would
    /// then never say it again. Safe at any time.
    void setFrameFloorFps(int fps);

    /// The viewer pressing the way out of a closed input gate — see
    /// Session::releaseInputBlock. Dropped when no session runs; there is
    /// nothing to unblock then.
    void releaseInputBlock();

    /// The receiver's report on the link (a `linkstats` message on the input
    /// channel, or the RTP counters of `clientstats`), handed to the engine's
    /// rate governor with the frames our own sender evicted since the last
    /// report folded in — see Session::reportLink. Safe from any thread;
    /// dropped when no session runs.
    void reportLink(const mw::native::LinkFeedback& feedback);

    /// The receiver never got frame @p frameNumber (the engine's own number,
    /// EncodedFrame::frameNumber — the relay maps its wire ids to it): the
    /// encoder predicts the next pictures from older frames and the stream
    /// heals with a delta instead of a keyframe (Session::invalidateReference).
    /// A keyframe instead on the shared feed and for a client that refuses
    /// repairs (StartParams::refInvalidation false). Safe from any thread;
    /// dropped when no session runs.
    void invalidateReference(uint32_t frameNumber);

    /// Whether the session's encoder really heals a lost frame with a delta —
    /// what the receiver needs to know before it keeps decoding through a gap.
    /// False until a session started, false on encoders without it, false for
    /// a client that refuses repairs.
    bool referenceInvalidation() const;

    /// Whether the delta the relay drops when the link stops draining is
    /// named to invalidateReference too (EncoderTuning::nameLinkDrops, plan
    /// §9-25) — on top of referenceInvalidation(), which the relay also asks.
    /// The key when a bench gave one; otherwise the engine's own for the
    /// running encoder (nameLinkDropsByDefault), false until a session started.
    bool nameLinkDrops() const;

    /// The relay's sender dropped a frame because the link had not taken the
    /// previous ones. Counted here, from the relay's thread, and carried by the
    /// next reportLink(): to the governor an eviction is loss the host caused
    /// itself, and the surest sign the link is full.
    void noteEviction() { m_Evictions.fetch_add(1, std::memory_order_relaxed); }
    /// The relay shed a frame off a send buffer backed up past its tolerance
    /// ("link not draining"). Carried by the next reportLink() as
    /// LinkFeedback::overflows, which the governor reads unless ovfgood=0.
    void noteOverflow() { m_Overflows.fetch_add(1, std::memory_order_relaxed); }

    /// The client's screen changed mid-session (a `clientrefresh` message):
    /// its refresh in millihertz and whether it paints on vsync. The engine
    /// re-chooses the stream's cadence against it — see
    /// Session::setClientRefresh. Safe from any thread; remembered for a
    /// session that starts after it was said.
    void setClientRefresh(int milliHz, bool vsync);

    /// The client asks for no more than @p fps (a `clientfpscap` message, 0
    /// lifts it): forwarded to Session::setClientFpsCap. Safe from any thread.
    void setClientFpsCap(int fps);

    /// Where the client's decode queue stands (a `decodequeue` message):
    /// forwarded to Session::setClientDecodeQueue. Safe from any thread.
    void setClientDecodeQueue(int depth);

    /// The relay's view of its send queue, for the bench's linkhold=:
    /// forwarded to Session::setLinkBusyProbe, and remembered for a session
    /// that starts after it was set. Empty takes it back. Safe from any thread.
    void setLinkBusyProbe(std::function<bool()> probe);

    /// When the client's screen refreshes (a `vsyncgrid` message): forwarded
    /// to Session::setClientVsyncGrid. Safe from any thread.
    void setClientVsyncGrid(double periodUs, int64_t phaseUs, int64_t leadUs, bool tearing,
                            bool steady, double budgetFps);

    /// What the session says about the client's grid, for the pong (see
    /// Session::vsyncGridStatus): whether it wants one, whether it follows
    /// it and aims at it, the host display's period in µs. Safe from any
    /// thread.
    struct VsyncGridStatus
    {
        bool wanted = false;
        bool followed = false;
        bool aimed = false;
        int presentUs = 0;
    };
    VsyncGridStatus vsyncGridStatus() const;

    /// "Auto"'s detection asks for the stream to run at @p fps, above the
    /// client's own rate, or with 0 to come back to it (an `fpsstep`
    /// message): forwarded to Session::setClientFpsStep, whose answer is the
    /// reply. A guest on the shared feed is refused — the feed's pace is the
    /// feed's. Safe from any thread.
    mw::native::FpsStep setClientFpsStep(int fps);

    /// What the detection reads of the cadence, for the stats (see
    /// Session::cadenceStatus); zero-filled with no session of our own.
    /// Safe from any thread.
    mw::native::CadenceStatus cadenceStatus() const;

    /// The viewer moved its bitrate (a `clientbitrate` message): the
    /// estimate following the frame the host really streams. The session's
    /// ceiling from the next frame — see mw::native::Session::
    /// setTargetBitrate. Ignored (with a log line) when the value is not a
    /// bitrate; safe from any thread.
    void setClientBitrate(int kbps);

    /// A human-readable description of what the session settled on, for the
    /// session log: "NVIDIA GeForce RTX 4070 · NVENC HEVC 4:4:4". Empty until
    /// the session has started.
    QString describeSession() const;

    /// Just the encoder's name — "NVENC", "AMF", "oneVPL", or the D3D12
    /// route's ("D3D12 VE", "NVENC (D3D12)"), with "(D3D11)" after the name
    /// when D3D12 was asked for and did not run. Empty until the session has
    /// started.
    ///
    /// This is what the client is told, not describeSession(): that one names
    /// the GPU, the codec and every flag, and a 60-character value pushed the
    /// stats overlay wider than a phone screen (04/09/2026). The codec already
    /// has its own row, the GPU is on the admin page, and what the overlay was
    /// missing is which silicon block does the encoding.
    QString describeEncoder() const;

    /// Whether the running stream really refreshes by intra-refresh.
    ///
    /// The relay reads this to decide whether it may ride out a gap instead of
    /// discarding deltas and demanding a keyframe. False until a session has
    /// started, and false whenever the encoder declined — so the answer is
    /// always what the stream DOES, never what was asked for.
    bool intraRefreshActive() const override;

    /// Frames per intra-refresh wave (SessionInfo::intraRefreshFrames), 0 when
    /// the stream does not intra-refresh or no session has started. The
    /// browser sizes its ride-out watchdog on it, in frames it receives.
    int intraRefreshFrames() const;

    /// What the session settled on, copied; false (and @p out untouched) until
    /// a session has started. The launch reply reads the frame and display
    /// geometry and the dynamic range off it. A guest on the shared feed gets
    /// the feed's, with its own session's audio beside it.
    bool sessionInfo(mw::native::SessionInfo& out) const;

    /// This engine carries the guests' shared feed instead of its own capture
    /// (StartParams::feedPipe).
    bool isFeedSubscriber() const { return m_Subscriber != nullptr; }

signals:
    /// The desktop portal issued a consent worth keeping — store it and hand
    /// it back as StartParams::portalRestoreToken on the next session, and the
    /// user is never asked again.
    ///
    /// Emitted at most once per session, from inside startCapture(), and only
    /// when the grant is new. Nothing depends on it: ignoring the token costs
    /// one dialog per session, it does not break the stream.
    void portalGrantReceived(const QString& token);

    /// The session turned Windows' Mouse Keys on (@p on, @p saved being what
    /// it found before: flags, maxSpeed, timeToMaxSpeed, ctrlSpeed), or no
    /// longer holds them. For the server, which puts them back after a worker
    /// that died holding them (mw::native::restoreMouseKeys). From the thread
    /// starting or stopping the session, or the capture thread: a receiver
    /// that must hear the last word connects directly.
    void mouseKeysChanged(bool on, const QJsonObject& saved);

private:
    void onEncodedFrame(const mw::native::EncodedFrame& frame);

    /// A guest's start: join the feed, wait for its `info`, then a session of
    /// input and audio only. Emits connectionStarted or connectionFailed.
    void startSubscriber(const StartParams& params);
    /// A picture from the feed, on the subscriber's thread, borrowed for the
    /// call: the same road as one this engine encoded itself.
    void onFeedFrame(const mw::native::feed::Header& header, const uint8_t* data, size_t size);
    /// A control message from the feed (a later `info`, `displayFormat`,
    /// `codec`, `bye`), on this engine's thread.
    void onFeedControl(const QJsonObject& message);
    /// The feed's pipe closed and it did not come back: this guest's session
    /// ends.
    void onFeedLost(const QString& why);
    /// The feed died and came back (relaunched under the same name): its
    /// pictures follow from its next keyframe, asked for at once.
    void onFeedRejoined(const QJsonObject& info);
    /// The feed worker's: say what the session is now, to every subscriber.
    void publishInfo();
    /// The feed's `info`, taken as this engine's own; false when unreadable.
    bool takeFeedInfo(const QJsonObject& info);

    /// The session's stage figures, once, when it ends. Idempotent.
    void logStageSummary();
    /// The bench's click trace (clicktrace=1), next to this process's log.
    void writeClickTrace();

    /// Turn a borrowed cursor image into a PNG and emit it. Runs on the capture
    /// thread — the pixels do not outlive the call.
    void onCursor(const mw::native::CursorUpdate& cursor);

    std::unique_ptr<mw::native::Session> m_Session;

    /// The session's CaptureHold verdict, kept past the session for the
    /// worker's last word to the server (captureHoldReport).
    QJsonObject m_CaptureHold;

    /// The HID passthrough's devices. Made at the first hidattach, gone with
    /// the session. The mutex guards the pointer only: HidPassthrough is
    /// thread-safe, and hidInput comes from the 'hid' channel's thread.
    std::mutex m_HidMutex;
    std::unique_ptr<mw::native::HidPassthrough> m_Hid;

    /// See setDirectFrameSink. The mutex is held for the whole sink call, so
    /// clearing the sink waits out a frame in flight; it is uncontended
    /// otherwise (one lock per frame on the capture thread, and the relay only
    /// touches it at setup and teardown).
    std::mutex m_SinkMutex;
    FrameSink m_DirectSink;

    /// The input dead-man switch (contract in InputWatchdog.h). Owned, on this
    /// engine's thread. Fed with the SHIFTED controller numbers, so what it
    /// neutralizes is the pad the host actually holds. Its sink speaks plain
    /// InputEvents — a key up, a button up, a centred pad — so the engine
    /// below needs no release message of its own: it already deduplicates,
    /// and a pad update with everything at zero IS a neutralization.
    InputWatchdog* m_Watchdog = nullptr;

    std::atomic<bool> m_Connected{false};

    /// Producer→consumer bookkeeping, mirroring MoonlightShim's so the relays'
    /// existing drop diagnostics keep working unchanged.
    std::atomic<int> m_PendingVideoFrames{0};
    /// The relay's probe for linkhold=, kept for a session that starts after
    /// it was set — see setLinkBusyProbe.
    std::mutex m_LinkProbeMutex;
    std::function<bool()> m_LinkBusyProbe;
    std::atomic<int64_t> m_WorkerDropCount{0};
    std::atomic<bool> m_WorkerDroppedDelta{false};

    /// Rotating window for takeHostProcessingLatencyMs. Measured here, unlike
    /// the GameStream path where it is reported by the host.
    std::atomic<int64_t> m_ProcWindowTotalUs{0};
    std::atomic<int64_t> m_ProcWindowCount{0};

    std::atomic<int64_t> m_FrameSubmitTimeUs{0};

    /// Presentation time of the latest frame, in µs SINCE THE FIRST FRAME — the
    /// relay's contract (IMediaEngine::videoFrameReady), not the engine's
    /// absolute stamp.
    std::atomic<int64_t> m_FramePresentationTimeUs{0};

    /// Absolute steady_clock present time of the first frame: the epoch of every
    /// relative presentation time above, and what firstFrameArrivalSteadyMs()
    /// reports. Zero until the first frame.
    std::atomic<int64_t> m_FirstPresentUs{0};

    // ── Per-stage latency ───────────────────────────────────────────────────
    //
    // Fed from two threads: the capture thread records t₀..t₃ as the frame
    // comes out of the encoder, the sender thread closes the timeline with
    // t₄/t₅ once the last fragment is on the wire. One mutex, uncontended in
    // practice (two short critical sections per frame), guards both the
    // histograms and the ring below.
    std::mutex m_StageMutex;
    mw::native::StageStats m_Stages;
    bool m_StageSummaryLogged = false;

    /// What the sender needs to know about a frame it is about to report on:
    /// its present and its encode stamp. Indexed by frame number modulo the
    /// size, and checked against the number so a frame the sender dropped
    /// cannot be scored against a later frame's stamps.
    struct InFlight
    {
        uint32_t frameNumber = 0;
        bool valid = false;
        int64_t presentUs = 0;
        int64_t encodedUs = 0;
    };
    static constexpr size_t kInFlightRing = 256;
    std::array<InFlight, kInFlightRing> m_InFlight{};

    /// VIDEO_FORMAT_* of what the encoder actually produces, so the browser
    /// configures the right decoder.
    std::atomic<int> m_NegotiatedVideoFormat{0};

    /// The client's last word on the still-screen frame floor, replayed onto a
    /// session that starts after it was said. See setFrameFloorFps.
    std::atomic<int> m_FrameFloorFps{0};

    /// The client's screen as last reported, replayed onto a session that
    /// starts after it was said. See setClientRefresh.
    std::atomic<int> m_ClientRefreshMilliHz{0};
    std::atomic<bool> m_ClientVsync{false};
    std::atomic<bool> m_ClientRefreshKnown{false};

    /// Sender evictions since the last link report — see noteEviction().
    std::atomic<int> m_Evictions{0};
    /// Frames shed off a backed-up send buffer since the last link report —
    /// see noteOverflow().
    std::atomic<int> m_Overflows{0};

    /// The session's EncoderTuning::nameLinkDrops key, for the relay's thread.
    std::atomic<mw::native::EncoderTuning::Choice> m_NameLinkDropsKey{
        mw::native::EncoderTuning::Choice::Default};

    /// False when the client's decoder takes no reference repairs (/start's
    /// ref_invalidation): every loss is healed by a keyframe, whatever the
    /// encoder can do (healsByReference). For the relay's thread.
    std::atomic<bool> m_ClientTakesRepairs{true};

    /// Samples per channel of the session's Opus frames (EncoderTuning's
    /// `audioframe=`), the relays' RTP step: 240 unless the bench says 480/960.
    std::atomic<int> m_AudioSamplesPerFrame{240};

    // ── The guests' shared feed ─────────────────────────────────────────────

    /// The feed worker's publisher (StartParams::feedPublisher), not owned.
    FeedPublisher* m_Publisher = nullptr;
    /// A guest's end of the feed's pipe; null for every other engine.
    std::unique_ptr<FeedSubscriber> m_Subscriber;
    /// The feed's session as its `info` said — what a guest's engine reports
    /// instead of its own — its overlay name and its log line. Under
    /// m_FeedMutex: read by the relay's thread, written on this engine's.
    mutable std::mutex m_FeedMutex;
    mw::native::SessionInfo m_FeedInfo;
    QString m_FeedEncoder;
    QString m_FeedDescription;
    /// Nothing of the feed goes to the relay before a keyframe: at the join,
    /// and again when a feed that died comes back — its deltas reference
    /// pictures this guest never had. Subscriber's thread and this engine's.
    std::atomic<bool> m_FeedNeedsKeyframe{true};
};
