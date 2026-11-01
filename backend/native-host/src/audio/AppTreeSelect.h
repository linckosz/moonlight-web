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
#include <functional>
#include <set>
#include <vector>

namespace mw::native::audio {

/// Which process trees the per-app capture opens (AppAudioCapture.h). Pure, so
/// the rule that keeps an excluded app out of the stream is tested anywhere.
struct TreeChoice
{
    /// One INCLUDE client each.
    std::set<uint32_t> wanted;
    /// Session owners left out because an excluded app runs under them: their
    /// tree would carry it in. Their own sound is lost, by design.
    std::set<uint32_t> unsafe;
};

/// `candidates`: processes that own an audio session and are not excluded.
/// `excluded`: every running process of an excluded app, session or not.
/// `ancestors(pid)`: its real ancestors, nearest first.
///
/// An INCLUDE client also takes the target's descendants, hence the two rules:
///  - a candidate with an excluded descendant is never opened (no leak);
///  - a candidate under another safe candidate is not opened either, since
///    that one's tree already carries it (no double).
inline TreeChoice
chooseCaptureTrees(const std::set<uint32_t>& candidates, const std::vector<uint32_t>& excluded,
                   const std::function<std::vector<uint32_t>(uint32_t)>& ancestors)
{
    TreeChoice out;
    for (uint32_t ex : excluded)
        for (uint32_t a : ancestors(ex))
            if (candidates.count(a)) out.unsafe.insert(a);

    for (uint32_t c : candidates) {
        if (out.unsafe.count(c)) continue;
        bool covered = false;
        for (uint32_t a : ancestors(c))
            if (candidates.count(a) && !out.unsafe.count(a)) {
                covered = true;
                break;
            }
        if (!covered) out.wanted.insert(c);
    }
    return out;
}

} // namespace mw::native::audio
