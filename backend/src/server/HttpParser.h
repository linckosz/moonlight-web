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

#include "common/Types.h"

/// Stateless parser for a single, already-complete HTTP/1.1 request buffer.
///
/// Extracted from HttpServer so this fragile hand-rolled parsing has no socket
/// dependency and can be unit-tested in isolation (it is a security-relevant
/// surface: it feeds routing and the auth checks). `clientAddress` is NOT set
/// here — the caller fills it from the socket peer.
namespace HttpParser {

/// The request as it came. `malformed` is set when its target decodes to a
/// control character, when its request line holds one, when its method is not
/// a word of capitals (isMethod, read once upper-cased), or when a header's
/// name or value holds one (isHeaderLine).
HttpRequest parse(const QByteArray& raw);

/// A method as a request may name it: 1 to 16 capitals, A to Z.
bool isMethod(const QString& method);

/// A header as a request may carry it: no control character in its name, and
/// none but the tab in its value. Control characters are C0, DEL, C1 and the
/// line and paragraph separators (U+2028, U+2029).
bool isHeaderLine(const QString& name, const QString& value);

/// Whether a request, parsed here or built by its caller (the tunnel builds
/// its own), may be served: not malformed, no control character in its path, a method
/// (isMethod) and headers (isHeaderLine) as a request may carry them.
bool mayBeServed(const HttpRequest& req);

} // namespace HttpParser
