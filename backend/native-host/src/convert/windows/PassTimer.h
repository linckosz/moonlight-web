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

#include "../ResampleCost.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace mw::native::convert {

/// What one pass costs this GPU, from D3D11 timestamps around it on the
/// stream's own frames: the bookkeeping of ResampleCost (warm-up, then the
/// median of a few dozen frames), and a small ring of queries the GPU answers
/// a frame or two late. Never waited for: a result not back yet is read at a
/// later frame. The Lanczos-2 resample (ColorConvert) and MWSR's statistics
/// (MwsrStats) each have one.
///
/// On the immediate context, so on the thread that converts.
class PassTimer
{
public:
    /// Brackets of this frame's pass: begin() names a slot (-1 when this
    /// frame is not timed), end() closes it. Every call reads first whatever
    /// the GPU has finished.
    int begin(ID3D11Device* device, ID3D11DeviceContext* context);
    void end(ID3D11DeviceContext* context, int slot);

    /// The median GPU time in @p costUs, once enough frames have been timed:
    /// true exactly once until reset(). The queries go with it.
    bool take(int64_t& costUs);

    /// A new device, a new size or a new pass: a new question.
    void reset();

    /// The queries go; the timings already in stay.
    void release();

private:
    void collect(ID3D11DeviceContext* context);

    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> begin;
        Microsoft::WRL::ComPtr<ID3D11Query> end;
        bool pending = false;
    };
    static constexpr int kSlots = 4;
    Slot m_Slots[kSlots];
    int m_Next = 0;
    ResampleCost m_Cost;
    bool m_Taken = false;
};

} // namespace mw::native::convert
