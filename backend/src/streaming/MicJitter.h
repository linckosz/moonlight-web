/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 MoonlightWeb contributors
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
#include <mutex>
#include <vector>

// Between the network and the host's audio device. Packets come in at the
// network's pace, in 20 ms bursts with whatever jitter the link adds; the
// device pulls at its own clock, a few ms at a time. Nothing here stretches
// time: the ring primes to a small target before it plays, and when it runs
// dry it goes silent and primes again — a dropout the listener hears as one,
// rather than a mic that slowly falls behind. A ring that grows past its cap
// (the device's clock slower than the browser's, or a burst after a stall)
// drops its oldest samples back down to the target, for the same reason:
// latency on a voice link is worse than a click.
namespace mw::mic {

class JitterRing
{
public:
    /// @param targetFrames what is held before playing, and kept after a trim.
    /// @param capFrames past this, the oldest samples go.
    JitterRing(size_t targetFrames, size_t capFrames)
        : m_Target(targetFrames)
        , m_Cap(std::max(capFrames, targetFrames + 1))
        , m_Buffer(m_Cap + 2) // one past the cap to notice it, one to tell full from empty
    {}

    /// The network side: decoded samples, in order.
    void push(const float* pcm, size_t frames)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (size_t i = 0; i < frames; ++i) {
            m_Buffer[m_Write] = pcm[i];
            m_Write = (m_Write + 1) % m_Buffer.size();
            if (sizeLocked() > m_Cap) {
                const size_t drop = sizeLocked() - m_Target;
                m_Read = (m_Read + drop) % m_Buffer.size();
                m_Trimmed += drop;
                ++m_Trims;
            }
        }
        m_Pushed += frames;
    }

    /// The device side: always fills `frames` samples, with silence where the
    /// ring has none (still priming, or run dry). Returns how many were real.
    size_t pull(float* out, size_t frames)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Primed) {
            if (sizeLocked() < m_Target) {
                std::fill(out, out + frames, 0.0f);
                return 0;
            }
            m_Primed = true;
        }
        const size_t have = std::min(frames, sizeLocked());
        for (size_t i = 0; i < have; ++i) {
            out[i] = m_Buffer[m_Read];
            m_Read = (m_Read + 1) % m_Buffer.size();
        }
        if (have < frames) {
            std::fill(out + have, out + frames, 0.0f);
            m_Primed = false;
            ++m_Underruns;
        }
        m_Pulled += have;
        return have;
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return sizeLocked();
    }

    struct Stats
    {
        uint64_t pushed = 0;
        uint64_t pulled = 0;
        uint64_t underruns = 0;
        uint64_t trims = 0;
        uint64_t trimmed = 0;
    };
    Stats stats() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return {m_Pushed, m_Pulled, m_Underruns, m_Trims, m_Trimmed};
    }

private:
    size_t sizeLocked() const { return (m_Write + m_Buffer.size() - m_Read) % m_Buffer.size(); }

    const size_t m_Target;
    const size_t m_Cap;
    std::vector<float> m_Buffer;
    size_t m_Read = 0;
    size_t m_Write = 0;
    bool m_Primed = false;
    uint64_t m_Pushed = 0;
    uint64_t m_Pulled = 0;
    uint64_t m_Underruns = 0;
    uint64_t m_Trims = 0;
    uint64_t m_Trimmed = 0;
    mutable std::mutex m_Mutex;
};

} // namespace mw::mic
