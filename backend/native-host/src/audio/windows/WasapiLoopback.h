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
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "mw/native/NativeHost.h"

#include <atomic>
#include <cstdint>
#include <future>
#include <string>
#include <thread>

namespace mw::native::audio {

/// What the host is playing, as the player hears it: WASAPI loopback on the
/// default render endpoint, encoded to Opus and delivered as AudioPackets.
///
/// One thread, registered with MMCSS as "Pro Audio", does everything: it drains
/// the loopback buffer whenever the endpoint signals new data or every 5 ms
/// otherwise, hands the samples to an AudioPacer, and encodes and delivers the
/// frames the pacer says are due. There is no queue between capture and encode
/// and no second wake-up: a 5 ms frame is encoded in well under a millisecond,
/// and the packet is on its way before the next one is due.
///
/// Loopback is what the mixer sends to the speakers, so the host keeps hearing
/// its own audio while streaming (v1; see the plan's D5). It follows the DEFAULT
/// device: when the user switches outputs mid-session the old client comes
/// back invalidated, and the thread reopens on the new default while the pacer
/// keeps the wire fed with silence.
///
/// The format asked of the engine is the stream's own — 48 kHz stereo float —
/// and Windows converts from whatever the endpoint runs at (AUTOCONVERTPCM):
/// a 44.1 kHz or 7.1 endpoint costs nothing here but a resampler in the mixer.
class WasapiLoopback
{
public:
    explicit WasapiLoopback(AudioCallback onPacket);
    ~WasapiLoopback();
    WasapiLoopback(const WasapiLoopback&) = delete;
    WasapiLoopback& operator=(const WasapiLoopback&) = delete;

    /// Open the default endpoint and start delivering. Fails — with the reason
    /// — only when there is no encoder or no audio device at all; a device that
    /// disappears later is handled by the thread.
    bool start(std::string& error);

    /// Stop and join. Idempotent.
    void stop();

    /// Packets delivered since start().
    int64_t packetsSent() const { return m_Packets.load(std::memory_order_relaxed); }

private:
    void run() noexcept;
    void runLoop();

    AudioCallback m_OnPacket;
    std::thread m_Thread;
    std::atomic<bool> m_Running{false};
    std::atomic<int64_t> m_Packets{0};
    /// The verdict start() is waiting for: empty string = the device opened.
    /// Points into the thread's own closure; null once resolved.
    std::promise<std::string>* m_FirstOpen = nullptr;
};

} // namespace mw::native::audio
