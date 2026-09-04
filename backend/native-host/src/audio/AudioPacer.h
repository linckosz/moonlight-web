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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>

namespace mw::native::audio {

/// Turns an irregular capture feed into one frame every 5 ms.
///
/// The relay that carries the audio advances the RTP clock by exactly one
/// frame per packet, whatever the wall clock did in between, and the browser
/// stretches or squeezes playback to match. So the packet cadence is the
/// contract: the wire wants 200 frames a second, evenly, whether the host is
/// playing a game or sitting in silence.
///
/// The capture side keeps no such promise. WASAPI loopback hands over 10 ms
/// bursts while something renders and nothing at all while nothing does; a
/// device switch leaves a hole; the scheduler wakes the thread a little late.
/// This class sits in between with a small queue and a due-time clock:
///
///  - `push()` appends captured PCM. Beyond `maxQueuedFrames` the OLDEST
///    samples are dropped — a queued sample is latency, and audio a frame
///    behind is worth less to a player than a click they will not hear.
///  - `dueFrames()` says how many frames the clock owes at `nowUs`. The clock
///    never stops: a frame is due every 5 ms from `start()` on.
///  - `pop()` fills one frame from the queue, or with silence when the queue
///    runs dry (an underrun, counted). Silence on the wire is what keeps the
///    receiver's clock honest through a quiet host.
///
/// A thread that fell behind by more than the queue's depth is not asked to
/// catch up with a burst of stale frames: the clock re-anchors and the lost
/// time is a hole the receiver has already concealed.
///
/// Pure arithmetic over floats, no platform: this is the part that is tested.
class AudioPacer
{
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 2;
    /// 5 ms at 48 kHz — the Opus frame the whole pipeline is built around.
    static constexpr int kFrameSamples = 240;
    static constexpr int64_t kFramePeriodUs = 5000;
    static constexpr size_t kFrameFloats = static_cast<size_t>(kFrameSamples) * kChannels;

    explicit AudioPacer(int maxQueuedFrames = 4)
        : m_MaxQueuedFloats(static_cast<size_t>(std::max(1, maxQueuedFrames)) * kFrameFloats)
    {}

    /// Anchor the clock: the first frame is due one period from `nowUs`.
    void start(int64_t nowUs)
    {
        m_NextDueUs = nowUs + kFramePeriodUs;
        m_Started = true;
    }

    /// Append `frames` stereo samples, interleaved. Drops the oldest beyond the
    /// queue cap, counting whole frames' worth.
    void push(const float* interleaved, size_t frames)
    {
        if (!interleaved || frames == 0) return;
        m_Queue.insert(m_Queue.end(), interleaved, interleaved + frames * kChannels);
        trim();
    }

    /// Append `frames` samples of digital silence (a SILENT capture buffer).
    void pushSilence(size_t frames)
    {
        if (frames == 0) return;
        m_Queue.insert(m_Queue.end(), frames * kChannels, 0.0f);
        trim();
    }

    /// How many frames are owed at `nowUs`. Never more than the queue depth: a
    /// longer lapse re-anchors the clock instead.
    int dueFrames(int64_t nowUs)
    {
        if (!m_Started || nowUs < m_NextDueUs) return 0;
        const int64_t owed = (nowUs - m_NextDueUs) / kFramePeriodUs + 1;
        const int maxBurst = static_cast<int>(m_MaxQueuedFloats / kFrameFloats);
        if (owed > maxBurst) {
            m_Reanchors++;
            m_NextDueUs = nowUs - (static_cast<int64_t>(maxBurst) - 1) * kFramePeriodUs;
            return maxBurst;
        }
        return static_cast<int>(owed);
    }

    /// Fill one frame (kFrameFloats floats) and advance the clock. Returns
    /// false when the queue had nothing and silence went out instead.
    bool pop(float* out)
    {
        m_NextDueUs += kFramePeriodUs;
        if (m_Queue.size() >= kFrameFloats) {
            std::copy(m_Queue.begin(), m_Queue.begin() + static_cast<std::ptrdiff_t>(kFrameFloats),
                      out);
            m_Queue.erase(m_Queue.begin(),
                          m_Queue.begin() + static_cast<std::ptrdiff_t>(kFrameFloats));
            return true;
        }
        // A partial frame is not worth a hole: keep what there is for the next
        // tick and send silence now.
        std::memset(out, 0, kFrameFloats * sizeof(float));
        m_Underruns++;
        return false;
    }

    /// Samples waiting, in frames (fractional part dropped).
    size_t queuedFrames() const { return m_Queue.size() / kFrameFloats; }
    int64_t nextDueUs() const { return m_NextDueUs; }

    /// Frames of captured audio thrown away because the queue was full.
    int64_t droppedFrames() const { return m_Dropped; }
    /// Frames that went out as silence because nothing had been captured.
    int64_t underruns() const { return m_Underruns; }
    /// Times the clock gave up catching up and re-anchored on the present.
    int64_t reanchors() const { return m_Reanchors; }

private:
    void trim()
    {
        if (m_Queue.size() <= m_MaxQueuedFloats) return;
        const size_t excess = m_Queue.size() - m_MaxQueuedFloats;
        // Drop whole frames only, so the queue keeps frame alignment: what is
        // left is what the next pop() reads first.
        const size_t dropFloats = ((excess + kFrameFloats - 1) / kFrameFloats) * kFrameFloats;
        const size_t n = std::min(dropFloats, m_Queue.size());
        m_Queue.erase(m_Queue.begin(), m_Queue.begin() + static_cast<std::ptrdiff_t>(n));
        m_Dropped += static_cast<int64_t>(n / kFrameFloats);
    }

    const size_t m_MaxQueuedFloats;
    std::deque<float> m_Queue;
    bool m_Started = false;
    int64_t m_NextDueUs = 0;
    int64_t m_Dropped = 0;
    int64_t m_Underruns = 0;
    int64_t m_Reanchors = 0;
};

} // namespace mw::native::audio
