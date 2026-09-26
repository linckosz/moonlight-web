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

/// How a stream starts on a host whose app outlives the stream (issue #24),
/// from what the host says runs right now.
///
/// Sunshine refuses /launch while ANY app runs, and /resume carries no app id:
/// it joins whatever runs. So the running app decides, never a hint about the
/// browser — joining app A when the viewer clicked B is the one wrong answer.
namespace startchoice {

enum class Verb
{
    Launch,     ///< nothing runs: start the requested app
    Resume,     ///< the requested app already runs: join it
    AppRunning, ///< another app runs: refuse, the viewer decides
    ByHint,     ///< no usable answer: fall back to the older launch↔resume rules
};

/// `running` is the host's `currentgame` (0 = none). `launchRefused` is true
/// when our /launch was just refused: nothing running then means the app ended
/// in between, which the running app cannot settle.
inline Verb decide(int running, int requested, bool launchRefused)
{
    if (running == 0) return launchRefused ? Verb::ByHint : Verb::Launch;
    return running == requested ? Verb::Resume : Verb::AppRunning;
}

} // namespace startchoice
