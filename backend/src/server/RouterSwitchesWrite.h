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

#include "server/AppSettings.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <initializer_list>

/// The router's switches, in the two routes that write them.
///
/// POST /api/admin/settings writes the admin page's two boxes, "ice_udp_mux"
/// ("One shared port for streams") and "ice_tcp_443" ("Also through TCP 443"),
/// and only them of the three. POST /api/settings/streaming writes
/// "upnp_enabled", and only it.
///
/// Each route asks its refused() before it writes anything of the body: a
/// value that is not a JSON boolean is a 400, and then no key of the body is
/// written. The streaming route also refuses a body that names a box: those
/// are written by the admin route alone, and a caller sent to the wrong one is
/// told so rather than ignored. Each write() is one write of the file, of what
/// the body names and nothing else (no other key of settings.json, the bench
/// keys included).
namespace mw::routerswitches {

inline const char* const kBoxKeys[] = {"ice_udp_mux", "ice_tcp_443"};
inline const char* const kUpnpKey = "upnp_enabled";

/// The first of @p body's keys among the named ones whose value is not a
/// JSON boolean, as a reason; an empty string when there is none.
inline QString notBoolean(const QJsonObject& body, std::initializer_list<const char*> keys)
{
    for (const char* key : keys) {
        const QString name = QLatin1String(key);
        if (body.contains(name) && !body.value(name).isBool())
            return QStringLiteral("%1 must be true or false").arg(name);
    }
    return {};
}

/// POST /api/admin/settings: why the body is refused, or an empty string.
inline QString adminRefused(const QJsonObject& body)
{
    return notBoolean(body, {kBoxKeys[0], kBoxKeys[1]});
}

/// POST /api/settings/streaming: why the body is refused, or an empty string.
inline QString streamingRefused(const QJsonObject& body)
{
    for (const char* key : kBoxKeys) {
        if (body.contains(QLatin1String(key)))
            return QStringLiteral("%1 is set in the admin settings (POST /api/admin/settings)")
                .arg(QLatin1String(key));
    }
    return notBoolean(body, {kUpnpKey});
}

/// What the admin route writes: the two boxes the body names, nothing else.
struct BoxesWritten
{
    bool sharedPorts = false;
    bool tcp443 = false;
    bool any() const { return sharedPorts || tcp443; }
};

/// POST /api/admin/settings, after adminRefused() said nothing: the boxes
/// the body names, in one write of the file; echoes in @p reply what the file
/// now resolves to.
inline BoxesWritten adminWrite(const QJsonObject& body, AppSettings& settings, QJsonObject& reply)
{
    AppSettings::RouterSwitches s;
    BoxesWritten done;
    if (body.contains(QLatin1String(kBoxKeys[0]))) {
        s.sharedPorts = body.value(QLatin1String(kBoxKeys[0])).toBool();
        done.sharedPorts = true;
    }
    if (body.contains(QLatin1String(kBoxKeys[1]))) {
        s.tcp443 = body.value(QLatin1String(kBoxKeys[1])).toBool();
        done.tcp443 = true;
    }
    if (!done.any()) return done;
    settings.setRouterSwitches(s);
    if (done.sharedPorts) reply[QLatin1String(kBoxKeys[0])] = settings.iceUdpMux();
    if (done.tcp443) reply[QLatin1String(kBoxKeys[1])] = settings.iceTcp443();
    reply[QStringLiteral("status")] = QStringLiteral("saved");
    return done;
}

/// POST /api/settings/streaming, after streamingRefused() said nothing:
/// "upnp_enabled" when the body names it. True when it was written.
inline bool streamingWrite(const QJsonObject& body, AppSettings& settings, QJsonObject& reply)
{
    if (!body.contains(QLatin1String(kUpnpKey))) return false;
    AppSettings::RouterSwitches s;
    s.upnp = body.value(QLatin1String(kUpnpKey)).toBool();
    settings.setRouterSwitches(s);
    reply[QLatin1String(kUpnpKey)] = settings.upnpEnabled();
    reply[QStringLiteral("status")] = QStringLiteral("saved");
    return true;
}

} // namespace mw::routerswitches
