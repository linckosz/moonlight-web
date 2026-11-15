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

#include <QString>

/// Who started a stream, as the server knows it rather than as the browser
/// says it. The browser's client_uniqueid is whatever the page sends; this is
/// read from the request at the start and kept on the slot.
///
/// Three kinds, and the third is a kind of its own rather than an empty token:
/// a slot whose start came with no session and not from this machine is not
/// "the host machine" by default, it is Unknown, and nothing is ever the same
/// owner as an Unknown one.
struct StreamOwner
{
    enum class Kind
    {
        Unknown,     ///< neither of the two below: never matches anybody
        ThisMachine, ///< asked from this machine itself (HttpRequest::isThisMachine)
        Session,     ///< an authenticated session, by its token
    };

    Kind kind = Kind::Unknown;
    /// The session's token when kind is Session, empty otherwise. A secret:
    /// never logged, compared in constant time.
    QString token;

    /// The owner a request names. This machine wins over a cookie it may
    /// carry: on this machine the session gate is skipped, so a cookie there
    /// was never checked and says nothing.
    static StreamOwner of(bool isThisMachine, const QString& sessionToken);

    /// Whether @p other is the same owner. Unknown is nobody's equal, its own
    /// included: two requests that cannot be told apart are not the same one.
    bool sameAs(const StreamOwner& other) const;

    /// The kind's name, for logs. Never the token.
    static const char* kindName(Kind kind);
};
