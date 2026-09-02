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

#include "mw/native/StageStats.h"

#include <cstdio>

namespace mw::native {

const char* toString(Stage stage)
{
    switch (stage) {
    case Stage::Acquire: return "acquire";
    case Stage::Convert: return "convert";
    case Stage::Encode: return "encode";
    case Stage::Queue: return "queue";
    case Stage::Send: return "send";
    case Stage::Total: return "total";
    case Stage::Count: break;
    }
    return "?";
}

// ── LatencyHistogram ────────────────────────────────────────────────────────

int LatencyHistogram::bucketOf(int64_t us)
{
    if (us < 1) return 0;
    // Position of the highest set bit: the octave. Then the three bits under
    // it: which eighth of the octave.
    int exponent = 0;
    for (int64_t v = us; v > 1; v >>= 1)
        ++exponent;
    if (exponent >= kOctaves) return kBuckets - 1;
    const int fraction = exponent >= 3 ? static_cast<int>((us >> (exponent - 3)) & 0x7)
                                       : static_cast<int>((us << (3 - exponent)) & 0x7);
    return exponent * kPerOctave + fraction;
}

int64_t LatencyHistogram::upperEdgeUs(int bucket)
{
    if (bucket < 0) bucket = 0;
    if (bucket >= kBuckets) bucket = kBuckets - 1;
    const int exponent = bucket / kPerOctave;
    const int fraction = bucket % kPerOctave;
    // The bucket covers [ (8+f)·2^e/8, (9+f)·2^e/8 ). Its upper edge is the
    // first value that lands in the NEXT bucket. Below 2^3 the eighths are not
    // whole microseconds, so the edge is rounded up to the next integer.
    const int64_t numerator = static_cast<int64_t>(9 + fraction) << exponent;
    return (numerator + 7) / 8;
}

void LatencyHistogram::add(int64_t us)
{
    if (us < 0) us = 0;
    m_Buckets[static_cast<size_t>(bucketOf(us))]++;
    m_Count++;
    m_Sum += us;
    if (us > m_Max) m_Max = us;
}

void LatencyHistogram::reset()
{
    m_Buckets.fill(0);
    m_Count = 0;
    m_Sum = 0;
    m_Max = 0;
}

int64_t LatencyHistogram::percentileUs(double p) const
{
    if (m_Count <= 0) return 0;
    if (p <= 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    // The rank of the p-th sample, 1-based, never below 1.
    int64_t target = static_cast<int64_t>(p * static_cast<double>(m_Count) + 0.999999);
    if (target < 1) target = 1;
    if (target > m_Count) target = m_Count;

    int64_t seen = 0;
    for (int i = 0; i < kBuckets; ++i) {
        seen += m_Buckets[static_cast<size_t>(i)];
        if (seen >= target) {
            // Never report above the true maximum: the last bucket's edge can
            // be far past the largest sample actually seen.
            const int64_t edge = upperEdgeUs(i);
            return edge < m_Max ? edge : m_Max;
        }
    }
    return m_Max;
}

StageSummary LatencyHistogram::summary() const
{
    StageSummary s;
    s.count = m_Count;
    s.meanUs = meanUs();
    s.p50Us = percentileUs(0.50);
    s.p95Us = percentileUs(0.95);
    s.p99Us = percentileUs(0.99);
    s.maxUs = m_Max;
    return s;
}

// ── StageStats ──────────────────────────────────────────────────────────────

void StageStats::record(Stage stage, int64_t us)
{
    const size_t i = static_cast<size_t>(stage);
    if (i >= static_cast<size_t>(Stage::Count)) return;
    m_Window[i].add(us);
    m_Session[i].add(us);
}

std::array<StageSummary, static_cast<size_t>(Stage::Count)> StageStats::takeWindow()
{
    std::array<StageSummary, static_cast<size_t>(Stage::Count)> out{};
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = m_Window[i].summary();
        m_Window[i].reset();
    }
    return out;
}

std::array<StageSummary, static_cast<size_t>(Stage::Count)> StageStats::session() const
{
    std::array<StageSummary, static_cast<size_t>(Stage::Count)> out{};
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = m_Session[i].summary();
    return out;
}

int64_t StageStats::sessionFrames() const
{
    return m_Session[static_cast<size_t>(Stage::Total)].count();
}

void StageStats::reset()
{
    for (auto& h : m_Window)
        h.reset();
    for (auto& h : m_Session)
        h.reset();
}

std::string
StageStats::describe(const std::array<StageSummary, static_cast<size_t>(Stage::Count)>& summaries)
{
    std::string out;
    int64_t frames = 0;
    for (size_t i = 0; i < summaries.size(); ++i) {
        const StageSummary& s = summaries[i];
        if (s.count <= 0) continue;
        if (frames < s.count) frames = s.count;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s%s %.2f/%.2f/%.2f", out.empty() ? "" : " ",
                      toString(static_cast<Stage>(i)), s.meanUs / 1000.0, s.p95Us / 1000.0,
                      s.p99Us / 1000.0);
        out += buf;
    }
    if (out.empty()) return out;
    out += " ms mean/p95/p99 (n=" + std::to_string(frames) + ")";
    return out;
}

} // namespace mw::native
