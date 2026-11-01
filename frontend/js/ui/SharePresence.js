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
 */

/**
 * MoonlightWeb — who is in a shared session (issue #47).
 *
 * Everyone watching hears when someone joins or leaves, by name: the owner
 * hears of their guests, a guest of the owner and of the other guests. Before
 * that, the owner only knew a guest was in when the guest said so.
 *
 * Each side reads what it already may: the owner the sharing board's status
 * (ShareMenu polls it), a guest /api/share/player/presence, which names the
 * people on the same host and nothing else. A first look only sets the scene:
 * whoever is already there when a stream starts is not announced.
 */
import { BackendClient } from '../api/BackendClient.js';
import { t } from '../i18n/i18n.js';
import { Icons } from './icons.js';
import { Toast } from './Toast.js';

const POLL_MS = 5000;
const TOAST_MS = 4000;

/**
 * A guest's name: the one the owner typed on the board, else "Player N" as the
 * board numbers its rows (the first guest slot is 2, its row "Player 1").
 * @param {{slot: number, name?: string}} row
 */
export function guestName(row) {
    return (row && row.name) || t('sharing.playerN', { n: row.slot - 1 });
}

/**
 * The guests streaming, as the owner's status lists them.
 * @param {Array<{slot: number, name?: string, streaming?: boolean}>} slots
 * @returns {Map<string, string>} key → name
 */
export function ownerSidePeople(slots) {
    const people = new Map();
    for (const s of slots || []) if (s && s.streaming) people.set(`p${s.slot}`, guestName(s));
    return people;
}

/**
 * The people a guest's presence answer names: the owner first when watching,
 * then the guests, this one included.
 * @param {{self?: number, owner?: boolean, players?: Array<{slot: number, name?: string}>}} data
 * @returns {Map<string, string>} key → name
 */
export function guestSidePeople(data) {
    const people = new Map();
    if (data && data.owner) people.set('owner', t('sharing.presenceHost'));
    for (const p of (data && data.players) || []) people.set(`p${p.slot}`, guestName(p));
    return people;
}

/**
 * Who came and who went between two looks, by name. @p skip is a key never
 * announced: a guest's own arrival is no news to them.
 * @param {Map<string, string>} before
 * @param {Map<string, string>} after
 * @param {string} [skip]
 * @returns {{joined: string[], left: string[]}}
 */
export function presenceChanges(before, after, skip) {
    const joined = [];
    const left = [];
    for (const [key, name] of after) if (key !== skip && !before.has(key)) joined.push(name);
    for (const [key, name] of before) if (key !== skip && !after.has(key)) left.push(name);
    return { joined, left };
}

/** One toast per arrival and per departure. */
export function announceChanges({ joined, left }) {
    for (const name of joined)
        Toast.info(t('sharing.presenceJoined', { name }), { durationMs: TOAST_MS });
    for (const name of left)
        Toast.info(t('sharing.presenceLeft', { name }), { durationMs: TOAST_MS });
}

/**
 * The hover text of a header's count: how many, then who, one per line.
 * @param {string[]} names
 */
export function presenceTitle(names) {
    return [t('sharing.presenceCount', { count: names.length }), ...names].join('\n');
}

/**
 * A guest's view of it: a count in the stream's header (everyone, the guest
 * included), the names on hover or on a tap, and a toast for each change.
 */
export class GuestPresence {
    /**
     * @param {HTMLElement} headerEl the .stream-header element
     * @param {HTMLElement|null} beforeEl insert the count before this node (Leave)
     */
    constructor(headerEl, beforeEl) {
        this.headerEl = headerEl;
        this.beforeEl = beforeEl;
        /** @type {Map<string, string>|null} null until the first look */
        this._people = null;
        this._self = '';
        this._timer = null;
        this.btn = null;
    }

    mount() {
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'stream-presence';
        btn.hidden = true;
        btn.innerHTML = `${Icons.users}<span class="stream-presence-count"></span>`;
        // A phone has no hover: a tap says the names instead.
        btn.addEventListener('click', (e) => {
            e.stopPropagation();
            if (this._people) Toast.info(this._names().join(', '), { durationMs: TOAST_MS });
            btn.blur();
        });
        this.headerEl.insertBefore(btn, this.beforeEl);
        this.btn = btn;
        this.refresh();
        this._timer = setInterval(() => {
            if (!document.hidden) this.refresh();
        }, POLL_MS);
    }

    destroy() {
        if (this._timer) clearInterval(this._timer);
        this._timer = null;
        if (this.btn) this.btn.remove();
        this.btn = null;
    }

    async refresh() {
        let data;
        try {
            data = await BackendClient.playerPresence();
        } catch (err) {
            // Not worth interrupting a stream for: the last count stays.
            return;
        }
        if (!this.btn) return;
        const people = guestSidePeople(data);
        this._self = `p${data.self}`;
        if (this._people) announceChanges(presenceChanges(this._people, people, this._self));
        this._people = people;
        this._paint();
    }

    /** Everyone, this guest marked as such. */
    _names() {
        return [...this._people].map(([key, name]) =>
            key === this._self ? t('sharing.presenceYou', { name }) : name,
        );
    }

    _paint() {
        const names = this._names();
        const count = /** @type {HTMLElement} */ (this.btn.querySelector('.stream-presence-count'));
        count.textContent = String(names.length);
        const title = presenceTitle(names);
        this.btn.title = title;
        this.btn.setAttribute('aria-label', title.replace(/\n/g, ', '));
        this.btn.hidden = false;
    }
}
