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

#include "../MwsrZones.h"
#include "PassTimer.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace mw::native::convert {

/// MWSR's statistics of the native capture, on the GPU, for the sharpness by
/// zone (plan B §12 c, upscaler-mwsr-b.md): the 8 statistics of every pixel of
/// an 8-bit capture, averaged per cell of the 128 × 72 grid MwsrZones.h turns
/// into the zones' codes. One compute pass (kMwsrStatsSource), then a copy of
/// its 9216 cells into a buffer the CPU reads once the GPU is done.
///
/// ── Never waited for ────────────────────────────────────────────────────────
///
/// The frame must not wait for its statistics: dispatch() goes with the
/// conversion, read() comes back later and answers false while the GPU has not
/// finished, and that frame then goes without a grid (the client keeps its
/// sharpness law). The copies sit in a ring of kSlots buffers; a ticket names
/// one dispatch, and the ring's reuse of its buffer kSlots dispatches later
/// retires it, so a reading is never of another frame.
///
/// Only for the 8-bit desktop: the statistics are those of the sRGB-encoded
/// values the bench measured, and an HDR or tone-mapped capture gets no grid.
///
/// ── Two passes, one budget ──────────────────────────────────────────────────
///
/// The full pass reads every pixel; the light one, one pixel in 16, for the
/// light grid of 16 × 9 zones (mwsrzones::decideAll). Both write the same
/// 128 × 72 cells. What the pass costs is timed on the stream's own frames
/// (PassTimer, as the Lanczos-2 is): Bruno's budget of 30/10/2026 is the
/// Lanczos-2's, ResampleCost::kBudgetUs — the full pass if it fits, else the
/// light one, else no grid (D3d11VideoPipeline::takeMwsrZones).
///
/// On the device's immediate context, so on the thread that converts.
class MwsrStats
{
public:
    /// The buffers of the ring.
    static constexpr int kSlots = 4;
    /// Floats in a reading: kCellsX × kCellsY cells, row by row, kStats each.
    static constexpr int kFloats = mwsrzones::kCellsX * mwsrzones::kCellsY * mwsrzones::kStats;

    MwsrStats() = default;
    MwsrStats(const MwsrStats&) = delete;
    MwsrStats& operator=(const MwsrStats&) = delete;

    /// Prepare the pass for @p width × @p height captures on @p device: the
    /// full one, or with @p light the light one. False, with the reason, below
    /// feature level 11_0 (no compute shaders) or for a capture smaller than
    /// the grid (a cell without a pixel).
    bool init(ID3D11Device* device, int width, int height, bool light, std::string& error);

    /// Queue the pass over @p source, a view of the 8-bit capture through its
    /// UNORM format (the encoded values, not light), and the copy of its cells.
    /// The ticket to read() them with; 0 when not initialized.
    uint64_t dispatch(ID3D11ShaderResourceView* source);

    /// The cells of @p ticket into @p cells (kFloats), if the GPU has finished
    /// them. False while it has not, and for good once the ticket's buffer has
    /// gone to a later dispatch. @p wait blocks until the GPU is done — for the
    /// tests: a frame never waits.
    bool read(uint64_t ticket, float* cells, bool wait = false);

    /// What the pass costs this GPU, once: true exactly once per init(), when
    /// enough frames have been timed, with the median GPU time in @p costUs —
    /// the pass and the copy of its cells.
    bool takeCost(int64_t& costUs) { return m_Timer.take(costUs); }

    int width() const { return m_Width; }
    int height() const { return m_Height; }
    bool light() const { return m_Light; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_Shader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_Cells;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_CellsView;
    /// The ring: a staging copy of m_Cells per slot, and the ticket in it.
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_Readback[kSlots];
    uint64_t m_Ticket[kSlots] = {};
    uint64_t m_Last = 0;
    int m_Width = 0;
    int m_Height = 0;
    bool m_Light = false;
    PassTimer m_Timer;
};

} // namespace mw::native::convert
