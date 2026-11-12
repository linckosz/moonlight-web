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
 * "Host limit" beside the resolution in the stats overlay (Bruno, 12/11/2026).
 *
 * The native host never streams more pixels than its own screen has
 * (mw::native::frameForDisplay, Bruno's rule of 16/09/2026): 1440p asked of
 * a 1080p screen streams 1080p, whatever the resolution setting — Auto, a
 * rung, Custom. The overlay says so in two words, and only when that screen
 * is what held the stream down: the administrator's ceiling, Auto's ceiling
 * or a decoder's lower the stream too, before the host ever sees the request,
 * and none of them is the host's screen.
 *
 * Only MoonlightWeb's native host tells the page its screen's size (the
 * launch reply's display_width × display_height, then each `displayformat`).
 * Every other host gets an explicit size and says nothing of its screen.
 */

/** frameForDisplay's tolerance: a frame within 0.5 % of the screen's shape
 *  keeps its own. */
const SHAPE_TOLERANCE = 0.005;

/** An encoder's grid shaves a few lines off the screen's size (AV1's 64×16
 *  blocks make 1920×1080 1792×1008); a frame well under the screen was sized
 *  by something else. */
const GRID_SLACK = 0.9;

/** Two sizes this close are one: a mode switched to, a display made to order. */
const SAME_SIZE_PX = 2;

function isSize(s) {
    return !!s && s.width > 0 && s.height > 0;
}

function sameSize(a, b) {
    return (
        Math.abs(a.width - b.width) <= SAME_SIZE_PX && Math.abs(a.height - b.height) <= SAME_SIZE_PX
    );
}

/** "W:H" → W/H, 0 for anything else ("auto"). */
function ratioOf(aspect) {
    const m = /^(\d+):(\d+)$/.exec(aspect || '');
    return m && +m[1] > 0 && +m[2] > 0 ? +m[1] / +m[2] : 0;
}

/**
 * The frame the native host makes of a request before "never larger than the
 * display": frameForDisplay's shaping, step for step. A height to keep takes
 * the screen's shape; a box to fit gives the largest frame of that shape
 * inside it.
 */
function shapedFrame(width, height, display, fitBox) {
    const displayAspect = display.width / display.height;
    const frameAspect = width / height;
    let w = width;
    let h = height;
    if (Math.abs(frameAspect - displayAspect) / displayAspect > SHAPE_TOLERANCE) {
        if (!fitBox || frameAspect > displayAspect) w = Math.round(height * displayAspect) & ~1;
        else h = Math.round(width / displayAspect) & ~1;
    }
    return { width: w & ~1, height: h & ~1 };
}

/**
 * What a launch asked of the host, from the settings it was sent with
 * (app.js _applyResolvedSize), read as the server reads them: the upscale and
 * the display mode only with a box, the fallback box only with the mode.
 * @param {object} settings a launch's streaming settings
 * @returns {{height:number, aspect:string|null, fitBox:boolean, allowUpscale:boolean,
 *            matchDisplay:boolean, fallback:{width:number,height:number}|null}}
 */
export function askedOf(settings) {
    const s = settings || {};
    const fitBox = s.stream_fit_box === true;
    const matchDisplay = fitBox && s.stream_match_display === true;
    const fw = Number(s.stream_fallback_width) || 0;
    const fh = Number(s.stream_fallback_height) || 0;
    return {
        height: Number(s.stream_height) || 0,
        aspect: typeof s.stream_aspect === 'string' ? s.stream_aspect : null,
        fitBox,
        allowUpscale: fitBox && s.stream_allow_upscale === true,
        matchDisplay,
        fallback: matchDisplay && fw > 0 && fh > 0 ? { width: fw & ~1, height: fh & ~1 } : null,
    };
}

/**
 * The native host's screen and what this launch asked of it, for the view
 * (null for every other host, or when the reply names no screen).
 * @param {object} result the launch reply
 * @param {object} settings the settings that launch was sent with
 */
export function hostScreenOf(result, settings) {
    const r = result || {};
    if (r.native !== true) return null;
    const display = {
        width: Number(r.display_width) || 0,
        height: Number(r.display_height) || 0,
    };
    if (!isSize(display)) return null;
    return { display, asked: askedOf(settings) };
}

/**
 * Whether the host's own screen is what capped this stream: what was asked,
 * shaped to that screen, is larger than it, and the frame came out at the
 * screen's size.
 * @param {ReturnType<typeof askedOf>} asked
 * @param {{width:number, height:number}} display the host's screen, in pixels
 * @param {{width:number, height:number}|null} frame the decoded frame
 */
export function hostScreenCaps(asked, display, frame) {
    if (!asked || !isSize(display) || !isSize(frame)) return false;
    if (frame.height < display.height * GRID_SLACK) return false;
    // Height 0 is the host's own size: nothing to cap.
    if (!(asked.height > 0)) return false;
    let height = asked.height;
    // The width as the server derives it; "auto" is the screen's own shape.
    let width = Math.round(height * (ratioOf(asked.aspect) || display.width / display.height)) & ~1;
    let fitBox = asked.fitBox;
    if (asked.allowUpscale) {
        // "Match my screen", or a display made to order: the screen IS the
        // size asked once it took it, and nothing was capped.
        if (!asked.matchDisplay || sameSize({ width, height }, display)) return false;
        // It could not: the host fell back to Auto's box, never upscaled.
        if (isSize(asked.fallback)) {
            width = asked.fallback.width;
            height = asked.fallback.height;
        }
        fitBox = true;
    }
    const out = shapedFrame(width, height, display, fitBox);
    return out.width > display.width || out.height > display.height;
}
