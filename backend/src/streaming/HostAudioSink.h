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

namespace HostAudioSink {

/// Restore Sunshine's virtual audio sinks ("Steam Streaming Speakers" /
/// "Steam Streaming Microphone" render endpoints) to 100% volume, unmuted.
///
/// Sunshine captures the virtual sink via WASAPI loopback, which is
/// POST-device-volume — any attenuation on that endpoint attenuates the whole
/// Opus stream for every Moonlight client. And because the sink becomes the
/// host's default output during a stream, pressing the host's volume keys
/// mid-stream silently lowers it, and the value persists across sessions
/// (observed at 64% → "stream is ~60% quieter than Parsec", issue #1).
///
/// Only endpoints whose friendly name contains "Steam Streaming" are touched —
/// never the user's physical speakers. Windows-only; no-op elsewhere. Call at
/// stream start, and only when the streamed Sunshine host IS this machine.
void ensureFullVolume();

} // namespace HostAudioSink
