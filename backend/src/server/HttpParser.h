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

HttpRequest parse(const QByteArray& raw);

} // namespace HttpParser
