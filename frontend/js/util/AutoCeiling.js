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
 * What this browser has earned from a host: how many pixels a second Auto may
 * ask of it.
 *
 * The budget of util/StreamResolution.js is one number for every machine: it
 * protects a N95 and costs a PC with a dedicated GPU. So a machine starts on it
 * and climbs, one rung at a time, by holding a stream that really moved
 * (stream/AutoCeilingMeter.js). A stream that does not hold costs one rung.
 *
 * One entry per host and per codec: holding 4K120 in HEVC says nothing of H.264.
 * Kept in localStorage, so per origin: a direct address and the rendezvous learn
 * apart. Every machine reached through the rendezvous shares its origin and every
 * native host answers to one uuid, so each such machine has a memory of its own
 * (autoCeilingKey). Nothing goes to the server.
 *
 * A memory that keeps a bad session stays stuck (the enhancer's ladder did), so:
 * a rung lost is regained by holding the one below for twice as long, up to
 * HOLD_MAX_MS, and every failure is forgotten after FAIL_TTL_MS.
 *
 * A host that crashes says little of this machine: short of memory, it crashes
 * at any rung. So its first crash at a rung costs nothing, and a second at the
 * same rung, in another stream within FAIL_TTL_MS, costs the rung (hostCrash).
 */

import { PIXEL_RATE_BUDGET } from './StreamResolution.js';

/** The rungs, in pixels a second. No step is more than ×1.43, and the top is
 *  3840×2160 at 120 fps (995 M). */
export const CEILING_RUNGS = Object.freeze([
    PIXEL_RATE_BUDGET,
    330000000,
    450000000,
    500000000,
    700000000,
    1000000000,
]);

/** The most a phone or a tablet may climb to. */
export const HANDHELD_MAX_CEILING = 450000000;

export const AUTO_CEILING_KEY = 'mw_auto_ceiling';
/** The switch: '0' holds Auto at PIXEL_RATE_BUDGET and measures nothing, 'log'
 *  measures and only says what it would do, '1' learns, as it does by default. */
export const AUTO_LEARN_KEY = 'mw_auto_learn';
/** What the switch means when it says nothing. */
const DEFAULT_LEARN_MODE = 'on';

/** How long a stream must hold its rung to earn the next one. */
export const HOLD_BASE_MS = 5 * 60 * 1000;
/** The most a past failure can ask for. */
export const HOLD_MAX_MS = 40 * 60 * 1000;
/** After this long, a failure no longer counts. */
export const FAIL_TTL_MS = 7 * 24 * 60 * 60 * 1000;
/** How many hosts and codecs are remembered; the oldest used goes first. */
export const ENTRIES_KEPT = 32;

const VERSION = 1;

/**
 * Where one machine's memory is kept.
 * @param {string|null} [instance] tunnelHostId(): its rendezvous identifier, or
 *   nothing on a direct address, where the origin is the machine
 * @returns {string} the localStorage key
 */
export function autoCeilingKey(instance) {
    return instance ? AUTO_CEILING_KEY + ':' + instance : AUTO_CEILING_KEY;
}

/**
 * Whether Auto learns here, only measures, or does neither.
 * @param {Storage|null} [storage] localStorage by default
 * @returns {'off'|'log'|'on'}
 */
export function autoLearnMode(storage) {
    let v = null;
    try {
        const store = storage === undefined ? localStorage : storage;
        v = store ? store.getItem(AUTO_LEARN_KEY) : null;
    } catch (e) {
        // Storage refused: the default.
    }
    if (v === '0') return 'off';
    if (v === '1') return 'on';
    if (v === 'log') return 'log';
    return DEFAULT_LEARN_MODE;
}

/**
 * The highest rung this kind of device may reach. A TV, a device of at most
 * 1 GB or Power Saving stay on the base budget: they learn nothing.
 * @param {{tv?:boolean, handheld?:boolean, lowMemory?:boolean, powerSave?:boolean}} [device]
 * @returns {number} pixels a second
 */
export function deviceMaxCeiling(device) {
    const d = device || {};
    if (d.tv || d.lowMemory || d.powerSave) return PIXEL_RATE_BUDGET;
    if (d.handheld) return HANDHELD_MAX_CEILING;
    return CEILING_RUNGS[CEILING_RUNGS.length - 1];
}

/**
 * The budget a preview that knows no host should assume: what was learned from
 * the host launched last, under what this device may reach.
 * @param {{tv?:boolean, handheld?:boolean, lowMemory?:boolean, powerSave?:boolean}} [device]
 * @param {Storage|null} [storage] localStorage by default
 * @param {string|null} [instance] the machine this page is on (autoCeilingKey)
 * @returns {number} pixels a second
 */
export function previewCeiling(device, storage, instance) {
    if (autoLearnMode(storage) !== 'on') return PIXEL_RATE_BUDGET;
    const top = deviceMaxCeiling(device);
    if (top <= PIXEL_RATE_BUDGET) return PIXEL_RATE_BUDGET;
    return autoCeilingMemory(storage, undefined, instance).lastCeiling(top);
}

/** The rung at or just under @p pixels, never under the first. */
function snapToRung(pixels) {
    let rung = CEILING_RUNGS[0];
    for (const r of CEILING_RUNGS) if (r <= pixels) rung = r;
    return rung;
}

/** The next rung up, or 0 at the top. */
export function rungAbove(ceiling) {
    const i = CEILING_RUNGS.indexOf(snapToRung(ceiling));
    return i + 1 < CEILING_RUNGS.length ? CEILING_RUNGS[i + 1] : 0;
}

/** The next rung down, or 0 at the base budget. */
export function rungBelow(ceiling) {
    const i = CEILING_RUNGS.indexOf(snapToRung(ceiling));
    return i > 0 ? CEILING_RUNGS[i - 1] : 0;
}

/** How long to hold a rung to reach one lost @p failures times. */
export function holdMsFor(failures) {
    const n = failures > 0 ? failures : 0;
    return Math.min(HOLD_MAX_MS, HOLD_BASE_MS * 2 ** n);
}

/**
 * The memory of one browser. Storage refused or broken: nothing is remembered
 * and every answer is the base budget.
 * @param {Storage|null} [storage] localStorage by default
 * @param {() => number} [clock] Date.now by default
 * @param {string|null} [instance] the machine this page is on (autoCeilingKey)
 */
export function autoCeilingMemory(storage, clock, instance) {
    const storeKey = autoCeilingKey(instance);
    let store = storage;
    if (store === undefined) {
        try {
            store = localStorage;
        } catch (e) {
            store = null;
        }
    }
    const nowMs = clock || (() => Date.now());

    const blank = () => ({ v: VERSION, last: '', hosts: {} });
    const read = () => {
        try {
            const all = JSON.parse(store.getItem(storeKey) || 'null');
            if (!all || all.v !== VERSION || typeof all.hosts !== 'object' || !all.hosts)
                return blank();
            return all;
        } catch (e) {
            return blank();
        }
    };
    const write = (all) => {
        try {
            store.setItem(storeKey, JSON.stringify(all));
        } catch (e) {
            // Storage refused: nothing remembered.
        }
    };
    const keyOf = (host, codec) => host + '|' + (codec || 'hevc');

    /** The failures still counting on an entry, the expired ones gone. */
    const freshFails = (entry, now) => {
        const kept = {};
        const fails = entry && typeof entry.fails === 'object' && entry.fails ? entry.fails : {};
        for (const rung of Object.keys(fails)) {
            const f = fails[rung];
            if (f && f.n > 0 && now - f.at < FAIL_TTL_MS) kept[rung] = f;
        }
        return kept;
    };

    /** When the host crashed at each rung, the expired ones gone. */
    const freshCrashes = (entry, now) => {
        const kept = {};
        const crashes =
            entry && typeof entry.crashes === 'object' && entry.crashes ? entry.crashes : {};
        for (const rung of Object.keys(crashes)) {
            const at = crashes[rung];
            if (at > 0 && now - at < FAIL_TTL_MS) kept[rung] = at;
        }
        return kept;
    };

    /** Change one entry and store the lot, oldest used out first. */
    const update = (key, change) => {
        const now = nowMs();
        const all = read();
        const entry = all.hosts[key] || { ceiling: CEILING_RUNGS[0], trial: false, openAt: 0 };
        entry.fails = freshFails(entry, now);
        entry.crashes = freshCrashes(entry, now);
        change(entry);
        if (!Object.keys(entry.crashes).length) delete entry.crashes;
        entry.at = now;
        all.hosts[key] = entry;
        const keys = Object.keys(all.hosts).sort(
            (a, b) => (all.hosts[b].at || 0) - (all.hosts[a].at || 0),
        );
        for (const k of keys.slice(ENTRIES_KEPT)) delete all.hosts[k];
        write(all);
        return entry;
    };

    const failuresAt = (entry, rung, now) => {
        const f = freshFails(entry, now)[rung];
        return f ? f.n : 0;
    };

    return {
        /**
         * What this host and codec have earned, on this device.
         * @param {string} host the host's uuid
         * @param {string} codec
         * @param {number} [deviceMax] deviceMaxCeiling(), the top rung by default
         * @returns {{ceiling:number, trial:boolean, crashed:boolean, holdMs:number}}
         *   the budget to use, whether it has yet to be held, whether the last
         *   stream that tried it never came back, and how long to hold it to
         *   earn the next rung.
         */
        entry(host, codec, deviceMax) {
            const top = deviceMax > 0 ? deviceMax : Infinity;
            const e = read().hosts[keyOf(host, codec)];
            const ceiling = Math.min(top, snapToRung(e && e.ceiling));
            const next = rungAbove(ceiling);
            return {
                ceiling,
                trial: !!(e && e.trial) && ceiling > CEILING_RUNGS[0],
                crashed: !!(e && e.openAt > 0),
                holdMs: holdMsFor(next ? failuresAt(e, next, nowMs()) : 0),
            };
        },

        /** The budget of the host and codec launched last, for a preview that
         *  knows no host. @param {number} [deviceMax] */
        lastCeiling(deviceMax) {
            const top = deviceMax > 0 ? deviceMax : Infinity;
            const all = read();
            const e = all.hosts[all.last];
            return Math.min(top, snapToRung(e && e.ceiling));
        },

        /** A launch at this rung is on its way: if it is never closed, it crashed. */
        open(host, codec) {
            const key = keyOf(host, codec);
            const now = nowMs();
            update(key, (e) => {
                e.openAt = now;
            });
            const all = read();
            all.last = key;
            write(all);
        },

        /** The stream ended, or the page is going away, on its own terms. */
        close(host, codec) {
            const key = keyOf(host, codec);
            const e = read().hosts[key];
            if (!e || !e.openAt) return;
            update(key, (entry) => {
                entry.openAt = 0;
            });
        },

        /** Remember which host a launch that risks nothing was for. */
        noteLast(host, codec) {
            const all = read();
            all.last = keyOf(host, codec);
            write(all);
        },

        /**
         * A rung has been held: its failures are gone, and it is no more a
         * trial, unless the stream has already earned the next one.
         * @param {string} host
         * @param {string} codec
         * @param {number} [rung] the rung that held; the stored one by default
         */
        hold(host, codec, rung) {
            update(keyOf(host, codec), (e) => {
                const stored = snapToRung(e.ceiling);
                const held = rung > 0 ? snapToRung(rung) : stored;
                delete e.fails[held];
                if (held === stored) e.trial = false;
            });
        },

        /**
         * A rung has been held long enough: climb one. Only from that rung, so
         * that two views of one session cannot climb twice on one proof.
         * @param {string} host
         * @param {string} codec
         * @param {number} [deviceMax]
         * @param {number} [fromRung] the rung that held; the stored one by default
         * @returns {number} the new ceiling, or 0 when there is nowhere to go
         */
        raise(host, codec, deviceMax, fromRung) {
            const top = deviceMax > 0 ? deviceMax : Infinity;
            const key = keyOf(host, codec);
            const stored = snapToRung((read().hosts[key] || {}).ceiling);
            if (fromRung > 0 && snapToRung(fromRung) !== stored) return 0;
            const next = rungAbove(stored);
            if (!next || next > top) return 0;
            update(key, (e) => {
                e.ceiling = next;
                e.trial = true;
            });
            return next;
        },

        /**
         * A rung did not hold: one under it, never under the base budget, and
         * regaining it will take longer. A stream runs at one rung to its end,
         * and may have earned the next one before it fell ill: what it ran at
         * is what failed.
         * @param {string} host
         * @param {string} codec
         * @param {number} [rung] the rung that failed; the stored one by default
         * @returns {number} the new ceiling, or 0 when already at the base budget
         */
        fail(host, codec, rung) {
            const key = keyOf(host, codec);
            const stored = snapToRung((read().hosts[key] || {}).ceiling);
            const was = rung > 0 ? snapToRung(rung) : stored;
            const below = rungBelow(was);
            if (!below) return 0;
            const now = nowMs();
            const ceiling = Math.min(stored, below);
            update(key, (e) => {
                const f = e.fails[was];
                e.fails[was] = { n: (f ? f.n : 0) + 1, at: now };
                e.ceiling = ceiling;
                e.trial = false;
            });
            return ceiling;
        },

        /**
         * The host crashed while a stream ran at this rung. The first crash is
         * only remembered; a second at the same rung, while the first still is,
         * counts against it, and the count starts over.
         * @param {string} host
         * @param {string} codec
         * @param {number} rung the rung the stream ran at
         * @returns {boolean} whether this crash costs the rung (then call fail)
         */
        hostCrash(host, codec, rung) {
            const was = snapToRung(rung);
            if (!rungBelow(was)) return false;
            const now = nowMs();
            let counts = false;
            update(keyOf(host, codec), (e) => {
                counts = e.crashes[was] > 0;
                if (counts) delete e.crashes[was];
                else e.crashes[was] = now;
            });
            return counts;
        },
    };
}
