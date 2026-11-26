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

class HttpServer;
class Feedback;

// GET  /api/feedback — whether feedback can leave, the message limit, and
//                      exactly what the "include version" box would add.
// POST /api/feedback — {message, email?, include_version?, website?}
//
// The owner's alone (req.isLocal, which --noadmin clears for everyone); a
// guest has no session to reach /api/ with in the first place. Every /api/
// write already passes the cross-site and JSON content-type checks
// (RequestGuard::evaluate) before it gets here.

HttpResponse feedbackStatus(const HttpRequest& req, const Feedback& feedback);
void takeFeedback(const HttpRequest& req, Feedback& feedback, ResponseCallback respond);

void registerFeedbackRoutes(HttpServer& server, Feedback& feedback);
