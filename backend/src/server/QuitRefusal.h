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

#include "common/QuitSignals.h"
#include "common/Types.h"

#include <QJsonObject>

namespace mw {

/// What a request that would start a stream gets once the server has begun
/// to quit (mw::quitting()): asked before anything is touched on the host — a
/// launch, a take-over, a display turned on — that nobody would undo after
/// the exit.
inline HttpResponse quittingRefusal()
{
    QJsonObject obj;
    obj[QStringLiteral("error")] = QStringLiteral("server_quitting");
    return HttpResponse::json(obj, 503);
}

} // namespace mw
