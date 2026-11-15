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

#include "server/StreamOwner.h"

#include <QByteArray>

namespace {

/// Equal bytes, in a time that depends only on the longer input.
bool sameBytes(const QString& a, const QString& b)
{
    const QByteArray ba = a.toUtf8();
    const QByteArray bb = b.toUtf8();
    const int n = qMax(ba.size(), bb.size());
    quint8 diff = ba.size() == bb.size() ? 0 : 1;
    for (int i = 0; i < n; ++i) {
        const quint8 ca = i < ba.size() ? static_cast<quint8>(ba[i]) : 0;
        const quint8 cb = i < bb.size() ? static_cast<quint8>(bb[i]) : 0;
        diff |= static_cast<quint8>(ca ^ cb);
    }
    return diff == 0;
}

} // namespace

StreamOwner StreamOwner::of(bool isThisMachine, const QString& sessionToken)
{
    StreamOwner o;
    if (isThisMachine) {
        o.kind = Kind::ThisMachine;
    } else if (!sessionToken.isEmpty()) {
        o.kind = Kind::Session;
        o.token = sessionToken;
    }
    return o;
}

bool StreamOwner::sameAs(const StreamOwner& other) const
{
    if (kind == Kind::Unknown || other.kind == Kind::Unknown) return false;
    if (kind != other.kind) return false;
    if (kind == Kind::ThisMachine) return true;
    return !token.isEmpty() && sameBytes(token, other.token);
}

const char* StreamOwner::kindName(Kind kind)
{
    switch (kind) {
    case Kind::Unknown: return "unknown";
    case Kind::ThisMachine: return "this-machine";
    case Kind::Session: return "session";
    }
    return "unknown";
}
