/*
 * MoonlightWeb — native capture & encoding engine.
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
 */

#pragma once

#include "mw/native/NativeHost.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mw::native::audio {

class AudioPacer;
class OpusEncoder;

/// The half of the audio path that has no platform in it: take PCM whenever a
/// capture happens to produce some, and put one Opus packet on the wire every
/// 5 ms whatever the capture did.
///
/// Windows does not use this. WASAPI signals an event when the endpoint has
/// data, so its loopback owns a single thread that waits on the device and on
/// the 5 ms tick together — one wake-up for both jobs, and no queue at all
/// (WasapiLoopback.h). The push APIs — ScreenCaptureKit here, PipeWire later —
/// call US, on their own dispatch queue, whenever the system feels like it. So
/// the tick has to live somewhere else, and this is it: `push()` from the
/// capture's thread, one thread of our own that encodes and delivers.
///
/// The cadence is the contract the relay depends on: it advances the RTP clock
/// by one frame per packet, so a missing packet is not a late packet, it is a
/// wrong clock. A quiet host therefore sends silence rather than nothing — the
/// pacer's job — and a capture that hands over 40 ms in one burst is drained
/// evenly rather than fired at the wire.
///
/// The encoder is opened by `start()`, on the caller's thread, so a machine
/// without a working libopus says so before the session claims to have audio.
class PacedOpusSink
{
public:
    explicit PacedOpusSink(AudioCallback onPacket);
    ~PacedOpusSink();
    PacedOpusSink(const PacedOpusSink&) = delete;
    PacedOpusSink& operator=(const PacedOpusSink&) = delete;

    /// Open the encoder and start the 5 ms thread. `describe` names the capture
    /// in the session log ("ScreenCaptureKit, 48 kHz stereo").
    bool start(const std::string& describe, std::string& error);

    /// Stop and join. Idempotent, and safe to call while a capture is still
    /// pushing: pushes after this are dropped.
    void stop();

    /// Append captured stereo PCM, interleaved. Thread-safe; called from
    /// whatever thread the capture delivers on.
    void push(const float* interleaved, size_t frames);

    /// Packets delivered since start().
    int64_t packetsSent() const { return m_Packets.load(std::memory_order_relaxed); }

private:
    void run() noexcept;
    void runLoop();

    AudioCallback m_OnPacket;
    std::unique_ptr<OpusEncoder> m_Encoder;
    /// Guards the pacer, which is plain arithmetic with no locking of its own.
    std::mutex m_Mutex;
    std::condition_variable m_Cv;
    std::unique_ptr<AudioPacer> m_Pacer;
    std::thread m_Thread;
    std::atomic<bool> m_Running{false};
    std::atomic<int64_t> m_Packets{0};
};

} // namespace mw::native::audio
