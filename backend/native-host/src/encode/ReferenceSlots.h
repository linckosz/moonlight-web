/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace mw::native::encode {

/// The long-term reference slots an encoder marks its pictures into, and the
/// arithmetic that turns "frame N never arrived" into "predict from this slot".
///
/// NVENC has `NvEncInvalidateRefFrames`: name the lost picture and the driver
/// drops it from the DPB. AMF has no such call, but it has long-term references
/// with a per-picture "force reference bitfield" — the same repair from the
/// other side. Instead of removing the lost picture, name a surviving one:
/// pictures are marked into slots as they are encoded, and when the receiver
/// names a lost frame, every picture from it onwards is unusable (each predicts
/// from the previous), so the next picture is forced onto the newest slot whose
/// frame predates the loss, and the slots holding later frames are forgotten —
/// the driver, in RESET_UNUSED mode, drops them as well. The stream is clean
/// again from that picture on, with no corruption left to sweep.
///
/// ── Reach ───────────────────────────────────────────────────────────────────
///
/// A slot table this small only reaches as far back as its slots: mark every
/// frame into 4 slots and a loss must be reported within 3 frames — 50 ms at
/// 60 fps, 18 ms at 165, less than an Internet round trip. So pictures are
/// marked every `stride` frames, the stride chosen so the slots together span
/// at least kReachMs whatever the frame rate: at 60 fps, 4 slots, stride 2 →
/// 8 frames; at 165 fps, stride 6 → 24 frames. The price is that the forced
/// reference can be up to `stride` frames older than the newest clean one —
/// one larger delta, once, instead of a keyframe.
///
/// Pure arithmetic, no driver: what the encoder records here is what the
/// driver CONFIRMED it marked (the output buffer says), never what was asked.
class ReferenceSlots
{
public:
    /// An amf_int64 bitfield could name 64, but no driver hands out more than a
    /// handful and the table is on the encode path.
    static constexpr int kMaxSlots = 16;
    /// How far back a loss may be reported and still be healed by a delta.
    static constexpr int kReachMs = 125;

    explicit ReferenceSlots(int slots = 0, int stride = 1)
        : m_Count(slots < 0 ? 0 : (slots > kMaxSlots ? kMaxSlots : slots))
        , m_Stride(stride < 1 ? 1 : stride)
    {
        clear();
    }

    /// The marking stride that makes @p slots span kReachMs at @p fps.
    static int strideFor(int fps, int slots)
    {
        if (fps <= 0 || slots <= 0) return 1;
        const int reachFrames = (fps * kReachMs + 999) / 1000;
        const int stride = (reachFrames + slots - 1) / slots;
        return stride < 1 ? 1 : stride;
    }

    int count() const { return m_Count; }
    int stride() const { return m_Stride; }
    bool enabled() const { return m_Count > 0; }
    /// How many frames back a loss can be named and still find a clean slot,
    /// once the table is full.
    int reachFrames() const { return m_Count * m_Stride; }

    /// The slot @p frameNumber should be marked into, or -1 when it is not
    /// this frame's turn. @p always marks regardless of the stride — for a
    /// keyframe, which empties the DPB and had better refill one slot.
    int slotFor(uint32_t frameNumber, bool always = false) const
    {
        if (m_Count <= 0) return -1;
        const auto stride = static_cast<uint32_t>(m_Stride);
        if (!always && frameNumber % stride != 0) return -1;
        return static_cast<int>((frameNumber / stride) % static_cast<uint32_t>(m_Count));
    }

    /// Record that the driver marked @p frameNumber into @p slot.
    void marked(int slot, uint32_t frameNumber)
    {
        if (slot < 0 || slot >= m_Count) return;
        m_Frame[static_cast<size_t>(slot)] = frameNumber;
        m_Held[static_cast<size_t>(slot)] = true;
        m_Ever[static_cast<size_t>(slot)] = true;
    }

    /// True when every slot named by @p bitfield carries a frame numbered below
    /// @p lostFrom.
    ///
    /// A driver may reference a slot other than the one asked for and still be
    /// right: what makes a reference clean is the frame it holds, not its
    /// index. An empty bitfield, an unknown slot, or a slot never marked all
    /// count as unclean — there is nothing to vouch for.
    bool allBefore(uint64_t bitfield, uint32_t lostFrom) const
    {
        if (bitfield == 0) return false;
        for (int s = 0; s < kMaxSlots; ++s) {
            if ((bitfield & bitFor(s)) == 0) continue;
            if (s >= m_Count) return false;
            const auto i = static_cast<size_t>(s);
            if (!m_Ever[i] || m_Frame[i] >= lostFrom) return false;
        }
        // Bits above kMaxSlots name slots this table never handed out.
        return (bitfield >> kMaxSlots) == 0;
    }

    /// The frames behind @p bitfield, for a log line: "slot 0 = frame 128".
    /// Says so when a named slot was never marked; drops nothing silently.
    std::string describe(uint64_t bitfield) const
    {
        std::string out;
        for (int s = 0; s < kMaxSlots; ++s) {
            if ((bitfield & bitFor(s)) == 0) continue;
            if (!out.empty()) out += ", ";
            out += "slot " + std::to_string(s) + " = ";
            if (s >= m_Count)
                out += "not a slot this encoder has";
            else if (!m_Ever[static_cast<size_t>(s)])
                out += "never marked";
            else
                out += "frame " + std::to_string(m_Frame[static_cast<size_t>(s)]);
        }
        return out.empty() ? "no long-term reference at all" : out;
    }

    /// The slot holding the newest frame numbered below @p lostFrom — the one
    /// the next picture can safely predict from — or -1 when no held frame
    /// predates the loss (the loss is out of reach: keyframe).
    int cleanSlotBefore(uint32_t lostFrom) const
    {
        int best = -1;
        for (int s = 0; s < m_Count; ++s) {
            const auto i = static_cast<size_t>(s);
            if (!m_Held[i] || m_Frame[i] >= lostFrom) continue;
            if (best < 0 || m_Frame[i] > m_Frame[static_cast<size_t>(best)]) best = s;
        }
        return best;
    }

    /// The frame a slot holds. Only meaningful for a slot cleanSlotBefore()
    /// returned — on a vendor that names pictures by frame number rather than
    /// by slot index (oneVPL), this is the whole answer.
    uint32_t frameAt(int slot) const
    {
        if (slot < 0 || slot >= m_Count) return 0;
        return m_Frame[static_cast<size_t>(slot)];
    }

    /// Forget every slot holding a frame from @p lostFrom on: they predict from
    /// the lost picture, and the driver drops them when another slot is forced.
    void dropFrom(uint32_t lostFrom)
    {
        for (int s = 0; s < m_Count; ++s) {
            const auto i = static_cast<size_t>(s);
            if (m_Held[i] && m_Frame[i] >= lostFrom) m_Held[i] = false;
        }
    }

    /// Everything is gone — a keyframe emptied the DPB.
    void clear()
    {
        m_Frame.fill(0);
        m_Held.fill(false);
        m_Ever.fill(false);
    }

    int held() const
    {
        int n = 0;
        for (int s = 0; s < m_Count; ++s)
            if (m_Held[static_cast<size_t>(s)]) n++;
        return n;
    }

    static uint64_t bitFor(int slot) { return slot < 0 ? 0 : (uint64_t{1} << slot); }

private:
    int m_Count;
    int m_Stride;
    std::array<uint32_t, kMaxSlots> m_Frame{};
    std::array<bool, kMaxSlots> m_Held{};
    /// Marked at least once and not emptied since. A slot dropped by dropFrom()
    /// keeps its frame number — it is exactly the frame that makes it unusable,
    /// and naming it is how a driver's own choice gets judged.
    std::array<bool, kMaxSlots> m_Ever{};
};

} // namespace mw::native::encode
