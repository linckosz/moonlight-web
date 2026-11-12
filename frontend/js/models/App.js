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

import { t } from '../i18n/i18n.js';

/** The native host's key for "MoonlightWeb Virtual Display"; no other backend sends one. */
const VIRTUAL_DISPLAY_KEY = 'moonlightweb-virtual-display';

/**
 * The order every list of a host's apps shows them in — its card on the hosts
 * page and the sharing board's picker alike: "MoonlightWeb Virtual Display"
 * always first, then alphabetical.
 *
 * Not the host's order: that one moves under the user — the native host lists
 * the primary display first, and the display just streamed becomes primary —
 * so the card they clicked would jump to the front. A fixed order keeps every
 * app where the hand expects it.
 *
 * Sorts in place and returns the array. Takes App models or the host's raw
 * entries alike: both carry `name` and `device`.
 */
export function sortApps(apps) {
    const virtual = (a) => (a?.device?.key === VIRTUAL_DISPLAY_KEY ? 1 : 0);
    return apps.sort(
        (a, b) =>
            virtual(b) - virtual(a) ||
            String(a?.name ?? '').localeCompare(String(b?.name ?? ''), undefined, {
                sensitivity: 'base',
                numeric: true,
            }),
    );
}

/**
 * MoonlightWeb — Remote application data model
 */
export class App {
    constructor(data, hostUuid = null) {
        this.id = data.id || 0;
        this.name = data.name || 'Unknown App';
        this.hdrSupported = data.hdrSupported || false;
        // Absent means yes: every GameStream host serves cover art, and only a
        // host that knows it has none — the native host, whose "apps" are
        // monitors — says so. An older host that says nothing keeps asking.
        this.hasBoxArt = data.boxArt !== false;
        // Only the native host sends it: what the display is (OS, built-in or
        // external screen, monitor name, a stable key), so the card can draw the
        // machine instead of cover art. See ui/HostArt.js.
        this.device = data.device && typeof data.device === 'object' ? data.device : null;
        this.hostUuid = hostUuid;
    }

    get displayName() {
        // The server names it in English; the browser, in its own language.
        if (this.isRemoteDesktopReturn) return t('apps.remoteDesktopReturn');
        if (this.isSignInScreen) return t('apps.signInScreen');
        return this.name;
    }

    /**
     * Is this card "This PC's sign-in screen" — the one card of a Windows
     * native host whose session Remote Desktop left without a screen, when
     * the host offers it? Opening it streams the PC's sign-in screen; once
     * the user has signed in there, the same card streams the PC's screen.
     */
    get isSignInScreen() {
        return this.device?.key === 'console-sign-in';
    }

    /**
     * Is this card "This PC's screen" — the one card of a Windows native host
     * whose session Remote Desktop left without a screen? Opening it brings
     * the session back to the PC's screen, locked, and streams it.
     */
    get isRemoteDesktopReturn() {
        return this.device?.key === 'remote-desktop-return';
    }

    /**
     * Is this a display a stream turned off (issue #39: the other screens off
     * while the virtual display streams)? The native host keeps its card, drawn
     * dark; it opens nothing until the screens come back.
     */
    get isOff() {
        return this.device?.off === true;
    }

    /**
     * Is this card "MoonlightWeb Virtual Display" — a screen made for the
     * stream rather than one that was already there? The resolution choice
     * means something else on it: the display is created at the size asked
     * for (see util/StreamResolution.js). The key is the native host's, and
     * no other backend sends one.
     */
    get isVirtualDisplay() {
        return this.device?.key === VIRTUAL_DISPLAY_KEY;
    }

    /**
     * Is this card an app in its own gamescope (a Linux native host: "Steam Big
     * Picture", or one of the owner's apps)? Its screen is made at the stream's
     * size when the session starts, and kept at that size for its whole life.
     */
    get isGamescope() {
        const key = this.device?.key;
        return (
            key === 'gamescope-steam' ||
            (typeof key === 'string' && key.startsWith('gamescope-app-'))
        );
    }

    /**
     * Where one of the native host's apps opens (the owner's, or a launcher
     * found on the PC): "gamescope", "virtual" (MoonlightWeb Virtual Display)
     * or "primary". Null on a display's card, and on every other host.
     */
    get screen() {
        const screen = this.device?.screen;
        return typeof screen === 'string' ? screen : null;
    }

    /** Is this card one of the native host's apps rather than a display? */
    get isHostApp() {
        return this.screen !== null;
    }

    /**
     * Is the screen behind this card made for the stream — the virtual display,
     * an app opened on it, or a gamescope app? The resolution choice then names
     * an exact size, a phone held upright included, instead of a box the host's
     * shape is fitted into (see util/StreamResolution.js).
     */
    get isMadeForStream() {
        return this.isVirtualDisplay || this.isGamescope || this.screen === 'virtual';
    }

    get boxArtUrl() {
        if (!this.hostUuid || !this.id || !this.hasBoxArt) return null;
        const url = `/api/hosts/${encodeURIComponent(this.hostUuid)}/appasset?appid=${this.id}`;
        // A native host's app image changes under the same id: its hash makes
        // each version a URL of its own, which the browser may keep for good.
        const art = this.device?.art;
        return typeof art === 'string' && art ? `${url}&v=${encodeURIComponent(art)}` : url;
    }
}
