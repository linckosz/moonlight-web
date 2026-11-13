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
 * Where the touch buttons go in the stream bar's flow: the gamepad button,
 * then the keyboard button, then Share and Stop. The mark on the left (title,
 * machine) keeps the room before them.
 *
 * The keyboard button is made with the view and the gamepad button only once
 * the stream shows, so whichever comes second finds its place from the other.
 * Share is put before Stop later still, and so lands after both.
 */

/**
 * Puts the keyboard button right of the gamepad button, or before Stop while
 * there is none.
 * @param {HTMLElement} header  the view's .stream-header
 * @param {HTMLElement} kbd
 */
export function placeKeyboardButton(header, kbd) {
    const pad = header.querySelector(':scope > #btn-stream-pad');
    if (pad) {
        pad.after(kbd);
        return;
    }
    header.insertBefore(kbd, header.querySelector(':scope > #btn-stream-quit'));
}

/**
 * Puts the gamepad button left of the keyboard button, or before Stop when the
 * bar has none.
 * @param {HTMLElement} header  the view's .stream-header
 * @param {HTMLElement} pad
 */
export function placeGamepadButton(header, pad) {
    const kbd = header.querySelector(':scope > #btn-stream-keyboard');
    header.insertBefore(pad, kbd || header.querySelector(':scope > #btn-stream-quit'));
}
