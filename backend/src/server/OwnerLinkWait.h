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

#include <QtGlobal>

/**
 * `moonlightweb --wait-link`: how long a caller that can wait goes on asking
 * the running server whether the link its owner's entry points open is settled.
 *
 * The internet link takes a few seconds to come up after a start, and the
 * installer's last page restarts the server seconds before it offers to open
 * that link: a click that came first would land on this machine's own address.
 * So the installer waits where it is already waiting, and the wait is this
 * rule. It asks for no link, so no key is made for it: the link itself is asked
 * for at the click, by the program that opens it.
 *
 * Free of the server and of the channel, so the rule is tested as it is
 * (tests/test_owner_link_wait.cpp).
 */
namespace OwnerLinkWait {

/// One answer of the running server to "is your owner's link settled yet".
enum class Answer
{
    Settled, ///< it knows: the internet link, or this machine's own address
    Pending, ///< the internet link is wanted and has not come up yet
    Refused, ///< it answered, with neither of the two
    Silent,  ///< no answer: not started yet, or not within reach of the caller
};

enum class Next
{
    Done,     ///< settled: the wait is over
    AskAgain, ///< worth another question, after a pause
    GiveUp,   ///< waiting longer would not help
};

/// Bounded on the clock, not on a number of questions: a server that has
/// wedged answers each one with a timeout, and a count of those has no length.
/// The server itself stops saying "not yet" twenty seconds after its start.
constexpr qint64 kCeilingMs = 30000;
/// A server that has not answered once by then is not coming back up.
constexpr qint64 kSilentMs = 15000;
/// Between two questions.
constexpr int kPauseMs = 400;

/// What to do after @p answer, @p elapsedMs into the wait. @p everAnswered: the
/// server has answered at least once since the wait began, this answer included.
inline Next next(Answer answer, bool everAnswered, qint64 elapsedMs)
{
    if (answer == Answer::Settled) return Next::Done;
    // Its considered answer: asking the same thing again would get the same.
    if (answer == Answer::Refused) return Next::GiveUp;
    if (elapsedMs >= kCeilingMs) return Next::GiveUp;
    if (!everAnswered && elapsedMs >= kSilentMs) return Next::GiveUp;
    return Next::AskAgain;
}

} // namespace OwnerLinkWait
