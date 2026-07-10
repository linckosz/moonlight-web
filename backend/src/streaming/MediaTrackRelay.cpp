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

#include "MediaTrackRelay.h"
#include "ClipboardBridge.h"
#include "MoonlightShim.h"

extern "C" {
#include "Limelight.h"
}

#include <rtc/rtc.hpp>
#include <rtc/plihandler.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QThread>
#include <QDebug>
#include <QMap>
#include <algorithm>
#include <cstring>
#include <random>
#include <chrono>

MediaTrackRelay::MediaTrackRelay(MoonlightShim* shim, QObject* parent)
    : RelayBase(parent)
    , m_Shim(shim)
{
    qInfo() << "[MediaTrackRelay] Created";

    // Video path threading (P2-B):
    //   Default = DirectConnection: onVideoFrame runs on the capture (worker)
    //   thread that emits the signal, removing one Qt event-loop hop before the
    //   RTP send. The send path is serialized with stop()/sendBufferedKeyframe()
    //   by m_VideoMutex. Set MW_MEDIA_QUEUED_VIDEO=1 to roll back to the old
    //   main-thread queued path (no rebuild needed) if it misbehaves.
    bool queuedVideo = qEnvironmentVariableIntValue("MW_MEDIA_QUEUED_VIDEO") != 0;
    m_DirectVideoSend = !queuedVideo;
    qInfo() << "[MediaTrackRelay] Video send mode:"
            << (m_DirectVideoSend ? "direct (capture thread)" : "queued (main thread)");
    connect(m_Shim, &MoonlightShim::videoFrameReady, this, &MediaTrackRelay::onVideoFrame,
            m_DirectVideoSend ? Qt::DirectConnection : Qt::QueuedConnection);
    connect(m_Shim, &MoonlightShim::audioSampleReady, this, &MediaTrackRelay::onAudioSample);
    connect(m_Shim, &MoonlightShim::connectionTerminated, this,
            &MediaTrackRelay::onShimConnectionTerminated);

    // ICE connection timeout: emit iceTimedOut() if PC doesn't reach
    // Connected within 3s after setRemoteDescription().
    m_IceCheckTimer = new QTimer(this);
    m_IceCheckTimer->setSingleShot(true);
    connect(m_IceCheckTimer, &QTimer::timeout, this, &MediaTrackRelay::onIceCheckTimeout);

    // Stats timer: sends periodic stats to the browser via Input DC.
    m_StatsTimer = new QTimer(this);
    m_StatsTimer->setInterval(1000); // matches moonlight-qt's stats window flip
    connect(m_StatsTimer, &QTimer::timeout, this, &MediaTrackRelay::onStatsTimerTick);
}

void MediaTrackRelay::setClipboardEnabled(bool enabled)
{
    m_ClipboardEnabled = enabled;
    if (!enabled) return;
    // Host clipboard changes → push to the browser over the input DC.
    // Context 'this': the slot runs on the relay's (future) thread; the DC
    // send itself is thread-safe. Auto-disconnected when the relay dies.
    connect(ClipboardBridge::instance(), &ClipboardBridge::hostTextChanged, this,
            [this](const QString& text) {
                if (m_Stopping.load() || !m_Connected || !m_InputDc) return;
                QJsonObject m;
                m["type"] = "clipboard";
                m["text"] = text;
                QByteArray j = QJsonDocument(m).toJson(QJsonDocument::Compact);
                try {
                    m_InputDc->send(std::string(j.constData(), j.size()));
                } catch (const std::exception&) {}
            });
}

MediaTrackRelay::~MediaTrackRelay()
{
    qInfo() << "[MediaTrackRelay] Destructor";
    // Static call: dynamic dispatch is meaningless in a destructor.
    MediaTrackRelay::stop();
}

bool MediaTrackRelay::prepare(const rtc::Configuration& config, bool)
{
    if (m_Pc) {
        qWarning() << "[MediaTrackRelay] already prepared";
        return false;
    }

    setupPeerConnection(config);
    return true;
}

bool MediaTrackRelay::setRemoteDescription(const std::string& sdp)
{
    if (!m_Pc) {
        qWarning() << "[MediaTrackRelay] No PeerConnection for setRemoteDescription";
        return false;
    }
    try {
        m_Pc->setRemoteDescription(rtc::Description(sdp));
        qInfo() << "[MediaTrackRelay] Remote description set — starting ICE timeout (3s)";
        if (m_IceCheckTimer) {
            m_IceCheckTimer->start(3000);
        }
        return true;
    } catch (const std::exception& e) {
        qWarning() << "[MediaTrackRelay] setRemoteDescription failed:" << e.what();
        return false;
    }
}

bool MediaTrackRelay::addRemoteCandidate(const std::string& candidate, const std::string& mid)
{
    if (!m_Pc) return false;
    try {
        m_Pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
        return true;
    } catch (const std::exception& e) {
        qWarning() << "[MediaTrackRelay] addRemoteCandidate failed:" << e.what();
        return false;
    }
}

void MediaTrackRelay::setupPeerConnection(const rtc::Configuration& config)
{
    qInfo() << "[MediaTrackRelay] Creating PeerConnection (media mode)";

    m_Pc = std::make_shared<rtc::PeerConnection>(config);

    // --- Local description callback ---
    m_Pc->onLocalDescription([this](const rtc::Description& sdp) {
        qInfo() << "[MediaTrackRelay] Local SDP generated, type=" << sdp.typeString()
                << "hasAudioOrVideo=" << sdp.hasAudioOrVideo()
                << "hasApplication=" << sdp.hasApplication();
        emit signalingSdpReady(std::string(sdp));
    });

    // --- Local ICE candidate callback ---
    m_Pc->onLocalCandidate([this](const rtc::Candidate& candidate) {
        rtc::Candidate modCandidate = candidate;

        // UPnP: rewrite host candidates with public IP
        if (m_ForceHostPublic && !m_PublicIP.empty() && m_PublicPort > 0 &&
            candidate.type() == rtc::Candidate::Type::Host) {
            std::string candStr = candidate.candidate();
            size_t firstSpace = candStr.find(' ');
            bool isIpv4 = true;
            if (firstSpace != std::string::npos &&
                candStr.find(':', firstSpace + 1) != std::string::npos) {
                isIpv4 = false;
            }

            if (isIpv4) {
                // Same-LAN client (incl. NAT-hairpinned public URL): also emit
                // the private host candidate for a direct connection. Gated on
                // m_EmitLanCandidate (false for internet clients) — see
                // DataChannelRelay for the full rationale.
                if (m_EmitLanCandidate)
                    emit signalingIceCandidate(candStr, std::string(candidate.mid()));
                try {
                    modCandidate.changeAddress(m_PublicIP, m_PublicPort);
                    qInfo() << "[MediaTrackRelay] Host candidate ->"
                            << QString::fromStdString(m_PublicIP) << ":" << m_PublicPort
                            << (m_EmitLanCandidate ? "(+ LAN)" : "");
                } catch (const std::exception& e) {
                    qWarning() << "[MediaTrackRelay] Failed to rewrite candidate:" << e.what();
                }
            }
        }

        // UPnP: suppress IPv6 candidates
        if (m_SuppressIPv6) {
            std::string candStr = std::string(modCandidate.candidate());
            size_t space = candStr.find(' ');
            if (space != std::string::npos && candStr.find(':', space + 1) != std::string::npos) {
                return; // Skip IPv6
            }
        }

        emit signalingIceCandidate(std::string(modCandidate.candidate()),
                                   std::string(modCandidate.mid()));
    });

    // --- State change callback ---
    m_Pc->onStateChange([this](rtc::PeerConnection::State state) {
        qInfo() << "[MediaTrackRelay] PC state changed to" << static_cast<int>(state);
        if (state == rtc::PeerConnection::State::Connected) {
            qInfo() << "[MediaTrackRelay] PeerConnection connected";
            // This callback runs on a libdatachannel thread; QTimer must be
            // stopped from its owning (relay) thread.
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    if (m_IceCheckTimer) m_IceCheckTimer->stop();
                },
                Qt::QueuedConnection);
        } else if (state == rtc::PeerConnection::State::Disconnected ||
                   state == rtc::PeerConnection::State::Failed ||
                   state == rtc::PeerConnection::State::Closed) {
            if (!m_Stopping.exchange(true)) {
                m_Connected = false;
                qInfo() << "[MediaTrackRelay] PC disconnected/failed/closed";
                emit sessionEnded();
            }
        }
    });

    // --- Gathering state ---
    m_Pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        qInfo() << "[MediaTrackRelay] ICE gathering state:" << static_cast<int>(state);
    });

    // Create the media tracks + DataChannels
    createTracksAndChannels();
}

void MediaTrackRelay::createTracksAndChannels()
{
    if (!m_Pc) return;

    qInfo() << "[MediaTrackRelay] Creating video track + audio/input DataChannels";

    // ── Video track (H.264, server->browser, RTP) ──────────────────────────
    // libdatachannel generates the SDP media section (m=video) automatically.
    // H264RtpPacketizer with LongStartSequence handles Annex B -> RTP.
    // Payload type 96 (dynamic, standard for H.264 in WebRTC).
    {
        auto videoDesc = rtc::Description::Video("video", rtc::Description::Direction::SendOnly);
        videoDesc.addH264Codec(96); // Uses default profile: 42e01f, packetization-mode=1

        m_VideoTrack = m_Pc->addTrack(videoDesc);
        if (m_VideoTrack) {
            // Generate a random SSRC
            std::random_device rd;
            uint32_t ssrc = static_cast<uint32_t>(rd());

            auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
                ssrc, "video", 96, rtc::H264RtpPacketizer::ClockRate);
            auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::H264RtpPacketizer::Separator::LongStartSequence, rtpConfig);
            // Chain: packetizer → RtcpNackResponder → PliHandler
            // RtcpNackResponder retransmits RTP packets on NACK with a 128-packet buffer.
            auto nackResponder = std::make_shared<rtc::RtcpNackResponder>(128);
            packetizer->addToChain(nackResponder);
            // PLI handler requests a keyframe from Sunshine on decoder PLI.
            // Throttled with backoff: PLI storms during sustained loss must not
            // flood Sunshine with IDR requests (each IDR inflates the bitrate).
            auto pliHandler = std::make_shared<rtc::PliHandler>([this]() {
                if (m_Shim && !m_Stopping.load()) {
                    sendIdrRequestThrottled();
                }
            });
            nackResponder->addToChain(pliHandler);
            m_VideoTrack->setMediaHandler(packetizer);

            m_VideoTrack->onOpen([this]() {
                qInfo() << "[MediaTrackRelay] Video Track open";
                QMetaObject::invokeMethod(
                    this, [this]() { sendBufferedKeyframe(); }, Qt::QueuedConnection);
            });
            m_VideoTrack->onClosed([this]() { qInfo() << "[MediaTrackRelay] Video Track closed"; });

            qInfo() << "[MediaTrackRelay] Video track created (H.264, PT=96)";
        } else {
            qWarning() << "[MediaTrackRelay] Failed to create video track";
        }
    }

    // ── Audio track (Opus, server->browser, RTP) ───────────────────────────
    // Native RTP Opus track instead of a DataChannel: the browser decodes Opus
    // with its own jitter buffer + in-band FEC + packet loss concealment, so a
    // lost UDP packet is masked instead of head-of-line-blocking the whole audio
    // stream (the periodic ~0.5s dropouts). useinbandfec=1 signals the decoder to
    // use the FEC carried in the next Opus packet to reconstruct a lost one.
    // stereo=1;sprop-stereo=1 is REQUIRED: without it libwebrtc instantiates a
    // MONO Opus decoder and downmixes the stereo Sunshine stream (L+R)/2, which
    // plays ~-6 dB quieter than moonlight-qt and loses the stereo image.
    {
        auto audioDesc = rtc::Description::Audio("audio", rtc::Description::Direction::SendOnly);
        audioDesc.addOpusCodec(111, "minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1");

        m_AudioTrack = m_Pc->addTrack(audioDesc);
        if (m_AudioTrack) {
            std::random_device rd;
            uint32_t ssrc = static_cast<uint32_t>(rd());

            auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
                ssrc, "audio", 111, rtc::OpusRtpPacketizer::DefaultClockRate);
            auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
            // NACK responder lets the browser request retransmission of a lost
            // RTP packet (complements Opus FEC/PLC) with a small history buffer.
            packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(64));
            m_AudioTrack->setMediaHandler(packetizer);

            m_AudioTrack->onOpen([this]() { qInfo() << "[MediaTrackRelay] Audio Track open"; });
            m_AudioTrack->onClosed([this]() { qInfo() << "[MediaTrackRelay] Audio Track closed"; });

            qInfo() << "[MediaTrackRelay] Audio track created (Opus, PT=111)";
        } else {
            qWarning() << "[MediaTrackRelay] Failed to create audio track";
        }
    }

    // ── Input DataChannel (bidirectional, JSON text) ──────────────────────
    {
        rtc::DataChannelInit inputConfig;
        inputConfig.negotiated = true;
        inputConfig.id = 1;

        m_InputDc = m_Pc->createDataChannel("input", inputConfig);
        if (m_InputDc) {
            m_InputDc->onOpen([this]() {
                qInfo() << "[MediaTrackRelay] Input DataChannel open";
                m_Connected = true;
                emit dataChannelsOpen();

                if (m_ClipboardEnabled) {
                    // Advertise clipboard sync, then push the current host
                    // clipboard once so copy-before-connect pastes locally.
                    static const char kCaps[] = "{\"type\":\"clipboardcaps\",\"available\":true}";
                    try {
                        m_InputDc->send(std::string(kCaps));
                    } catch (const std::exception&) {}
                    ClipboardBridge::instance()->requestAnnounce();
                }

                // Start periodic stats timer on the relay's own thread: this
                // callback runs on a libdatachannel thread and QTimer::start()
                // must be invoked from the timer's owning thread (otherwise Qt
                // warns "Timers cannot be started from another thread" and the
                // timer never fires → no stats reach the browser).
                QMetaObject::invokeMethod(
                    this,
                    [this]() {
                        if (m_StatsTimer) {
                            m_StatsTimer->start();
                            qInfo() << "[MediaTrackRelay] Stats timer started (2s interval)";
                        }
                    },
                    Qt::QueuedConnection);
            });
            m_InputDc->onClosed(
                [this]() { qInfo() << "[MediaTrackRelay] Input DataChannel closed"; });

            m_InputDc->onMessage([this](const std::variant<rtc::binary, rtc::string>& msg) {
                if (std::holds_alternative<rtc::string>(msg)) {
                    std::string text = std::get<rtc::string>(msg);
                    QMetaObject::invokeMethod(
                        this, [this, text]() { onInputMessage(text); }, Qt::QueuedConnection);
                }
            });
        }
    }

    qInfo() << "[MediaTrackRelay] Tracks+Channels created (video=Track, audio=DC#0, input=DC#1)";
}

// ── Video forwarding (via RTP media track) ──────────────────────────────────────

void MediaTrackRelay::onVideoFrame(const QByteArray& data, int frameType, int)
{
    // Balance the worker→main pending counter (incremented before each emit).
    // Consume the worker-drop flag: recovery on this transport relies on the
    // browser's proactive periodic IDR requests (no PLI API available).
    if (m_Shim) {
        m_Shim->videoFrameDelivered();
        m_Shim->takeWorkerDroppedDelta();
    }

    if (m_Stopping.load()) {
        static int dropCount = 0;
        if (++dropCount <= 3) qInfo() << "[MediaTrackRelay] onVideoFrame dropped — m_Stopping=true";
        return;
    }

    bool isKeyframe = (frameType == 1);

    // Serialize the video send path with stop() and sendBufferedKeyframe().
    // In direct mode this runs on the capture thread; the lock guarantees the
    // Video Track and the buffered keyframe are not torn down mid-send.
    std::lock_guard<std::mutex> lk(m_VideoMutex);

    // Re-check after acquiring the lock: stop() may have run while we waited.
    if (m_Stopping.load()) return;

    // Buffer keyframes arriving before the Video Track is ready
    if (isKeyframe && (!m_VideoTrack || !m_VideoTrack->isOpen())) {
        m_BufferedKeyframe = data;
        m_HaveBufferedKeyframe = true;
        qInfo() << "[MediaTrackRelay] Buffered keyframe size=" << data.size()
                << "(Track ready=" << (m_VideoTrack && m_VideoTrack->isOpen()) << ")";
        return;
    }

    if (!m_VideoTrack || !m_VideoTrack->isOpen()) {
        static int noTrackCount = 0;
        if (++noTrackCount <= 5)
            qInfo() << "[MediaTrackRelay] onVideoFrame dropped — Video Track not ready";
        return;
    }

    // Derive the RTP timestamp from real arrival time (any FPS, real jitter).
    uint32_t rtpTs = computeRtpTimestamp();

    // Create FrameInfo with isKeyFrame flag. The H264RtpPacketizer reads this
    // to decide STAP-A (single NAL) vs FU-A (fragmented) packetization.
    auto frameInfo = std::make_shared<rtc::FrameInfo>(rtpTs);
    frameInfo->isKeyFrame = isKeyframe;

    // Copy data into binary format for sendFrame
    rtc::binary bin(static_cast<size_t>(data.size()));
    if (data.size() > 0) {
        std::memcpy(bin.data(), data.constData(), static_cast<size_t>(data.size()));
    }

    try {
        m_VideoTrack->sendFrame(std::move(bin), *frameInfo);
    } catch (const std::exception& e) {
        if (!m_Stopping.load()) {
            qWarning() << "[MediaTrackRelay] sendFrame error:" << e.what();
        }
        return;
    }

    m_FrameCount++;
}

uint32_t MediaTrackRelay::computeRtpTimestamp()
{
    auto now = std::chrono::steady_clock::now();
    if (!m_HaveFirstFrameTime) {
        m_FirstFrameTime = now;
        m_HaveFirstFrameTime = true;
        return 0;
    }
    // 90 kHz clock: ticks = elapsed_seconds * 90000. uint32 wrap is expected.
    auto elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(now - m_FirstFrameTime).count();
    return static_cast<uint32_t>((elapsedUs * 90000) / 1000000);
}

void MediaTrackRelay::sendBufferedKeyframe()
{
    // Same lock as onVideoFrame: the buffered keyframe may be written from the
    // capture thread while this runs on the main thread (direct mode).
    std::lock_guard<std::mutex> lk(m_VideoMutex);

    if (!m_HaveBufferedKeyframe) return;
    if (m_Stopping.load() || !m_VideoTrack || !m_VideoTrack->isOpen()) return;

    qInfo() << "[MediaTrackRelay] Sending buffered keyframe, size=" << m_BufferedKeyframe.size();

    auto frameInfo = std::make_shared<rtc::FrameInfo>(computeRtpTimestamp());
    frameInfo->isKeyFrame = true;

    rtc::binary bin(static_cast<size_t>(m_BufferedKeyframe.size()));
    if (m_BufferedKeyframe.size() > 0) {
        std::memcpy(bin.data(), m_BufferedKeyframe.constData(),
                    static_cast<size_t>(m_BufferedKeyframe.size()));
    }

    try {
        m_VideoTrack->sendFrame(std::move(bin), *frameInfo);
        // Keyframe sent: reset the IDR request backoff (recovery completed).
        m_IdrOutstanding.store(false, std::memory_order_release);
        m_IdrCooldownMs.store(kIdrCooldownBaseMs, std::memory_order_release);
    } catch (const std::exception& e) {
        qWarning() << "[MediaTrackRelay] sendBufferedKeyframe error:" << e.what();
    }

    m_BufferedKeyframe.clear();
    m_HaveBufferedKeyframe = false;
}

// ── Audio forwarding (native RTP Opus track) ───────────────────────────────────

void MediaTrackRelay::onAudioSample(const QByteArray& data)
{
    if (m_Stopping.load()) return;

    // Serialize against track teardown in stop().
    std::lock_guard<std::mutex> lk(m_AudioMutex);
    if (m_Stopping.load() || !m_AudioTrack || !m_AudioTrack->isOpen()) {
        static int notReady = 0;
        if (++notReady <= 3)
            qInfo() << "[MediaTrackRelay] onAudioSample dropped — audio track not ready";
        return;
    }

    auto frameInfo = std::make_shared<rtc::FrameInfo>(m_AudioRtpTs);
    // Advance by the negotiated Opus frame size (48 kHz clock): one clean tick per
    // packet. A jittery arrival-time clock makes NetEq time-stretch → robotic audio.
    m_AudioRtpTs += static_cast<uint32_t>(m_Shim ? m_Shim->audioSamplesPerFrame() : 240);

    rtc::binary bin(static_cast<size_t>(data.size()));
    if (data.size() > 0)
        std::memcpy(bin.data(), data.constData(), static_cast<size_t>(data.size()));

    try {
        m_AudioTrack->sendFrame(std::move(bin), *frameInfo);
    } catch (const std::exception& e) {
        if (!m_Stopping.load())
            qWarning() << "[MediaTrackRelay] audio sendFrame error:" << e.what();
    }
}

void MediaTrackRelay::onShimConnectionTerminated(int errorCode)
{
    qInfo() << "[MediaTrackRelay] Shim connection terminated, code=" << errorCode;
    if (!m_Stopping.exchange(true)) {
        m_Connected = false;
        emit sessionEnded();
    }
}

// ── Input handling (from libdatachannel callback, marshaled to main thread) ──

void MediaTrackRelay::onInputMessage(const std::string& message)
{
    if (m_Stopping.load() || !m_Connected) return;

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(message));
    if (!doc.isObject()) {
        qWarning() << "[MediaTrackRelay] Invalid input JSON";
        return;
    }

    QJsonObject msg = doc.object();
    QString type = msg["type"].toString();

    if (type == "keydown" || type == "keyup") {
        bool down = (type == "keydown");
        int vk = msg["keyCode"].toInt(0);
        QString code = msg["code"].toString();
        char mods = 0;
        if (msg["ctrlKey"].toBool(false)) mods |= 0x02;
        if (msg["shiftKey"].toBool(false)) mods |= 0x01;
        if (msg["altKey"].toBool(false)) mods |= 0x04;
        if (msg["metaKey"].toBool(false)) mods |= 0x08;

        short keyCode;
        char flags = 0;

        // International keys without standard US VK equivalents:
        // IntlBackslash (ISO key next to left Shift) and IntlRo (JIS \ key)
        // need NON_NORMALIZED mode: Sunshine injects the keyCode as a raw VK
        // (not a US-layout scancode), so the host's active layout resolves it.
        if (code == "IntlBackslash") {
            keyCode = 0xE2; // VK_OEM_102 (ISO <> key)
            flags = SS_KBE_FLAG_NON_NORMALIZED;
        } else if (code == "IntlRo") {
            keyCode = 0xC1; // VK_ABNT_C1 (JIS Ro key)
            flags = SS_KBE_FLAG_NON_NORMALIZED;
        } else {
            keyCode = static_cast<short>(vk);
            flags = 0;
        }
        m_Shim->sendKeyEvent(keyCode, down, mods, flags);
    } else if (type == "mousemove") {
        // Absolute mouse position (non-gaming mode)
        if (msg.contains("x") && msg.contains("y") && msg.contains("referenceWidth") &&
            msg.contains("referenceHeight")) {
            short x = static_cast<short>(msg["x"].toInt(0));
            short y = static_cast<short>(msg["y"].toInt(0));
            short refW = static_cast<short>(msg["referenceWidth"].toInt(0));
            short refH = static_cast<short>(msg["referenceHeight"].toInt(0));
            m_Shim->sendMousePosition(x, y, refW, refH);
        } else {
            // Legacy / gaming mode: relative mouse movement
            short dx = static_cast<short>(msg["dx"].toInt(0));
            short dy = static_cast<short>(msg["dy"].toInt(0));
            m_Shim->sendMouseMove(dx, dy);
        }
    } else if (type == "mousedown" || type == "mouseup") {
        bool down = (type == "mousedown");
        int button = msg["button"].toInt(1);
        m_Shim->sendMouseButton(down, button);
    } else if (type == "mousewheel") {
        short delta = static_cast<short>(msg["delta"].toInt(0));
        m_Shim->sendMouseScroll(delta);
    } else if (type == "textinput") {
        // Virtual/soft keyboard text (UTF-8) — forwarded as a text event.
        m_Shim->sendUtf8Text(msg["text"].toString());
    } else if (type == "locksync") {
        // Align the host's toggle locks (Num/Caps/Scroll) with the client's,
        // sent once per session on the browser's first real keyboard event.
        m_Shim->syncLockKeys(msg["numLock"].toBool(false), msg["capsLock"].toBool(false),
                             msg["scrollLock"].toBool(false));
    } else if (type == "clipboardpaste") {
        // Browser Ctrl/Cmd+V: commit the client text to the host clipboard,
        // then inject the paste chord (main-thread hop keeps that order).
        // Only meaningful when the streamed host is this machine.
        if (m_ClipboardEnabled) {
            ClipboardBridge::instance()->pasteFromClient(m_Shim, msg["text"].toString(),
                                                         msg["injectCtrl"].toBool(false));
        }
    } else if (type == "request_idr") {
        qInfo() << "[MediaTrackRelay] Requesting IDR frame via DataChannel (browser)";
        sendIdrRequestThrottled();
    } else if (type == "requestidr") {
        qInfo() << "[MediaTrackRelay] Requesting IDR frame from Sunshine (browser request)";
        sendIdrRequestThrottled();
    } else if (type == "ping") {
        // Respond with pong (mirror the browser's timestamp for RTT calculation).
        int seq = msg["seq"].toInt(0);
        double ts = msg["ts"].toDouble(0);
        QJsonObject pong;
        pong["type"] = "pong";
        pong["seq"] = seq;
        pong["ts"] = ts;
        QByteArray pongJson = QJsonDocument(pong).toJson(QJsonDocument::Compact);
        if (m_InputDc && !m_Stopping.load()) {
            try {
                m_InputDc->send(std::string(pongJson.constData(), pongJson.size()));
            } catch (const std::exception& e) {
                if (!m_Stopping.load()) {
                    qWarning() << "[MediaTrackRelay] Pong send error:" << e.what();
                }
            }
        }
    } else {
        qWarning() << "[MediaTrackRelay] Unknown input type:" << type;
    }
}

// ── Stats timer (1s interval) ───────────────────────────────────────────────────

void MediaTrackRelay::onStatsTimerTick()
{
    if (m_Stopping.load() || !m_Connected) return;
    if (!m_InputDc || !m_InputDc->isOpen()) return;

    double hostRttMs = 0.0;
    if (m_Shim) {
        hostRttMs = m_Shim->hostRttMs();
    }

    QJsonObject stats;
    stats["type"] = "stats";
    stats["hostRttMs"] = hostRttMs;
    // Media track mode: decode happens natively in the browser via RTP,
    // so backend decode latency is not meaningful here.
    stats["decodeLatencyUs"] = 0;
    // Sunshine capture→encode latency (RTP extension), averaged over the frames
    // of this stats window. 0 when the host doesn't report it.
    stats["hostProcMs"] = m_Shim ? m_Shim->takeHostProcessingLatencyMs() : 0.0;
    QByteArray statsJson = QJsonDocument(stats).toJson(QJsonDocument::Compact);

    try {
        m_InputDc->send(std::string(statsJson.constData(), statsJson.size()));
    } catch (const std::exception& e) {
        if (!m_Stopping.load()) {
            qWarning() << "[MediaTrackRelay] Stats send error:" << e.what();
        }
    }
}

// ── ICE timeout ────────────────────────────────────────────────────────────────

void MediaTrackRelay::onIceCheckTimeout()
{
    if (m_Stopping.load()) return;
    if (m_Connected) return;
    if (m_Pc) {
        auto state = m_Pc->state();
        if (state == rtc::PeerConnection::State::Connected) return;
    }

    qWarning() << "[MediaTrackRelay] ICE timeout — PC did not reach Connected within 3s."
               << "Emitting iceTimedOut().";

    // In auto mode: the relay tracking will catch this and trigger tryNext().
    // In explicit mode: SignalingServer will start WS fallback.
    emit iceTimedOut();
}

// ── Stop ────────────────────────────────────────────────────────────────────────

void MediaTrackRelay::notifyClientTakenOver()
{
    sendExitNotice("takeover");
}

void MediaTrackRelay::notifyClientRevoked()
{
    sendExitNotice("revoked");
}

void MediaTrackRelay::sendExitNotice(const char* type)
{
    // Best-effort exit notice on the input DC before stop() closes it.
    if (!m_InputDc || m_Stopping.load()) return;
    QByteArray json = QJsonDocument(QJsonObject{{"type", type}}).toJson(QJsonDocument::Compact);
    try {
        m_InputDc->send(std::string(json.constData(), json.size()));
    } catch (...) {}
}

void MediaTrackRelay::stop()
{
    // Marshal onto the relay's session thread when called cross-thread (main:
    // /quit, Session::quit, auto-fallback). Queued (non-blocking) avoids deadlock.
    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(this, [this]() { stop(); }, Qt::QueuedConnection);
        return;
    }

    if (m_Stopping.exchange(true)) {
        qInfo() << "[MediaTrackRelay::stop] Already stopping, skip";
        return;
    }

    qInfo() << "[MediaTrackRelay::stop] ENTER, frameCount=" << m_FrameCount;

    // Stop timers
    if (m_IceCheckTimer) {
        m_IceCheckTimer->stop();
    }
    if (m_StatsTimer) {
        m_StatsTimer->stop();
    }

    m_Connected = false;
    {
        // Serialize with the (possibly concurrent) capture-thread send path.
        std::lock_guard<std::mutex> lk(m_VideoMutex);
        m_BufferedKeyframe.clear();
        m_HaveBufferedKeyframe = false;
    }

    // Reset IDR throttle state for the next session
    m_IdrCooldownMs.store(kIdrCooldownBaseMs, std::memory_order_release);
    m_LastIdrRequestMs.store(0, std::memory_order_release);
    m_IdrOutstanding.store(false, std::memory_order_release);

    // Close DataChannels
    auto closeDc = [](std::shared_ptr<rtc::DataChannel>& dc, const char* name) {
        if (dc) {
            qInfo() << "[MediaTrackRelay] Closing" << name << "DataChannel";
            try {
                dc->close();
            } catch (...) {}
            dc.reset();
        }
    };

    closeDc(m_InputDc, "input");

    // Close the audio track under the audio send lock, so an in-flight audio
    // sendFrame finishes before the track is destroyed.
    {
        std::lock_guard<std::mutex> lk(m_AudioMutex);
        if (m_AudioTrack) {
            qInfo() << "[MediaTrackRelay] Closing audio track";
            try {
                m_AudioTrack->close();
            } catch (...) {}
            m_AudioTrack.reset();
        }
    }

    // Close Video Track under the send lock, so an in-flight sendFrame on the
    // capture thread finishes before the track is destroyed.
    {
        std::lock_guard<std::mutex> lk(m_VideoMutex);
        if (m_VideoTrack) {
            qInfo() << "[MediaTrackRelay] Closing video track";
            try {
                m_VideoTrack->close();
            } catch (...) {}
            m_VideoTrack.reset();
        }
    }

    // Close PeerConnection
    if (m_Pc) {
        qInfo() << "[MediaTrackRelay] Closing PeerConnection";
        try {
            m_Pc->close();
        } catch (...) {}
        m_Pc.reset();
    }

    qInfo() << "[MediaTrackRelay::stop] EXIT";
}

void MediaTrackRelay::requestIdrFrame()
{
    if (m_Stopping.load() || !m_Shim) return;
    qInfo() << "[MediaTrackRelay] requestIdrFrame: forwarding to MoonlightShim";
    sendIdrRequestThrottled();
}

void MediaTrackRelay::sendIdrRequestThrottled()
{
    if (m_Stopping.load() || !m_Shim) return;

    using namespace std::chrono;
    const int64_t nowMs =
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    int64_t last = m_LastIdrRequestMs.load(std::memory_order_acquire);
    const int64_t cooldown = m_IdrCooldownMs.load(std::memory_order_acquire);
    if (last != 0 && nowMs - last < cooldown) {
        return; // Absorbed — cooldown not elapsed yet
    }
    // CAS claims the send slot; a concurrent caller (PLI thread vs main thread)
    // that loses the race is absorbed like a throttled request.
    if (!m_LastIdrRequestMs.compare_exchange_strong(last, nowMs, std::memory_order_acq_rel)) {
        return;
    }

    // Backoff: previous request never produced a keyframe — double the cooldown
    // (up to 5 s). Reset when a keyframe is sent (onVideoFrame keyframe path).
    if (m_IdrOutstanding.exchange(true, std::memory_order_acq_rel)) {
        m_IdrCooldownMs.store(std::min<int64_t>(cooldown * 2, kIdrCooldownMaxMs),
                              std::memory_order_release);
    }
    m_Shim->requestIdrFrame();
}

void MediaTrackRelay::setPublicAddress(const std::string& publicIP, uint16_t publicPort)
{
    m_PublicIP = publicIP;
    m_PublicPort = publicPort;
    qInfo() << "[MediaTrackRelay] UPnP public address set:" << QString::fromStdString(publicIP)
            << ":" << publicPort;
}
