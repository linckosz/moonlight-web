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

/**
 * @brief Click-to-photon probe, host side: a French flag that pops up on the
 *        host's screen the instant a click is injected into Windows.
 *
 * The browser's LatencyProbe (frontend/js/stream/LatencyProbe.js) sends a
 * click, stamps the moment, and then watches the decoded picture for this flag.
 * The time between the two is the full loop the player feels: input encoding,
 * network, the host's input injection, capture, encode, network again, decode
 * and presentation. No software timestamp covers that loop end to end — the
 * session stats stop at the host's last byte sent and at the browser's draw().
 *
 * How the flag is shown matters as much as when:
 *   - It is an OS window (WS_POPUP, topmost, click-through), never something
 *     drawn into the captured texture or a shader pass: nothing is added to the
 *     capture → encode pipeline, so the measurement does not perturb what it
 *     measures. The window is only ever created while the setting is on.
 *   - It sits at the TOP of the primary screen, at a fixed place whatever the
 *     click position. With tearing allowed the top rows of a frame are the
 *     newest scanned out, so the flag lands in the freshest picture.
 *   - Three flat bands, pure blue / white / red, wide enough to survive 4:2:0
 *     chroma and a downscale to 720p — the browser classifies three pixels,
 *     one per band. Geometry is shared with the frontend as screen fractions
 *     (kLeft..kRight × kTop..kBottom) so it needs no resolution handshake.
 *   - It stays kShowMs: long enough to be in at least one captured frame even
 *     if the first one after the click is skipped or lost.
 *
 * The click is caught with a low-level mouse hook (WH_MOUSE_LL) on a dedicated
 * thread, so it fires whichever host injects it — Sunshine/Apollo on this
 * machine, or our own native host — at the moment Windows delivers it. Only
 * injected clicks (LLMHF_INJECTED) count: a physical click on the host during a
 * run would otherwise be mistaken for the browser's.
 *
 * Debug builds on Windows only, and only from a desktop session (session 0 has
 * no screen to draw on). Everywhere else every function is an inert stub.
 */
namespace LatencyFlag {

/// Flag rectangle as fractions of the primary screen. Mirrored in
/// LatencyProbe.js — change both or neither.
constexpr double kLeft = 0.44;
constexpr double kRight = 0.56;
constexpr double kTop = 0.0;
constexpr double kBottom = 0.05;

/// How long the flag stays up after a click, in milliseconds.
constexpr int kShowMs = 100;

/// True when this build can show the flag: debug build on Windows.
bool isSupported();

/// Start (create the window + hook on their own thread) or stop the probe.
/// Idempotent; a no-op when unsupported.
void setEnabled(bool enabled);

/// Whether the overlay thread is currently running.
bool isEnabled();

} // namespace LatencyFlag
