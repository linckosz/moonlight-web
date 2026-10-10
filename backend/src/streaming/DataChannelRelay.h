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

#include "RelayBase.h"
#include "HostLagTracker.h"
#include "LinkFreezeLog.h"
#include "SendBacklog.h"
#include "FrameSender.h"
#include "LinkLoss.h"
#include "RelayFrameLog.h"
#include "SctpCounters.h"
#include "AroadPacer.h"
#include "mw/native/EncoderTuning.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMutex>
#include <QTimer>
#include <array>
#include <deque>
#include <vector>
#include <memory>
#include <atomic>
#include <string>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <chrono>

namespace rtc {
class DataChannel;
class Track;
} // namespace rtc

class IMediaEngine;
class SctpFlood;

// WebRTC DataChannel relay that replaces StreamRelay.
// Forwards video/audio from the media engine + input from the browser over a
// hybrid PeerConnection:
//   - video: DataChannel (SCTP, negotiated id=0, ordered, each message given
//     up on after kVideoFrameLifetimeMs)
//   - audio: rtc::Track (Opus over RTP) — not a DataChannel, despite the name
//   - input: DataChannel (SCTP, negotiated id=2, reliable + ordered)
//
// Thread safety:
// - The relay lives on a per-session relay thread (Session::spawnRelayThread),
//   together with the media engine and the signaling server.
// - GameStream engines: media engine signals are queued onto the relay thread
//   (AutoConnection), byte for byte the historical path.
// - Native engine: no signal at all. The relay installs itself as the engine's
//   direct frame sink (NativeMediaEngine::setDirectFrameSink), so
//   handleVideoFrame runs on the capture thread that just finished encoding,
//   reading the encoder's own buffer, and the wire chunks are built right
//   there — no event-loop hop and no intermediate copy before the frame
//   reaches the sender. Everything that path touches (buffered keyframe, IDR
//   gate, drop counters, the video DC pointer) is guarded by m_VideoMutex, the
//   arrangement MediaTrackRelay already runs with.
// - libdatachannel callbacks (onMessage for input) fire from internal threads.
//   GameStream engines: input is marshaled back to the relay thread via
//   QMetaObject::invokeMethod, unchanged. Native engine: the message is parsed
//   and injected right there, on the thread that received it — the engine's
//   input path is thread-safe end to end (InputWatchdog is mutex-guarded, the
//   clipboard bridge hops to the main thread itself, WindowsSession::sendInput
//   injects on the calling thread under its own lock), so the only thing the
//   hop bought was a queue behind whatever the relay thread was doing. The
//   relay-side state that path reads (the input policy, the DC pointer) is
//   guarded by m_InputMutex, which stop() also takes once to let a message
//   in flight finish before the engine goes away.
class DataChannelRelay : public RelayBase
{
    Q_OBJECT

public:
    explicit DataChannelRelay(IMediaEngine* engine, QObject* parent = nullptr);
    ~DataChannelRelay() override;

    // PeerConnection access (not part of RelayBase interface)
    std::shared_ptr<rtc::PeerConnection> peerConnection() const { return m_Pc; }

    // ── RelayBase interface ─────────────────────────────────────────────────

    bool prepare(const rtc::Configuration& config, bool isInternet = false) override;

    bool setRemoteDescription(const std::string& sdp) override;

    bool addRemoteCandidate(const std::string& candidate, const std::string& mid) override;

    void stop() override;

    /// Hides RelayBase::setInputPolicy: in direct-input mode the policy is read
    /// on a libdatachannel thread while the session pushes updates onto the
    /// relay thread, so the write goes under m_InputMutex. Same signature and
    /// meaning; callers hold a DataChannelRelay*, so they land here.
    // cppcheck-suppress duplInheritedMember ; intentional non-virtual hide, see comment above
    void setInputPolicy(const InputMsg::Policy& policy)
    {
        std::lock_guard<std::mutex> lk(m_InputMutex);
        RelayBase::setInputPolicy(policy);
    }

    void notifyClientTakenOver() override;

    void notifyClientRevoked() override;
    void notifyClientSessionEnded() override;

    /// Retrieve and clear the buffered keyframe (if any).
    /// Used by SignalingServer before stop() to preserve the keyframe for
    /// WebSocket fallback — without this, the fallback starts with delta
    /// frames and the browser's VideoDecoder can never configure.
    QByteArray takeBufferedKeyframe()
    {
        std::lock_guard<std::mutex> lk(m_VideoMutex);
        QByteArray kf = m_BufferedKeyframe;
        m_BufferedKeyframe.clear();
        m_BufferedKeyframePresUs = -1;
        m_HaveBufferedKeyframe = false;
        m_NewKeyframeArrived = false;
        return kf;
    }

    void requestIdrFrame() override;

    bool isConnected() const override { return m_Connected; }

    IMediaEngine* mediaEngine() const override { return m_Shim; }

    /// Enable bidirectional text clipboard sync. Only called with true when
    /// the streamed host is this machine (the backend clipboard IS the host
    /// clipboard) — see ClipboardBridge. Must be called before the relay is
    /// moved to its dedicated thread (Session does, right after creation).
    void setClipboardEnabled(bool enabled);

    /// Stop answering every gap with a keyframe request, and let the picture
    /// repair itself instead.
    ///
    /// Only ever true when BOTH hold: the encoder really is intra-refreshing
    /// (so there is a repair to wait for), and the client said it will keep
    /// decoding through the damage (so the repair is allowed to happen). Either
    /// missing and this stays false, which is exactly today's behaviour.
    ///
    /// Set once before the relay moves to its own thread, like the flags above.
    void setRideOutLoss(bool enabled) { m_RideOutLoss = enabled; }

    /// The stream's bitrate, which sizes the transport's send buffer and the
    /// backlog gate's noise floor (SendBacklog.h): both are amounts of TIME,
    /// and the same bytes are 100 ms at 20 Mbit/s and a second at 2 Mbit/s.
    /// Unknown (0) keeps the fixed sizes that served the LAN. Set once before
    /// the relay moves to its own thread, like the flags above.
    void setStreamBitrateKbps(int kbps) { m_StreamBitrateKbps = kbps; }

    /// The bench's link keys of a native session (plan Idées Punktfunk, A0):
    /// its losses (`loss=`, `burst=`), usrsctp's congestion module (`sctpcc=`)
    /// and a flood on channel id 3 (`flood=`, SctpFlood.h). All off unless a
    /// bench set them in MW_NATIVE_TUNING or native_tuning. Set once before
    /// prepare(), like the flags above.
    void setLinkBench(const mw::native::EncoderTuning& tuning);

    /// Which codecs ride an RTP video track instead of the video DataChannel
    /// (POC Ultra U1.4): `native:h264+hevc+av1+ultra;other:h264+hevc+av1`, the
    /// section matching @p nativeHost applies. Matched against the engine's
    /// negotiated codec when the offer is built; empty = the DataChannel, as
    /// before. The browser may still refuse the track (no Encoded Transform):
    /// the video then stays on the DataChannel. Set once before prepare().
    void setRtpVideoPolicy(const QString& spec, bool nativeHost);

private:
    /// Both halves of the bargain, asked at the moment a gap happens.
    ///
    /// The engine's half has to be a live question: the relay is built before
    /// the engine starts, so anything cached at construction would answer "no
    /// intra-refresh" for the whole session. Defined in the .cpp because the
    /// engine is only forward-declared here.
    bool ridingOutLoss() const;

public:
    // Signals inherited from RelayBase: signalingSdpReady, signalingIceCandidate,
    // dataChannelsOpen, sessionEnded.

private slots:
    void onVideoFrame(const QByteArray& data, int frameType, int frameNumber,
                      qint64 presentationTimeUs);
    void onAudioSample(const QByteArray& data);
    void onShimConnectionTerminated(int errorCode);

private:
    // The video path proper, shared by the queued slot above and the native
    // engine's direct sink. `data` may be a BORROWED buffer
    // (QByteArray::fromRawData over the encoder's output): valid until this
    // returns, never to be kept — the one place that keeps a frame (the
    // buffered keyframe) copies explicitly.
    void handleVideoFrame(const QByteArray& data, bool isKeyframe, int frameNumber,
                          qint64 presentationTimeUs);

    // Best-effort exit notice ({"type": ...}) on the input DC before stop().
    void sendExitNotice(const char* type);

    void setupPeerConnection(const rtc::Configuration& config);
    void createDataChannels();
    // @p recvUs: the host's steady clock (µs) when the message came off the
    // channel; 0 for "now". Echoed to the client for a stamped message.
    void onInputMessage(const std::string& message, int64_t recvUs = 0);
    // The reply to an input message that carried a `stamp` (T7 of the radios
    // plan): when it arrived and when its handling ended, on the host's
    // steady clock, so the client can split a click into its way up and the rest.
    void sendInputStamp(double id, int64_t recvUs);
    void handleKeyEvent(const std::string& type, const std::string& body);
    void handleMouseMove(const std::string& body);
    void handleMouseButton(const std::string& body);
    void handleMouseScroll(const std::string& body);

    // Fragmentation helpers — sends data in chunks over a DataChannel.
    // Header:
    // [frame_id:4][chunk_index:2][total_chunks:2][is_keyframe:1][payload_size:4][backend_ts:4]
    // backend_ts: monotonic millisecond timestamp (mod 2^32) taken at send time,
    // used by the frontend to compute end-to-end latency.
    // Max payload per chunk: kMaxPayloadSize (stays under SCTP 16KB fragment limit).
    static constexpr int kFragHeaderSize = 17;
    static constexpr int kMaxPayloadSize = 16000;

    /// How long a video message may wait in the transport before SCTP gives
    /// up on it — a lifetime, not a retransmit count. See the video channel's
    /// setup for the freeze that made the difference. Matches the receiver's
    /// own patience with an incomplete frame (FRAME_TIMEOUT_MS).
    static constexpr int kVideoFrameLifetimeMs = 500;
    /// The receiver's link reports come every kLinkstatsPeriodMs; a gap of
    /// kLinkstatsSilentMs between two of them is the link frozen upstream,
    /// held key or not.
    static constexpr qint64 kLinkstatsPeriodMs = 500;
    static constexpr qint64 kLinkstatsSilentMs = 2 * kLinkstatsPeriodMs;

    // Backpressure is measured in TIME, not bytes — see SendBacklog.h for why
    // a byte threshold could only ever be too late or too trigger-happy, and
    // for the correction to the old claim that dc->send() blocks the event
    // loop (it does not; libdatachannel's SCTP socket is non-blocking).
    SendBacklog m_Backlog;
    // Both directions of every link freeze — the backlog above, and the
    // client's silence, read on its held inputs and on the cadence of its link
    // reports — counted for the stats card (see LinkFreezeLog.h).
    LinkFreezeLog m_Freezes;
    /// When the last `linkstats` arrived, 0 before the first. Input thread only.
    qint64 m_LastLinkstatsMs = 0;
    /// The host's capture-to-send lag, taken out of the receiver's link report
    /// (see HostLagTracker.h). Under m_VideoMutex.
    HostLagTracker m_HostLag;
    /// The receiver's last report with the host's share taken out, for the
    /// stats card's LINK QUEUE; -1 before the first report.
    std::atomic<int> m_LinkQueueMs{-1};
    // Deltas a GameStream engine may leave waiting on the sender thread before
    // the oldest is evicted (the native engine keeps one). See the constructor.
    static constexpr size_t kGameStreamQueuedDeltas = 2;

    // presentationTimeUs: the frame's own capture time (from the decode unit),
    // carried through the queued signal. -1 = unknown → fall back to the shim's
    // latest value. Never read the shim atomic for regular frames: a drained
    // burst would share one backendTs and defeat the frontend's ordering filter.
    //
    // frameNumber: the engine's own number for the frame, so the sender can
    // report when it left (IMediaEngine::frameSentSink). -1 = not a live frame
    // (a buffered keyframe replayed at DC open) — nothing is reported.
    //
    // In direct mode the chunks are built here, on the calling thread, from
    // `data` (which may be borrowed — see handleVideoFrame), and the sender
    // only sends. Otherwise the frame is queued whole and the sender cuts it.
    void sendFragmented(const QByteArray& data, bool isKeyframe,
                        std::shared_ptr<rtc::DataChannel>& dc, qint64 presentationTimeUs = -1,
                        int frameNumber = -1);

    // Send a previously buffered keyframe (arrived before Video DC was open).
    // Called from the Video DC onOpen callback (marshaled to the relay thread).
    void sendBufferedKeyframe();

    // Coalescing IDR throttle: all IDR requests (frontend + internal) go through
    // this method. Requests arriving within the adaptive cooldown of the last
    // effective request are absorbed to prevent LiRequestIdrFrame flooding.
    // Caller holds m_VideoMutex. A no-op while m_IdrWaitsForDrain: the gate
    // would drop the keyframe, and the drain asks (requestIdrOnDrain).
    void sendIdrRequestThrottled();
    // The buffer moved again after the gate held keyframes back: ask now, on
    // a fresh cooldown, instead of when the backed-off timer would have.
    // Caller holds m_VideoMutex.
    void requestIdrOnDrain();
    /// The bench's pacing (`pace=`), told to the sender once the stream's
    /// bitrate is known: at prepare().
    void applyPacing();

    IMediaEngine* m_Shim;

    // True when this relay is the native engine's direct frame sink:
    // handleVideoFrame then runs on the engine's capture thread over a borrowed
    // buffer, and sendFragmented builds the chunks itself. False for every
    // GameStream engine, whose frames are its own QByteArray and are cut by the
    // sender thread — but since September 2026 they too reach handleVideoFrame
    // on the thread that produced them (a direct signal connection), not
    // through the relay thread's event loop: m_VideoMutex is what serializes
    // the frame path in both cases, and a queued hop bought nothing but a
    // wake-up behind the input parser (docs/optimisations-existant.md).
    bool m_DirectVideoSend = false;

    // Input messages are handled on the libdatachannel thread that received
    // them, for every engine: IMediaEngine's input calls are thread-safe by
    // contract (LiSend* for GameStream, the injector for the native host) and
    // the relay thread was only a queue in front of them. Kept as a flag so
    // the two lock sites that exist for it read as what they are.
    bool m_DirectInput = true;
    /// The receiver's first `linkstats` has arrived (native host): logged once.
    bool m_LinkReportsSeen = false;

    // Direct-input mode: serializes onInputMessage against setInputPolicy and
    // stop(). One message at a time is already libdatachannel's contract for a
    // single channel, so it is uncontended in steady state; stop() takes it
    // once, after closing the input DC, so a message halfway through injection
    // finishes before the engine is torn down. Never held while waiting on
    // m_VideoMutex from stop(): the requestidr path takes m_VideoMutex INSIDE
    // this one, so stop() must not do the reverse.
    std::mutex m_InputMutex;

    // Serializes the video path — onVideoFrame (capture thread in direct mode),
    // sendBufferedKeyframe / takeBufferedKeyframe / requestidr (relay thread),
    // stop() and the stats tick. Held for one frame's bookkeeping at most; the
    // sender thread never takes it, so a full SCTP buffer cannot stall a
    // holder. Everything from here to the ICE timer that is not atomic is
    // guarded by it.
    std::mutex m_VideoMutex;

    // Dedicated thread for DataChannel fragmentation + send (keeps the per-frame
    // memcpy + dc->send off the Qt main thread / HTTP event loop).
    std::unique_ptr<FrameSender> m_Sender;

    std::shared_ptr<rtc::PeerConnection> m_Pc;
    std::shared_ptr<rtc::DataChannel> m_VideoDc;
    // Audio is a native RTP Opus track (browser-decoded: jitter buffer + in-band
    // FEC + PLC) on the same PeerConnection as the video DataChannel — a lost
    // packet no longer head-of-line-blocks the audio (the periodic dropouts).
    std::shared_ptr<rtc::Track> m_AudioTrack;
    // POC Ultra U1.4: the video on an RTP track (setRtpVideoPolicy). The
    // browser takes each frame off it by Encoded Transform, before its own
    // decoder. The RTP timestamp carries the frame's backendTs in ms as is (the
    // client reads it raw; nothing on that side runs the 90 kHz clock).
    QString m_RtpVideoSpec;
    bool m_RtpVideoNativeHost = false;
    std::shared_ptr<rtc::Track> m_VideoTrack;
    bool m_RtpVideoAudioRoad = false; // the bench's `aroad`: frames cut in Opus packets
    bool m_UltraAudioRoad = false;    // the same for the Ultra track
    uint16_t m_UltraAudioRoadSeq = 0;
    uint16_t m_RtpAudioRoadSeq = 0;
    bool m_RtpVideoSentKeyframe = false;         // deltas wait for the first keyframe on the track
    std::atomic<bool> m_RtpVideoAccepted{false}; // set from the answer
    std::atomic<bool> m_UltraRtpAccepted{false};
    std::atomic<int> m_RtpPliCount{0};
    std::vector<int> m_RtpSendUs, m_RtpLateUs; // under m_VideoMutex
    // The Ultra synthetic stream on its own RTP track (VP8 envelope, every
    // frame a keyframe so none depends on another), when "ultra" is named.
    std::shared_ptr<rtc::Track> m_UltraTrack;
    /// True once the answer accepted the video track: frames go there.
    bool rtpVideoActive() const;
    void createRtpVideoTracks();
    // `frameNumber`: the engine's, for a reference invalidation when the page
    // names the frame lost (-1 when unknown, as for the buffered keyframe).
    void sendRtpVideo(const QByteArray& frameData, bool isKeyframe, int64_t presentationTimeUs,
                      int frameNumber = -1);
    // What the audio road sent lately, per track, for the page's NACKs (U1.4
    // quater): the chunks of the last frames as they went, with their RTP
    // timestamp.
    struct AudioRoadHistory
    {
        struct Frame
        {
            uint16_t seq = 0;
            uint32_t timestamp = 0;
            std::vector<std::vector<std::byte>> chunks;
        };
        std::mutex mutex;
        std::deque<Frame> frames;
        size_t maxFrames = 60;
        int resent = 0;
    };
    AudioRoadHistory m_VideoRoadHistory, m_UltraRoadHistory;
    // `frameId`: the video's wire frame id (the DataChannel's sequence), so
    // the page can name a lost frame to a host that heals by invalidation.
    void sendAudioRoad(const std::shared_ptr<rtc::Track>& track, AudioRoadHistory& history,
                       uint16_t seq, const uint8_t* data, size_t size, bool isKeyframe,
                       uint32_t timestamp, uint32_t frameId = 0);
    void resendAudioRoad(const QJsonObject& msg);
    // The road's paced sender (AroadPacer.h), made with the road's tracks;
    // null with aroadpace=0. Its first sends and resends, counted for the
    // rate governor next to SCTP's retransmissions (linkstats).
    std::unique_ptr<AroadPacer> m_AroadPacer;
    double m_AroadPace = -1;
    int m_AroadWindowKb = 0;
    // Bytes of the frame per packet (aroadchunk=, POC Ultra P-B).
    size_t m_AroadChunk = 1100;
    int64_t m_AroadAcks = 0;
    std::atomic<int64_t> m_AroadSent{0}, m_AroadResent{0};
    // The resends' budget (AroadPacer.h): a fifth of what the road sent,
    // another share with the bench key aroadbudget=.
    std::mutex m_AroadBudgetMutex;
    AroadResendBudget m_AroadBudget{0.2, 16 * 1024};
    int64_t m_LinkAroadSent = 0, m_LinkAroadResent = 0;
    std::shared_ptr<rtc::DataChannel> m_InputDc;
    // The HID passthrough's reports (id 4, unordered, never retransmitted): a
    // lost one is repaired by the next, the page repeats an unchanged report.
    std::shared_ptr<rtc::DataChannel> m_HidDc;

    // The bench's link keys (setLinkBench). m_Loss is the video path's, under
    // m_VideoMutex; the rest is read once, at setup.
    LinkLoss m_Loss;
    int m_SctpCongestion = -1;
    int m_FloodKbps = 0;
    int m_FloodBytes = 0;
    bool m_FloodLikeVideo = false;
    std::shared_ptr<rtc::DataChannel> m_FloodDc;
    std::unique_ptr<SctpFlood> m_Flood;
    // POC Ultra U1.1 (`ultra=synthetic:<KiB>`, `ultrachannel=`): a synthetic
    // Ultra stream on channel id 5, one message train per video frame sent,
    // through a FrameSender of its own so that it never evicts the video.
    // Read at setup; the payload is made once, incompressible.
    int m_UltraSynthKb = 0;
    bool m_UltraUnordered = false;
    std::shared_ptr<rtc::DataChannel> m_UltraDc;
    std::unique_ptr<FrameSender> m_UltraSender;
    std::vector<uint8_t> m_UltraPayload;
    uint32_t m_UltraSeq = 0;
    std::atomic<bool> m_UltraOpen{false};
    /// One synthetic Ultra frame, stamped @p backendTs like the video frame it
    /// follows. On the capture thread, never blocking (the sender's thread sends).
    void sendUltraSynthetic(uint32_t backendTs);
    // `relaylog=1`: each video frame's way through the relay (RelayFrameLog.h),
    // written next to the log at the end of the session. Made at setup, before
    // any frame; null otherwise, and every hook below is one null check.
    std::unique_ptr<RelayFrameLog> m_FrameLog;
    // The video channel, for that log's sender-side probe of bufferedAmount:
    // set once when the channel is made, and expiring with it.
    std::weak_ptr<rtc::DataChannel> m_FrameLogDc;
    // SCTP's smoothed round trip, sampled each second for that log.
    std::atomic<int> m_SrttMs{-1};
    // `audiolog=1` (plan « le son et la priorité des paquets », A0): each audio
    // packet's way through the host (AudioPathLog.h, process-wide), written
    // next to the log at the end of the session. Set before any packet.
    bool m_AudioLog = false;
    /// clicktrace=1: a log line per stamped input (see sendInputStamp).
    bool m_ClickTraceLog = false;
    // `pace=`, `paceburst=` (plan Wi-Fi W2 A): the sender hands a frame's
    // chunks to SCTP at this many times the stream's bitrate, this many KB at
    // a time; chunks no bigger than that run. Set at setup, before any frame.
    int m_PaceMultiple = 0;
    int m_PaceBurstKb = 0;
    size_t m_ChunkPayload = 16000;
    // `sctpbuf=`, `linkhold=` (plan Wi-Fi W2 C): usrsctp's send buffer in KB,
    // really (0: libdatachannel's 256 KiB), and after how long video waiting
    // outside it has the native session hold its pictures (0: never). Set at
    // setup.
    int m_SctpBufferKb = 0;
    int m_LinkHoldMs = 0;
    // `sctpburst=` (plan Wi-Fi W2.5): usrsctp's max burst, in packets; 0 no
    // limit, -1 libdatachannel's 10. Set at setup, by setLinkBench, which the
    // native host's sessions always call.
    int m_SctpMaxBurst = -1;
    // The native host's own max burst when `sctpburst=` is not said: no limit
    // (plan Wi-Fi W2.5). Windows, 04/10/2026: the click of a Mac in Wi-Fi
    // 66.9 -> 58.5 ms, p90 98 -> 69. Linux, the same night: a frame ~6 ms
    // younger on a clean link. macOS, 05/10: to an N95 in Wi-Fi, a frame's
    // time in usrsctp ~22 -> ~11 ms. libdatachannel's own is 10.
    static constexpr int kNativeSctpMaxBurst = 0;
    // `sctpss=` (plan Wi-Fi W2.3): usrsctp's stream scheduler, -1 its own.
    int m_SctpScheduler = -1;
    // usrsctp's counters at the last link report, for the share of chunks
    // sent again in each report window (LinkFeedback::retransPermille).
    // Relay thread only.
    mw::sctp::Counters m_LinkSctp{};
    bool m_LinkSctpSet = false;

    // Audio RTP timestamp (48 kHz Opus clock), advanced by samplesPerFrame per
    // packet for a smooth, jitter-free clock; serialized with track teardown.
    std::mutex m_AudioMutex;
    uint32_t m_AudioRtpTs = 0;

    std::atomic<bool> m_Connected{false};
    std::atomic<bool> m_Stopping{false};
    // usrsctp's counters when the channels opened, so the destructor can
    // report the session's own. Written once in onOpen, then the flag is raised.
    mw::sctp::Counters m_SctpAtOpen{};
    std::atomic<bool> m_SctpAtOpenSet{false};
    // Bidirectional clipboard sync (only when the streamed host is this
    // machine). Written once on the main thread before the relay moves to its
    // dedicated thread, read from relay/libdatachannel threads afterwards.
    bool m_ClipboardEnabled = false;
    /// See setRideOutLoss. Written once before the thread move, read on the
    /// relay thread afterwards.
    bool m_RideOutLoss = false;
    /// See setStreamBitrateKbps. Written once before the thread move.
    int m_StreamBitrateKbps = 0;
    int m_FrameCount = 0;
    uint32_t m_FrameId = 0; // Monotonic counter for VIDEO fragmentation headers
    /// Wire frameId → the engine's own frame number, for the last few hundred
    /// frames: the browser names a lost frame by the wire id, the native
    /// engine's reference invalidation wants its own number, and the two drift
    /// apart at every frame the relay drops before assigning an id. Written on
    /// the sending thread, read on the input thread — hence atomics; a slot
    /// holds -1 until a frame has used it. Indexed by frameId modulo the size.
    static constexpr uint32_t kFrameNumberRing = 512;
    std::array<std::atomic<int64_t>, kFrameNumberRing> m_FrameNumberById;
    uint32_t m_AudioFrameId = 0; // Separate counter for audio — audio must not
                                 // consume video frameIds (frontend gap detection
                                 // relies on contiguous video ids)

    // Backpressure counters (diagnostic logging)
    int m_DeltaDroppedCount = 0; // Delta frames dropped due to full SCTP buffer
    // Of those, the ones named to the encoder (namedrops, plan §9-25).
    int m_NamedDeltaDropCount = 0;
    // Deltas dropped by the awaiting-IDR gate. This is the BULK of a stall:
    // one delta hits a full buffer, the gate closes, and every frame after it
    // is discarded here — at 60 fps, ~60 per second — while m_DeltaDroppedCount
    // barely moves (it only counts the deltas that reached the buffer check).
    // Uncounted, a 19s outage read as "21 dropped deltas" in the logs.
    int m_AwaitingIdrDropCount = 0;
    int m_KeyframeBackpressureWarnings = 0; // Keyframes sent while buffer was above watermark
    int m_BackpressureDropCount = 0;        // Frames dropped in current backpressure episode
    qint64 m_LastDropSnapshot = 0;          // Sum of all drop counters at last stats tick
    // Decode latency tracking (microseconds)
    std::atomic<int64_t> m_LastDecodeLatencyUs{0};

    // IDR coalescing: adaptive cooldown between effective LiRequestIdrFrame calls.
    // All IDR sources (frontend requestidr, backpressure) converge here.
    // Exponential backoff: while requests keep firing without a keyframe getting
    // through, the cooldown doubles (300 ms → 5 s). Each IDR is a large frame
    // that inflates the encoded bitrate exactly when the link is saturated, so
    // an IDR flood feeds the very congestion it tries to fix.
    static constexpr qint64 kIdrCooldownBaseMs = 300;
    static constexpr qint64 kIdrCooldownMaxMs = 5000;
    QElapsedTimer m_IdrCooldownTimer;            // Monotonic timer; invalid until first request
    qint64 m_IdrCooldownMs = kIdrCooldownBaseMs; // Current adaptive cooldown
    bool m_IdrOutstanding = false; // True from an effective request until a keyframe is sent
    // The backlog gate is dropping frames, keyframes included: no request is
    // made until the buffer drains, and then one is made at once. Before this
    // (16/09/2026), every keyframe the gate dropped counted as a request that
    // produced nothing, the cooldown doubled under it — 300, 600, 1200 ms —
    // and once the link was back the picture stayed frozen until that timer
    // ran out: half a second to five, per freeze. Guarded by m_VideoMutex.
    bool m_IdrWaitsForDrain = false;
    qint64 m_IdrWaitSinceMs = 0; // For the log line, when the wait began

    // Awaiting IDR: true when a delta was dropped (backpressure or DC not ready).
    // All delta frames are dropped and IDR requested until a keyframe is sent.
    // Guarded by m_VideoMutex.
    bool m_AwaitingIdr = false;

    // Buffered keyframe: if the first IDR arrives before the Video DataChannel
    // is open, we save it here and send it as soon as the DC opens.
    // This prevents a rare black-screen race where the browser receives only
    // delta frames because it missed the initial IDR.
    //
    // Stale buffer detection: tracks whether a NEW keyframe was sent directly
    // (via sendFragmented) while the buffer was held. When the DC opens, Sunshine
    // may send a second IDR (with updated SPS/VUI) while the first is still
    // in the buffer. Sending both creates a race where the browser's decoder
    // configures with stale SPS/PSS, producing wrong colors (green image).
    // DELTA frames arriving do NOT make the buffer stale — they are useless
    // without a keyframe, so we must still send the buffered one.
    QByteArray m_BufferedKeyframe;
    qint64 m_BufferedKeyframePresUs = -1; // presentationTimeUs of the buffered keyframe
    bool m_HaveBufferedKeyframe = false;
    bool m_NewKeyframeArrived = false; // True if a new keyframe was sent directly while buffer held

    // ── HEVC VPS/SPS patching ──────────────────────────────────────────────
    // Applied once to the first HEVC keyframe.  Patches level_idc and
    // max_sub_layers to fix Chrome Windows black screen on decode.
    bool m_HevcPatched = false;

    // ── ICE timeout ──────────────────────────────────────────────────────────
    QTimer* m_IceCheckTimer = nullptr;

    // ── Stats timer (2s interval) ────────────────────────────────────────────
    QTimer* m_StatsTimer = nullptr;

private slots:
    void onIceCheckTimeout();
    void onStatsTimerTick();

signals:
    /// Emitted when ICE fails to reach Connected within 3s after setRemoteDescription.
    /// Used by SignalingServer to trigger WS fallback.
    void iceTimedOut();
};
