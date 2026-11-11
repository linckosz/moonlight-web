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

#include <string>
#include <string_view>

namespace mw::native::platform {

/**
 * Sign-in screen mode: a worker that streams the console's sign-in screen and
 * nothing else (Windows; the launcher starts it with --signin-screen, and the
 * server's RemoteDesktopReturnPolicy says when).
 *
 * The console shows the sign-in screen of a session nobody is signed in on.
 * Its picture lives on the `Winlogon` desktop, and a SYSTEM worker attached
 * there can duplicate it and type into it, exactly as on a lock screen. What
 * the mode guards against is everything else that session may show next:
 * somebody signing in on it with another account turns it into their
 * session, and its `Default` desktop is theirs. So, per picture and per input:
 *
 *  - the input desktop is `Winlogon`;
 *  - for a picture, the capturing thread's own desktop too (Desktop
 *    Duplication copies that one);
 *  - nobody is signed in on the session.
 *
 * The first time one of these is not so, the stream ends for good: nothing is
 * sent or injected after it, and the mode is never left. The decisions are
 * here, without a Windows call, so that a test pins them on every platform.
 */

/// The desktop of the sign-in screen, the lock screen and UAC.
inline constexpr std::string_view kSignInDesktop = "Winlogon";

/// Whether @p name is that desktop. Windows compares desktop names without
/// case; an empty name (one that could not be read) is not.
inline bool isSignInDesktop(std::string_view name)
{
    if (name.size() != kSignInDesktop.size()) return false;
    for (size_t i = 0; i < name.size(); ++i) {
        char a = name[i];
        char b = kSignInDesktop[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

/// What one check saw.
struct SignInLook
{
    /// The desktop that has the input now; empty when it could not be read.
    std::string inputDesktop;
    /// For a picture: the capturing thread's own desktop. Not looked at for an
    /// input (@p forPicture false in signInRefusal).
    std::string threadDesktop;
    /// Somebody is signed in on this session — or Windows would not say.
    bool sessionHasUser = true;
};

/// Why a picture (@p forPicture) or an input may NOT go through, in English;
/// empty when it may.
inline std::string signInRefusal(const SignInLook& look, bool forPicture)
{
    if (look.sessionHasUser) return "somebody is signed in on this session";
    if (!isSignInDesktop(look.inputDesktop))
        return "the screen shows the \"" +
               (look.inputDesktop.empty() ? std::string("(unreadable)") : look.inputDesktop) +
               "\" desktop, not the sign-in screen";
    if (forPicture && !isSignInDesktop(look.threadDesktop))
        return "the capture stands on the \"" +
               (look.threadDesktop.empty() ? std::string("(unreadable)") : look.threadDesktop) +
               "\" desktop, not the sign-in screen";
    return {};
}

} // namespace mw::native::platform
