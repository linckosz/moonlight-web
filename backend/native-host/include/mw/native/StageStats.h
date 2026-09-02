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

#include <array>
#include <cstdint>
#include <string>

namespace mw::native {

/// Where a frame spends its time on the host, from the display's present to the
/// last byte leaving the process. Each stage is the difference of two of the
/// stamps EncodedFrame carries (t0..t3) or that the sender adds (t4, t5).
enum class Stage : int
{
    Acquire = 0, ///< t0 present → t1 acquired: how long DXGI took to wake us
    Convert,     ///< t1 acquired → t2 converted: the colour pass, CPU side
    Hold,        ///< t2 converted → t2b due: held for the stream's cadence
    Encode,      ///< t2b due → t3 encoded: the encoder, bitstream readable
    Queue,       ///< t3 encoded → t4 first byte sent: every hop before the wire
    Send,        ///< t4 first byte → t5 last byte: the fragments through SCTP
    Total,       ///< t0 present → t5 last byte: what the client waits for
    Count
};

const char* toString(Stage stage);

/// Mean and tail of one stage over some horizon, in microseconds. Zero-filled
/// when nothing was recorded.
struct StageSummary
{
    int64_t count = 0;
    int64_t meanUs = 0;
    int64_t p50Us = 0;
    int64_t p95Us = 0;
    int64_t p99Us = 0;
    int64_t maxUs = 0;
};

/// A fixed-memory histogram of durations, in microseconds, with log-spaced
/// buckets: eight per octave, from 1 µs to about 16 s.
///
/// Why not keep the samples: a session runs for hours at 240 fps, and the
/// tail of a latency distribution is the whole point — it is what the viewer
/// feels once a second — so the p99 of the WHOLE session has to be available
/// at the end, not just of the last window. Log buckets give every percentile
/// to within one bucket, which is 9 % at any scale: 0.1 ms on a 1 ms stage,
/// 1 ms on a 10 ms one. The mean and the maximum are exact.
///
/// Percentiles report the UPPER edge of their bucket, never the lower or the
/// middle: a number that can only overstate the tail is one that can be
/// trusted when it says a stage is fast.
class LatencyHistogram
{
public:
    static constexpr int kOctaves = 24; // 2^24 µs ≈ 16.8 s
    static constexpr int kPerOctave = 8;
    static constexpr int kBuckets = kOctaves * kPerOctave;

    void add(int64_t us);
    void reset();

    int64_t count() const { return m_Count; }
    int64_t meanUs() const { return m_Count > 0 ? m_Sum / m_Count : 0; }
    int64_t maxUs() const { return m_Max; }

    /// `p` in (0, 1]. Upper edge of the bucket holding the p-th sample.
    int64_t percentileUs(double p) const;

    StageSummary summary() const;

    /// Which bucket a duration lands in, and that bucket's upper edge. Exposed
    /// for the tests, which check the geometry rather than trusting it.
    static int bucketOf(int64_t us);
    static int64_t upperEdgeUs(int bucket);

private:
    std::array<uint32_t, kBuckets> m_Buckets{};
    int64_t m_Count = 0;
    int64_t m_Sum = 0;
    int64_t m_Max = 0;
};

/// The seven stages over two horizons at once: the current reporting window,
/// which the owner drains every few seconds for the live stats, and the whole
/// session, read once at the end for the log.
///
/// Not thread-safe by itself. The stamps come from two threads — capture for
/// t0..t3, the sender for t4..t5 — and the owner holds the lock; keeping it out
/// of here keeps the class testable without a threading model.
class StageStats
{
public:
    void record(Stage stage, int64_t us);

    /// The window since the last call, then start a new one.
    std::array<StageSummary, static_cast<size_t>(Stage::Count)> takeWindow();

    /// Everything since construction or reset().
    std::array<StageSummary, static_cast<size_t>(Stage::Count)> session() const;

    /// Frames counted since construction — the Total stage's count, which is
    /// the one that needs both threads to have reported.
    int64_t sessionFrames() const;

    void reset();

    /// One line, milliseconds with two decimals, mean/p95/p99 per stage:
    /// "acquire 0.06/0.10/0.21 convert 0.31/0.52/0.90 ... total 4.12/5.60/8.30 (n=1234)".
    /// An empty string when nothing was recorded.
    static std::string
    describe(const std::array<StageSummary, static_cast<size_t>(Stage::Count)>& summaries);

private:
    std::array<LatencyHistogram, static_cast<size_t>(Stage::Count)> m_Window{};
    std::array<LatencyHistogram, static_cast<size_t>(Stage::Count)> m_Session{};
};

} // namespace mw::native
