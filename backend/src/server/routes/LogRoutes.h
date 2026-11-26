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
#include "server/LogScrubber.h"

#include <functional>

class HttpServer;
class LogArchive;

/**
 * Append one batch of a browser's console to the client log. In debug mode,
 * every line; out of it, only its warnings and errors, and only where
 * @p outOfDebug (an owner's page: 404 otherwise, a guest's as before). The
 * level is read on each line, here, whatever the page chose to send.
 *
 * Body: {"id": page id, "page": path, "ua": user agent, "seq": batch number,
 * "dropped": lines the page had to let go, "lines": [{"t": ms, "l": level,
 * "m": text}]}. @p who names the sender in each line ("owner", "guest 2");
 * @p sender is what its lines are counted against (ClientLogBudget). Both are
 * the server's, never the page's. Shared by the owner's route here and the
 * guest's under /api/share/player/.
 */
HttpResponse writeClientLog(const HttpRequest& req, const QString& who, const QString& sender,
                            bool outOfDebug);

/**
 * POST /api/logs/client                  a session's console batch (warnings and errors;
 *                                        everything in debug mode)
 * GET  /api/logs/archive                 the archive's progress (admin)
 * POST /api/logs/archive                 start building it (admin)
 * GET  /api/logs/archive/download        the finished .zip (admin)
 *
 * @p names gives the names to replace in the archive (LogScrubber), asked for
 * each time one is built: hosts come and go.
 */
void registerLogRoutes(HttpServer& server, LogArchive& archive,
                       std::function<LogScrubber::Names()> names);
