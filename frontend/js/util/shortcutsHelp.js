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
 * The in-stream controls cheat-sheet, in one place.
 *
 * Two surfaces show it and neither can be the source of truth for the other:
 * the slide that flashes over the stream at startup (it auto-hides after 5s,
 * which is why the settings page carries the same thing), and the last section
 * of the settings page, which is where you go when the slide was too quick.
 *
 * Both show exactly one of the two lists, chosen the same way — a device with
 * touch gets the gestures, everything else gets the key combos — so that what
 * the settings page teaches is what the stream actually obeys.
 */
import { t } from '../i18n/i18n.js';
import { escapeHtml } from './escapeHtml.js';

/**
 * The three modifiers of the control combos, in the order they are written on
 * the platform's own keyboard.
 *
 * macOS takes Ctrl where Windows and Linux take Shift: a Cmd+Option+Shift chord
 * collides with system-level macOS shortcuts.
 */
export function comboModifiers(isMac) {
    return isMac ? ['Ctrl', 'Option', 'Cmd'] : ['Shift', 'Ctrl', 'Alt'];
}

/**
 * Keyboard rows: `[action, ...keys]`. Mirrors the combo block in
 * `StreamView.handleKeyDown` — a combo added there belongs here too.
 */
export function keyboardShortcutRows(isMac) {
    const mods = comboModifiers(isMac);
    return [
        [t('stream.scQuit'), ...mods, 'Q'],
        [t('stream.scFullscreen'), ...mods, 'X'],
        [t('stream.scRelease'), ...mods, 'Z'],
        [t('stream.scMouseMode'), ...mods, 'M'],
        [t('stream.scMute'), ...mods, 'S'],
        [t('stream.scMic'), ...mods, 'P'],
        [t('stream.scStats'), ...mods, 'I'],
        [t('stream.scDesktopPrev'), ...mods, '←'],
        [t('stream.scDesktopNext'), ...mods, '→'],
    ];
}

/**
 * Gesture rows: `[action, gesture]`. Mirrors the touch handlers.
 *
 * Touch-screen mode changes the 1-finger meaning from a relative trackpad move
 * to a direct, absolute touch — and the drag that follows scrolls the content
 * instead of moving the cursor. The on-screen controller's row is there only
 * where a tap of four fingers does bring it (`touchPad`).
 */
export function gestureRows(touchScreen, touchPad = false) {
    const rows = touchScreen
        ? [
              [t('stream.tcMoveCursor'), t('stream.tsMoveCursorVal')],
              [t('stream.tcLeftClick'), t('stream.tsLeftClickVal')],
              [t('stream.tcRightClick'), t('stream.tcRightClickVal')],
              [t('stream.tcDrag'), t('stream.tsDragVal')],
              [t('stream.tcScroll'), t('stream.tsScrollVal')],
              [t('stream.tcZoom'), t('stream.tcZoomVal')],
              [t('stream.tcPanZoom'), t('stream.tcPanZoomVal')],
              [t('stream.tcDesktop'), t('stream.tcDesktopVal')],
              [t('stream.tcKeyboard'), t('stream.tcKeyboardVal')],
          ]
        : [
              [t('stream.tcMoveCursor'), t('stream.tcMoveCursorVal')],
              [t('stream.tcLeftClick'), t('stream.tcLeftClickVal')],
              [t('stream.tcRightClick'), t('stream.tcRightClickVal')],
              [t('stream.tcDrag'), t('stream.tcDragVal')],
              [t('stream.tcScroll'), t('stream.tcScrollVal')],
              [t('stream.tcZoom'), t('stream.tcZoomVal')],
              [t('stream.tcPanZoom'), t('stream.tcPanZoomVal')],
              [t('stream.tcDesktop'), t('stream.tcDesktopVal')],
              [t('stream.tcKeyboard'), t('stream.tcKeyboardVal')],
          ];
    if (touchPad) rows.push([t('stream.tcTouchPad'), t('stream.tcTouchPadVal')]);
    return rows;
}

/**
 * TV remote rows: `[action, key]`. Mirrors StreamView's handling of a remote
 * (_remoteOkDown): its arrows and OK go to the host, OK held opens the menu —
 * Back never reaches a page, so the menu is the only way out.
 */
export function remoteRows() {
    return [
        [t('stream.rcMenu'), t('stream.rcMenuVal')],
        [t('stream.rcArrows'), t('stream.rcArrowsVal')],
        [t('stream.rcOk'), t('stream.rcOkVal')],
        [t('stream.rcPointer'), t('stream.rcPointerVal')],
        [t('stream.rcBack'), t('stream.rcBackVal')],
        [t('stream.rcCursor'), t('stream.rcCursorVal')],
    ];
}

/** The title above the list, for the same cases as the rows. */
export function shortcutsTitle({ touch, touchScreen, remote = false }) {
    if (remote) return t('stream.remoteTitle');
    if (!touch) return t('stream.shortcutsTitle');
    return touchScreen ? t('stream.touchScreenTitle') : t('stream.touchTitle');
}

/**
 * The `<div class="shortcuts-slide-grid">` both surfaces render.
 *
 * @param {object} opts
 * @param {boolean} opts.touch      device has touch → gestures instead of keys
 * @param {boolean} [opts.touchScreen] touch-screen mode rather than trackpad
 * @param {boolean} [opts.touchPad] four fingers bring the on-screen controller
 * @param {boolean} [opts.isMac]    Mac keyboard labels (ignored when `touch`)
 * @param {boolean} [opts.remote]   a TV remote (RemoteNav) → its rows, first of all
 */
export function shortcutsGridHtml({
    touch,
    touchScreen = false,
    touchPad = false,
    isMac = false,
    remote = false,
}) {
    let html = '<div class="shortcuts-slide-grid">';
    if (remote || touch) {
        for (const [action, gesture] of remote
            ? remoteRows()
            : gestureRows(touchScreen, touchPad)) {
            html += '<div class="shortcut-row">';
            html += '<span class="shortcut-action">' + escapeHtml(action) + '</span>';
            html +=
                '<span class="shortcut-keys"><kbd class="gesture">' +
                escapeHtml(gesture) +
                '</kbd></span>';
            html += '</div>';
        }
    } else {
        for (const [action, ...keys] of keyboardShortcutRows(isMac)) {
            html += '<div class="shortcut-row">';
            html += '<span class="shortcut-action">' + escapeHtml(action) + '</span>';
            html += '<span class="shortcut-keys">';
            for (let i = 0; i < keys.length; i++) {
                if (i > 0) html += '<span class="shortcut-plus">+</span>';
                html += '<kbd>' + escapeHtml(keys[i]) + '</kbd>';
            }
            html += '</span></div>';
        }
    }
    return html + '</div>';
}
