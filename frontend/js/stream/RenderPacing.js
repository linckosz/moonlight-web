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

/**
 * How many draws may be in flight at once, on every platform.
 *
 * A draw resolves only when the GPU is done with the frame (WebGPU awaits
 * onSubmittedWorkDone, Canvas2D can await an image-bitmap upload). Drawing
 * strictly one at a time therefore paces the pipeline on a full round trip:
 * frame N+1 is not even submitted until N has fully retired, so the GPU sits
 * idle between two submissions and the throughput ceiling becomes
 * 1000/roundTripMs — 51fps for a 19.5ms round trip, whatever the host sends.
 *
 * With 2, the encode/submit of the next frame overlaps the tail of the current
 * one: same per-frame latency, but no idle gap, so a round trip longer than the
 * frame interval stops forcing a drop (those drops are the visible micro-stutter
 * on fast motion).
 *
 * Set to 1 to go back to strictly serial drawing — the value a renderer that
 * cannot tolerate concurrent frames gets anyway, see VideoRenderer.serialDrawsOnly.
 */
export const MAX_DRAWS_IN_FLIGHT = 2;

/**
 * Effective cap for a renderer: its own constraint wins over the global one.
 * @param {{serialDrawsOnly?: boolean}|null} renderer
 */
export function drawCapFor(renderer) {
    if (!renderer || renderer.serialDrawsOnly) return 1;
    return Math.max(1, MAX_DRAWS_IN_FLIGHT);
}
