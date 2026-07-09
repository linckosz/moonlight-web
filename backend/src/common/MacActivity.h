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

// Suppresses macOS App Nap / timer coalescing while a stream session is live.
//
// The server is a windowless QCoreApplication, usually started by a
// LaunchAgent: macOS classifies it as a background process and throttles its
// timers and thread wakeups. That adds tens of ms of latency to the relay
// event loop, the worker→relay pending-frame bound (3) is exceeded, delta
// frames get dropped and the pipeline degenerates into a permanent
// drop → IDR-request → keyframe → drop cycle (visible stutter). Holding an
// NSActivityUserInitiated | NSActivityLatencyCritical assertion during
// streaming restores normal scheduling (same approach as moonlight-qt).
//
// Ref-counted and thread-safe; every beginStreaming() must be balanced by one
// endStreaming(). No-ops on non-Apple platforms.
namespace MacActivity {
#ifdef __APPLE__
void beginStreaming();
void endStreaming();
#else
inline void beginStreaming() {}
inline void endStreaming() {}
#endif
} // namespace MacActivity
