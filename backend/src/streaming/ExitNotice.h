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

#include <rtc/datachannel.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

/// Exit notices ("takeover" / "revoked") sent to the browser on the input
/// DataChannel just before a relay tears the session down.
///
/// The notice is what lets the client show a graceful exit instead of a generic
/// "connection lost". Getting it onto the wire is a race: `send()` only queues
/// the message into the SCTP stack, and the caller closes the channel and the
/// PeerConnection immediately afterwards — a close can discard anything still
/// queued. So the send must be followed by a bounded wait for the buffer to
/// drain, which is what `sendAndFlush` does.
namespace exitnotice {

/// Upper bound on the teardown stall. On a healthy association the buffer
/// drains on the first poll (usrsctp takes the message synchronously), so this
/// only bites when the link is already dead — exactly the case where the notice
/// was never going to arrive anyway.
inline constexpr auto kFlushTimeout = std::chrono::milliseconds(250);
inline constexpr auto kPollInterval = std::chrono::milliseconds(5);

/// Send `payload` on `dc`, then wait (bounded by `kFlushTimeout`) until the
/// SCTP stack has taken it. Returns true if the buffer drained, false if the
/// send failed, the channel closed first, or the timeout expired.
///
/// Blocking is deliberate: the caller proceeds straight to `stop()`, so the
/// wait has to happen before this returns to be worth anything.
inline bool sendAndFlush(const std::shared_ptr<rtc::DataChannel>& dc, const std::string& payload)
{
    if (!dc) return false;

    try {
        dc->send(payload);
    } catch (...) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + kFlushTimeout;
    for (;;) {
        try {
            if (dc->bufferedAmount() == 0) return true;
            // A close mid-flush means the notice is gone; no point waiting out
            // the full timeout.
            if (dc->isClosed()) return false;
        } catch (...) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(kPollInterval);
    }
}

} // namespace exitnotice
