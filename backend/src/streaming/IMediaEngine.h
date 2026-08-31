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

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>
#include <cstdint>

/**
 * @brief What produces a stream's video, audio and input, whatever the source.
 *
 * The relays (DataChannelRelay, MediaTrackRelay, StreamRelay) and the WSS
 * fallback in SignalingServer speak only to this. They do not know, and must
 * not learn, whether the frames come from a remote GameStream host or from this
 * machine's own screen.
 *
 * Two implementations:
 *  - MoonlightShim — moonlight-common-c against Sunshine / Apollo / Wolf. This
 *    is the path every existing host uses and its behaviour must not change.
 *  - NativeMediaEngine — MoonlightWeb's own capture and encoding, for streaming
 *    the machine it runs on (backend/native-host/).
 *
 * Which one a session gets is decided in exactly one place: the media-engine
 * branch in StreamSession::onLaunchResult(), keyed on MediaDescriptor::type.
 *
 * ── On the split between pure and defaulted ────────────────────────────────
 *
 * Pure virtual = every engine must genuinely do this.
 *
 * Virtual with a default = the concept only exists for GameStream, and a native
 * engine answering "nothing to report" is the honest answer rather than a stub
 * it was forced to write. `hostRttMs()` is the clearest case: there is no host
 * round trip when the host is this process, and reporting 0 is true, whereas
 * inventing a number would put a fiction in the stats overlay.
 *
 * ── Threading ──────────────────────────────────────────────────────────────
 *
 * Unchanged from MoonlightShim, because the relays' assumptions are unchanged:
 * the input methods are called from relay/libdatachannel threads and must be
 * safe there; the metric accessors are read from the relay thread while the
 * producer writes them elsewhere, so implementations use atomics.
 */
class IMediaEngine : public QObject
{
    Q_OBJECT

public:
    explicit IMediaEngine(QObject* parent = nullptr)
        : QObject(parent)
    {}
    ~IMediaEngine() override = default;

    /// A press currently applied to the host, kept so the watchdog can release
    /// it and a heartbeat can restore it.
    ///
    /// Lives here rather than in an implementation because the input watchdog
    /// is session logic, not GameStream logic: a key held through a link stall
    /// sticks down just the same when the host is this machine.
    struct HeldKey
    {
        short keyCode = 0;
        char modifiers = 0;
        char flags = 0;
        bool hold = false;
    };

    // ── Lifecycle ───────────────────────────────────────────────────────────
    //
    // Starting is deliberately absent: it takes engine-specific parameters (an
    // RTSP session URL and an AES key for GameStream, a display id for native)
    // and belongs to the branch that already knows which engine it built.

    virtual void stopConnection() = 0;

    /// Tear the connection down without waiting for it to unwind cleanly. Used
    /// on the teardown paths where the caller is about to exit anyway.
    virtual void interruptConnection() = 0;

    virtual bool isConnected() const = 0;

    // ── Video ───────────────────────────────────────────────────────────────

    /// Ask for a keyframe. Rate-limited by the caller, never here.
    virtual void requestIdrFrame() = 0;

    /// Balance the producer→consumer pending-frame counter. Called by the relay
    /// at the head of its frame handler, once per frame delivered.
    virtual void videoFrameDelivered() = 0;

    /// Consume the producer-side delta-drop flag (true once per drop episode).
    /// The relay uses it to arm awaiting-IDR recovery.
    virtual bool takeWorkerDroppedDelta() = 0;

    virtual int64_t workerDropCount() const = 0;
    virtual int pendingVideoFrames() const = 0;

    /// The video format actually in use, as a VIDEO_FORMAT_* mask
    /// (0x0001 H.264, 0x0100 HEVC, 0x0200 AV1); 0 before it is known.
    virtual int negotiatedVideoFormat() const = 0;

    // ── Audio ───────────────────────────────────────────────────────────────

    /// Opus samples per channel per packet at 48 kHz (240 = 5 ms, 480 = 10 ms).
    /// The relay advances the RTP timestamp by exactly this, which is what
    /// keeps the browser's NetEq from time-stretching into robotic audio.
    virtual int audioSamplesPerFrame() const = 0;

    // ── Input ───────────────────────────────────────────────────────────────
    //
    // `hold` marks a press the client wants kept down through a brief link
    // stall (movement keys in gaming mode) — see the watchdog notes below.

    virtual void sendKeyEvent(short keyCode, bool down, char modifiers, char flags,
                              bool hold = false) = 0;
    virtual void sendUtf8Text(const QString& text) = 0;
    virtual void sendMouseMove(short deltaX, short deltaY) = 0;
    virtual void sendMousePosition(short x, short y, short referenceWidth,
                                   short referenceHeight) = 0;
    virtual void sendMouseButton(bool down, int button, bool hold = false) = 0;
    virtual void sendMouseScroll(short scrollAmount) = 0;
    virtual void sendMouseHScroll(short scrollAmount) = 0;

    virtual void sendControllerArrival(uint8_t controllerNumber, uint16_t activeGamepadMask,
                                       uint8_t type, bool hasRumble) = 0;
    virtual void sendControllerState(short controllerNumber, short activeGamepadMask,
                                     int buttonFlags, unsigned char leftTrigger,
                                     unsigned char rightTrigger, short leftStickX, short leftStickY,
                                     short rightStickX, short rightStickY) = 0;

    /// Shift this session's controller numbering so concurrent sessions do not
    /// collapse onto the host's controller 0. Zero keeps the browser's own.
    virtual void setControllerOffset(int offset) = 0;

    /// Align the host's toggle locks with the client's.
    virtual void syncLockKeys(bool numLock, bool capsLock, bool scrollLock) = 0;

    /// Reconcile the host with the client's authoritative held-input state (its
    /// heartbeat): press what the watchdog released but the user still holds,
    /// release what drifted. `buttonMask` is 1 << (button - 1).
    virtual void syncHeldInputs(const QVector<HeldKey>& keys, quint32 buttonMask,
                                bool buttonsHold) = 0;

    /// Release every held key/button (and neutralize every non-idle gamepad).
    /// `includeHold` also releases the inputs flagged `hold`.
    virtual void releaseHeldInputs(bool includeHold) = 0;

    /// Refresh the client-liveness timestamp. Called from every relay on every
    /// inbound client message, whatever its type.
    virtual void noteClientAlive() = 0;

    // ── Metrics ─────────────────────────────────────────────────────────────

    /// Host processing latency (capture→encode, ms) averaged over the frames
    /// since the previous call. Read-and-reset: one consumer only.
    ///
    /// Measured rather than reported when the host is this machine, which is
    /// the one metric the native engine can state with certainty.
    virtual double takeHostProcessingLatencyMs() = 0;

    virtual int64_t frameSubmitTimeUs() const = 0;
    virtual int64_t framePresentationTimeUs() const = 0;
    virtual int64_t firstFrameArrivalSteadyMs() const = 0;

    // ── GameStream-only, with truthful defaults ─────────────────────────────

    /// One-way latency to the host, in ms. Zero when the host is this process:
    /// there is no round trip to measure, and a fabricated number would be
    /// worse than an absent one.
    virtual double hostRttMs() const { return 0.0; }

    /// IP TTL of the first datagram received from the host — the one thing on
    /// the wire that hints at the host's OS family (see HostOsProbe.h). Zero
    /// when nothing has been read, and permanently zero when there is no wire.
    virtual int hostIpTtl() const { return 0; }

    /// Quantize scroll to whole 120-unit notches, for a host that discards
    /// sub-notch amounts. Meaningless when we inject the scroll ourselves.
    virtual void setScrollQuantization(bool enabled) { Q_UNUSED(enabled); }

    /// Snapshot the host's real lock-key state. Only possible when the streamed
    /// host IS this machine — which is always true for a native engine and
    /// conditional for GameStream, hence the parameter.
    virtual void captureHostLockState(bool hostIsSelf) { Q_UNUSED(hostIsSelf); }

    /// Whether this session may nudge the host pointer to unstick a still
    /// screen. Off for a guest who was not given keyboard/mouse.
    virtual void setWakeNudgeAllowed(bool allowed) { Q_UNUSED(allowed); }

    /// Whether the stream repairs itself by intra-refresh rather than by
    /// keyframes.
    ///
    /// False for GameStream, and truthfully so: what a remote host's encoder
    /// does is not ours to know, so the relay keeps its keyframe recovery.
    /// Only the native engine can answer this, and only once its encoder has
    /// accepted the request — which is why it is a live question rather than a
    /// flag set at construction.
    virtual bool intraRefreshActive() const { return false; }

signals:
    /// presentationTimeUs travels WITH the frame through the queued connection:
    /// relays must not re-read a "latest frame" atomic at drain time, or a
    /// drained burst gets stamped with one shared timestamp and defeats the
    /// frontend's out-of-order frame filter on reordering links.
    void videoFrameReady(QByteArray data, int frameType, int frameNumber,
                         qint64 presentationTimeUs);
    void audioSampleReady(QByteArray data);

    void stageChanged(int stage);
    void connectionStarted();
    void connectionFailed(const QString& error);
    void connectionTerminated(int errorCode);
    void connectionStopped();

    /// The host asked to rumble a controller (forwarded to the browser).
    void rumble(int controllerNumber, int lowFreqMotor, int highFreqMotor);

    /// The mouse pointer's SHAPE changed, and the browser is the one drawing it.
    ///
    /// Native host only, and only in desktop mode — see setCompositeCursor.
    /// Carries no position: the browser already knows where its own pointer is,
    /// sooner and more accurately than this could say.
    ///
    /// `visible` and `png` are separate on purpose. Desktop Duplication only
    /// hands over a shape when it CHANGES, so a session can legitimately know
    /// that a pointer is on the display without ever having been shown what it
    /// looks like. `visible` with an empty `png` means exactly that — draw
    /// something ordinary — while `visible == false` means draw nothing at all.
    ///
    /// `kind` names the pointer as a CSS cursor keyword when it is one of the
    /// system's standard shapes, and is empty for an application's own artwork.
    /// It lets a client show ITS native pointer, changing with the content,
    /// instead of the host's foreign-looking bitmap — see CursorUpdate::kind.
    void cursorShapeChanged(QByteArray png, int hotspotX, int hotspotY, bool visible, QString kind);
};
