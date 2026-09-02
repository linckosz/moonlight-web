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

#pragma once

#include <cstdint>

/// Told, by the thread that writes to the wire, when a frame has left.
///
/// The two stamps close the host-side timeline a frame carries from its
/// present (t₀) to its encode (t₃): t₄ is just before the first fragment goes
/// into the DataChannel, t₅ just after the last. Both are steady_clock
/// microseconds, the same clock as the engine's own stamps.
///
/// Called on the sender thread. An implementation must be cheap and must not
/// block: this sits between two frames on the wire.
class FrameSentSink
{
public:
    virtual ~FrameSentSink() = default;

    /// `frameNumber` is the engine's own gapless counter for the frame, the one
    /// it delivered the frame under — not the relay's fragmentation id.
    virtual void frameSent(uint32_t frameNumber, int64_t firstByteUs, int64_t lastByteUs) = 0;
};
