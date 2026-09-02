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

#include <cstdint>

namespace mw::native {

/// How long the capture loop waits before trying to reopen a lost duplication
/// again, after @p failures consecutive attempts have failed.
///
/// ── Why the wait is unbounded, and why it grows ─────────────────────────────
///
/// A duplication is lost for three very different lengths of time. A mode set
/// takes a few hundred milliseconds. A UAC prompt takes as long as the person
/// takes to read it. A locked screen — Win+L, a screensaver timeout, a laptop
/// lid — takes minutes, and DXGI refuses to duplicate the secure desktop for
/// the whole of it. The first version of the loop gave up after ten seconds,
/// which made the second and third cases end the session: locking the PC from
/// the stream killed the stream.
///
/// So the loop retries for as long as the session is running. The first tries
/// come quickly, because a mode set IS over quickly and a viewer clicking
/// "Keep these settings" deserves the picture back within the same beat. Then
/// the interval doubles up to a second: an attempt is a device creation and a
/// DuplicateOutput call, which is not free, and a screen that has been locked
/// for a minute will not come back in the next 100 ms.
///
/// 100, 200, 400, 800, then 1000 ms for every attempt after that.
constexpr int64_t restartRetryDelayMs(int failures)
{
    constexpr int64_t kFirstMs = 100;
    constexpr int64_t kCeilingMs = 1000;
    if (failures <= 1) return kFirstMs;
    int64_t delay = kFirstMs;
    for (int i = 1; i < failures && delay < kCeilingMs; ++i)
        delay *= 2;
    return delay < kCeilingMs ? delay : kCeilingMs;
}

} // namespace mw::native
