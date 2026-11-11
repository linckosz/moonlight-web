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
 * The player number a Windows native host gave one of our pads (its
 * `padnumber` message: XInput's user index, 0 for player 1).
 *
 * A Sunshine session on the same PC that was only disconnected (a Stop where
 * the app outlives the stream) keeps its own virtual pad plugged in, as player
 * 1. The native host's pad then comes second, and a game listening to player 1
 * (Vanquish, seen 05/11) never hears it. Closing that session frees player 1;
 * the pad, plugged in again, takes it.
 */
import { Host } from '../models/Host.js';

/** XInput has four players; anything else is not a number to act on. */
const PLAYERS = 4;

/**
 * How soon after the question a press is too soon to be its answer. The toast
 * can sit over the on-screen controller's buttons: a double tap on A, meant for
 * the game, would otherwise ask and answer at once.
 */
export const CONFIRM_GAP_MS = 600;

/**
 * Whether the host put our pad behind one of something else's: a player
 * number past the controller's own.
 * @param {{index?: unknown, number?: unknown}} msg
 */
export function padBehindAnother(msg) {
    const index = msg && msg.index;
    const number = msg && msg.number;
    return (
        Number.isInteger(index) &&
        Number.isInteger(number) &&
        /** @type {number} */ (index) >= 0 &&
        /** @type {number} */ (number) < PLAYERS &&
        /** @type {number} */ (number) > /** @type {number} */ (index)
    );
}

/**
 * The session on this very PC that may hold player 1: a host other than the
 * native one, here, paired, with an app running. Null when there is none,
 * and then something else holds it (a pad of the host's own) that this page
 * cannot free.
 * @param {Array<object>} hostsData  /api/hosts' `hosts`
 * @returns {Host | null}
 */
export function sessionHere(hostsData) {
    for (const data of hostsData || []) {
        if (!data) continue;
        const host = new Host(data);
        if (
            host.isLocalHost &&
            host.backendType !== 'native' &&
            host.isPaired &&
            host.currentGameId > 0
        ) {
            return host;
        }
    }
    return null;
}

/**
 * Says so, once per stream, when the host put a pad behind another, and offers
 * to close the session that holds player 1 where it is one this page can
 * close. The offer asks twice before it closes anything: a tap meant for the
 * game can land on it. Everything it touches comes in `deps`, so it is tested
 * on its own.
 */
export class PadNumberNotice {
    /**
     * @param {{
     *   hosts: () => Promise<{hosts?: Array<object>}>,
     *   stopSession: (uuid: string) => Promise<unknown>,
     *   replug: () => void,
     *   warn: (text: string, options?: object) => void,
     *   error: (text: string) => void,
     *   t: (key: string, vars?: object) => string,
     *   now?: () => number,
     * }} deps
     */
    constructor(deps) {
        this._deps = deps;
        this._now = deps.now || (() => Date.now());
        this._said = false;
        this._closing = false;
        this._askedAt = 0;
    }

    /**
     * A `padnumber` message from the host.
     * @param {{index?: unknown, number?: unknown}} msg
     */
    async note(msg) {
        if (this._said || !padBehindAnother(msg)) return;
        // Said once: a pad plugged in again gets a number again, and a viewer
        // who chose to keep the other session does not want to hear it twice.
        this._said = true;
        const { t } = this._deps;
        const number = /** @type {number} */ (msg.number) + 1;
        let holder = null;
        try {
            holder = sessionHere((await this._deps.hosts()).hosts);
        } catch {
            holder = null;
        }
        if (!holder) {
            this._deps.warn(t('stream.padNumberOther', { number }), { durationMs: 10000 });
            return;
        }
        const uuid = holder.uuid;
        this._deps.warn(t('stream.padNumberTaken', { number }), {
            sticky: true,
            action: {
                label: t('stream.padNumberClose'),
                onClick: () => this._ask(uuid),
            },
        });
    }

    /**
     * The first press: the question, in a toast of its own (a toast goes when
     * its button is pressed). It stays until answered or dismissed.
     */
    _ask(uuid) {
        const { t } = this._deps;
        this._askedAt = this._now();
        this._deps.warn(t('stream.padNumberConfirm'), {
            sticky: true,
            action: {
                label: t('stream.padNumberConfirmClose'),
                onClick: () => this._answer(uuid),
            },
        });
    }

    /** The second press: a yes, unless it came too soon to be one. */
    async _answer(uuid) {
        if (this._now() - this._askedAt < CONFIRM_GAP_MS) {
            this._ask(uuid);
            return;
        }
        await this._close(uuid);
    }

    /** The answer: the session closed, then every pad plugged in again. */
    async _close(uuid) {
        if (this._closing) return;
        this._closing = true;
        try {
            await this._deps.stopSession(uuid);
            this._deps.replug();
        } catch {
            this._deps.error(this._deps.t('stream.padNumberCloseFailed'));
        } finally {
            this._closing = false;
        }
    }
}
