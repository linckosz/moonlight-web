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
 * Whether the host's pointer is drawn in Touch Screen Mode (issue #61).
 *
 * There the finger is the pointer: it taps where it means, so a drawn pointer
 * only jumps to each tap and covers what was just tapped. It is hidden, and
 * comes back while a mouse or a trackpad moves (an iPad with its keyboard
 * case), until the next touch hides it again.
 *
 * Read from pointer events, not mouse events: the mouse events a browser
 * makes up after a tap carry nothing that tells them apart in Safari, while
 * a pointer event names its device (`pointerType`).
 */
export class TouchScreenPointer {
    /** @param {boolean} on  Touch Screen Mode, where the pointer is the finger. */
    constructor(on) {
        this._on = on === true;
        this._mouse = false;
    }

    /** Whether this stream hides the pointer at all. */
    on() {
        return this._on;
    }

    /** Whether the pointer is hidden right now. */
    hidden() {
        return this._on && !this._mouse;
    }

    /**
     * A pointer moved over the picture.
     * @param {string} pointerType  the event's: 'mouse', 'touch' or 'pen'
     * @returns {boolean} whether the pointer has just come back
     */
    moved(pointerType) {
        if (!this._on || pointerType !== 'mouse' || this._mouse) return false;
        this._mouse = true;
        return true;
    }

    /**
     * A finger touched the screen.
     * @returns {boolean} whether the pointer has just been hidden again
     */
    touched() {
        if (!this._on || !this._mouse) return false;
        this._mouse = false;
        return true;
    }
}
