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
#include <QString>

/*
 * What a home-screen shortcut is made of, for a page reached through the
 * introduction server.
 *
 * On a phone, "Add to Home Screen" reads three things from the page: a name, an
 * icon and the address to open. Served as files, the application gets all three
 * wrong there, and none of it is visible until someone tries:
 *
 *   the address   the manifest says "/". Every machine shares the one origin,
 *                 and the shortcut runs in a storage of its own (iOS keeps a
 *                 web app's storage apart from Safari's), so "/" is not the
 *                 machine you were on — it is no machine at all.
 *   the name      "MoonlightWeb" for every shortcut, when a shortcut can only
 *                 ever open one machine and a person with two cannot tell them
 *                 apart.
 *   the icon      fetched by the system outside the page, so outside the service
 *                 worker that holds the application's files — the request lands
 *                 on the introduction server, which answers every unknown path
 *                 with the entry page. A letter on a grey tile.
 *
 * So the shell and the manifest this host hands down the tunnel carry all three
 * in themselves: the name and the machine's identifier written in, the icons as
 * data: URLs that need no second request. A socket visit is left alone — there
 * the origin IS the machine, and "/" is right.
 *
 * Pure functions over bytes, so the rewriting is tested without a server.
 */
namespace mw::homescreen {

/// "MoonlightWebDev [DualRTX]", or the edition's name alone with no machine name.
QString title(const QString& editionName, const QString& machineName);

/// The machine's name as the shell and the manifest may carry it: for a signed-in
/// session (@p signedIn) only, which is where a shortcut is made. Anyone else,
/// who may hold no more than the entry link, reads the edition's name alone.
QString shownMachineName(const QString& machineName, bool signedIn);

/// The manifest with @p title as its name, the machine's own address as where it
/// opens (only when @p hostId is a rendezvous identifier) — carrying
/// @p handoffKey in its fragment when there is one — and each icon whose bytes
/// are given inlined. Returned unchanged if it is not a JSON object.
QByteArray manifest(const QByteArray& json, const QString& title, const QString& hostId,
                    const QString& handoffKey, const QByteArray& icon192,
                    const QByteArray& icon512);

/// The shell with @p title as its apple-mobile-web-app-title and @p icon180
/// inlined as its apple-touch-icon. Anything it does not find is left as it is.
QByteArray shell(const QByteArray& html, const QString& title, const QByteArray& icon180);

} // namespace mw::homescreen
