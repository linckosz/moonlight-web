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

#include <QCoreApplication>

/**
 * Entry point for `MoonlightWeb --stream-worker`: hosts ONE streaming session
 * (Sunshine launch + relay graph + moonlight-common-c) in a dedicated child
 * process, so the parent server can run several concurrent sessions despite
 * moonlight-common-c being a process-global singleton.
 *
 * Protocol (JSON lines):
 *   stdin  (parent → worker):
 *     first line       — the session config (see StreamWorkerHost::buildConfig)
 *     {"cmd":"quit"}      — graceful local teardown, then exit(0)
 *     {"cmd":"takenOver"} — notify the browser it was taken over, teardown, exit
 *     {"cmd":"revoked"}   — notify the browser it was revoked, teardown, exit
 *     EOF                 — parent died: teardown, exit
 *   stdout (worker → parent):
 *     {"event":"response","code":<http>,"body":{...}} — the /start HTTP reply
 *     {"event":"ended"}                               — session over (any cause)
 * All logging goes to the log file / stderr; stdout carries only the protocol.
 */
int runStreamWorker(QCoreApplication& app);
