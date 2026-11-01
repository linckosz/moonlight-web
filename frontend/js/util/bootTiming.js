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

/**
 * MoonlightWeb — when each step of opening the page happened.
 *
 * One `[perf] boot` console line per step, the first time it happens, in
 * milliseconds since this page started loading. In debug mode the console
 * reaches the server's client log (ClientLog.js), so a slow opening on a phone
 * can be read back step by step from "Download logs" instead of guessed at
 * (01/11/2026: the Host page stayed empty for 5 to 6 s on an iPhone, and nothing
 * said whether the page, the tunnel or the machine was the wait).
 */

const seen = new Set();

/**
 * Note that a step of the opening happened, once per page.
 * @param {string} step short name, no spaces (`tunnel-up`, `hosts-answered`…)
 * @param {string} [detail] what is worth knowing with it (`3 hosts, 1840 ms`)
 */
export function bootMark(step, detail) {
    if (seen.has(step)) return;
    seen.add(step);
    const at = Math.round(globalThis.performance?.now?.() ?? 0);
    console.log(`[perf] boot ${step} at ${at} ms${detail ? ` (${detail})` : ''}`);
}
