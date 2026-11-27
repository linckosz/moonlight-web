/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin
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
 * How much of the gaming-mode shortcuts the stream bar can show (HDRHELP).
 *
 * The Fullscreen button sits in the middle of the bar, outside its flow, with
 * the sound group on its left and the exit reminder on its right; the whole
 * group is centred. Nothing of it may lie on the buttons in the flow on the
 * right (the on-screen controller, the keyboard, Share, Stop): on an iPad it
 * did, and they could no longer be tapped (27/11/2026).
 *
 * The helpers give way one by one, the least useful first:
 *   0  the reminder's keys and its words, and the combo on Fullscreen;
 *   1  the reminder's keys only;
 *   2  the reminder's keys only, and no combo on Fullscreen;
 *   3  no reminder either (its combo goes in the button's tooltip).
 */
export const FIT_FULL = 0;
export const FIT_KEYS = 1;
export const FIT_NO_FS_COMBO = 2;
export const FIT_NONE = 3;

/**
 * @param {{
 *   centre: number,     // the bar's middle (x), where Fullscreen is centred
 *   limit: number,      // left edge of the first button right of the middle
 *   gap: number,        // the space kept between the group's parts
 *   fsW: number,        // Fullscreen with its combo
 *   fsComboW: number,   // what the combo adds to Fullscreen
 *   soundW: number,     // speaker + microphone left of Fullscreen, gap included (0: none)
 *   remW: number,       // the reminder with its words
 *   remTextW: number,   // what its words add
 * }} m  widths in CSS pixels, measured with every helper shown
 * @returns {number} the first level whose group ends a gap short of `limit`
 */
export function gamingHelperLevel(m) {
    const fits = (fsW, remW) => {
        const right = remW > 0 ? remW + m.gap : 0;
        const shift = (right - m.soundW) / 2;
        return m.centre - shift + fsW / 2 + right <= m.limit - m.gap;
    };
    if (fits(m.fsW, m.remW)) return FIT_FULL;
    const keysW = m.remW - m.remTextW;
    if (fits(m.fsW, keysW)) return FIT_KEYS;
    const bareFsW = m.fsW - m.fsComboW;
    if (fits(bareFsW, keysW)) return FIT_NO_FS_COMBO;
    return FIT_NONE;
}
