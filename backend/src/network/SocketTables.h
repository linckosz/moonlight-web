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

#include <QByteArray>

#include <cstdint>

/// Whether something on this machine listens on a port, read from the
/// system's own tables of sockets.
///
/// Read, never tried: a bind made to find out would take the port for a
/// moment from a program that is starting, and in UDP can succeed on a port
/// that is in use.
namespace mw::sockettables {

enum class Listens
{
    No,
    Yes,
    /// The tables could not be read, or not understood: nothing is known.
    Unknown,
};

/// In UDP, a socket of any process bound to `port`, on any address. In TCP,
/// one that listens on it. Windows and Linux; Unknown on any other system.
/// On Linux the tables are those of this process's network namespace: a
/// program in another one (a container) is not seen.
Listens listens(uint16_t port, bool tcp);

/// The same from the text of one Linux table (/proc/net/udp, udp6, tcp or
/// tcp6). Unknown for a text that is not such a table.
Listens inProcTable(const QByteArray& text, uint16_t port, bool tcp);

} // namespace mw::sockettables
