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

#include <functional>

class HttpServer;
class ErrorReports;

/**
 * Hand one page's error lines to the error reports (ErrorReports.h). Bounded
 * here, before anything is parsed: a page sends a few lines once a stream has
 * ended, not more. Answers the same whether the reports are on or off: the page
 * has nothing to do about either.
 */
HttpResponse takeErrorLines(const HttpRequest& req, ErrorReports& reports, const QString& who);

/**
 * POST /api/errors/client          an owner's page (any session; "host" for this
 *                                  PC's own browser)
 * POST /api/share/player/errors    a guest's page: @p guestSlot reads the slot
 *                                  its cookie holds, -1 when it holds none
 *
 * Body: {"sent": ms, "dropped": n, "lines": [{"t": ms, "n": count, "m": text}]}.
 */
void registerErrorRoutes(HttpServer& server, ErrorReports& reports,
                         std::function<int(const HttpRequest&)> guestSlot);
