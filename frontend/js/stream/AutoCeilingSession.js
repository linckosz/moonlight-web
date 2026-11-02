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
 * One launch's dealings with the learned ceiling: read once when the viewer
 * launches, frozen until the stream ends, relaunches included. A rung earned
 * serves the next launch: resizing a virtual display mid-stream can kill a game.
 *
 * Everything it decides goes to the console (`[MW] Auto ceiling: …`): the trap
 * of the enhancer's memory was descents nobody could read.
 */

import {
    HOLD_BASE_MS,
    autoCeilingMemory,
    autoLearnMode,
    deviceMaxCeiling,
    rungAbove,
    rungBelow,
} from '../util/AutoCeiling.js';
import { PIXEL_RATE_BUDGET } from '../util/StreamResolution.js';
import { AutoCeilingMeter } from './AutoCeilingMeter.js';

/** How far over its rung a stream may run and still say something of it: a
 *  half the viewer named can go past the budget, and then the rung is not what
 *  was tested. */
const RUNG_MARGIN = 1.02;

/** Pixels a second, as the journal says them. */
const millions = (pixels) => Math.round(pixels / 1e6) + ' M';

export class AutoCeilingSession {
    /**
     * @param {object} deps
     * @param {string} deps.host the host's uuid
     * @param {string} deps.codec the codec launched
     * @param {boolean} deps.native the native host: no other can go past 1080p
     * @param {{tv?:boolean, handheld?:boolean, lowMemory?:boolean, powerSave?:boolean}} deps.device
     * @param {'off'|'log'|'on'} [deps.mode] autoLearnMode() by default
     * @param {ReturnType<typeof autoCeilingMemory>} [deps.memory]
     * @param {(line: string) => void} [deps.log]
     */
    constructor({ host, codec, native, device, mode, memory, log }) {
        this._host = host;
        this._codec = codec;
        this._mode = mode || autoLearnMode();
        this._deviceMax = deviceMaxCeiling(device);
        this._log = log || ((line) => console.log(line));
        this._memory = memory || autoCeilingMemory();
        /** @type {AutoCeilingMeter|null} */
        this._meter = null;
        this._open = false;
        this._ended = false;
        this._limited = false;
        this._provesRung = false;
        this._asked = null;
        /** Whether this launch measures at all. */
        this.learns = !!native && this._mode !== 'off' && this._deviceMax > PIXEL_RATE_BUDGET;
        /** Whether what it measures is written, and read back at the next launch. */
        this.active = this.learns && this._mode === 'on';
        /** Why this session's stream failed, '' while it has not. */
        this.sick = '';
        /** The rung this launch runs at, in pixels a second. */
        this.rung = PIXEL_RATE_BUDGET;
        this.trial = false;
        this._holdMs = HOLD_BASE_MS;
        if (this.active) this._readRung();
    }

    /** The pixels a second Auto may spend on this launch. */
    get budget() {
        return this.rung;
    }

    _readRung() {
        let e = this._memory.entry(this._host, this._codec, this._deviceMax);
        if (e.crashed) {
            this._memory.close(this._host, this._codec);
            const fell = this._memory.fail(this._host, this._codec, e.ceiling);
            this._log(
                '[MW] Auto ceiling: the last stream at ' +
                    millions(e.ceiling) +
                    ' never came back' +
                    (fell ? ' — down to ' + millions(fell) : ''),
            );
            e = this._memory.entry(this._host, this._codec, this._deviceMax);
        }
        this.rung = e.ceiling;
        this.trial = e.trial;
        this._holdMs = e.holdMs;
    }

    /**
     * Weigh this launch against its rung and the next one.
     * @param {(budget: number) => {width:number, height:number, fps:number,
     *          pixelRate:number}} askAt what the launch would come to under a budget
     */
    weigh(askAt) {
        if (!this.learns) return;
        const now = askAt(this.rung);
        const next = rungAbove(this.rung);
        const reachable = next > 0 && next <= this._deviceMax;
        const below = rungBelow(this.rung);
        this._limited = reachable && askAt(next).pixelRate > now.pixelRate;
        this._provesRung = now.pixelRate > below && now.pixelRate <= this.rung * RUNG_MARGIN;
        this._asked = { width: now.width, height: now.height, fps: now.fps, below };
        this._log(
            '[MW] Auto ceiling: ' +
                millions(this.rung) +
                ' on ' +
                this._codec +
                (this.trial ? ', on trial' : '') +
                ', asking ' +
                millions(now.pixelRate) +
                (this._limited ? ', held back by it' : '') +
                (this._provesRung ? '' : ', proves nothing of it') +
                (this.active ? '' : ' (journal only)'),
        );
        if (this.active) this._memory.noteLast(this._host, this._codec);
    }

    /**
     * A meter for the view on screen. The stream is up: from here, if it is
     * never closed, it crashed. A launch the host refused never gets this far.
     * @param {number} now the view's clock
     * @returns {AutoCeilingMeter|null}
     */
    meter(now) {
        if (!this.learns || this.sick || !this._asked) {
            this._meter = null;
            return null;
        }
        if (this.active) {
            if (this._provesRung && this._asked.below > 0) this._markOpen();
            else this._markClosed();
        }
        this._meter = new AutoCeilingMeter({
            startedAt: now,
            fps: this._asked.fps,
            limited: this._limited,
            provesRung: this._provesRung,
            holdMs: this._holdMs,
            confirmMs: HOLD_BASE_MS,
            asked: this._asked,
            onEvent: (e) => this._onEvent(e),
        });
        return this._meter;
    }

    _onEvent(e) {
        const held = Math.round(e.loadedMs / 1000) + ' s loaded';
        const only = this.active ? '' : ' (journal only)';
        if (e.type === 'held') {
            if (this.active) this._memory.hold(this._host, this._codec, this.rung);
            this.trial = false;
            this._log('[MW] Auto ceiling: ' + millions(this.rung) + ' held, ' + held + only);
        } else if (e.type === 'earned') {
            const next = this.active
                ? this._memory.raise(this._host, this._codec, this._deviceMax, this.rung)
                : rungAbove(this.rung);
            if (next)
                this._log(
                    '[MW] Auto ceiling: ' +
                        millions(next) +
                        ' earned for the next launch, ' +
                        held +
                        only,
                );
        } else {
            this.sick = e.reason || 'unknown';
            const fell =
                this.active && e.atRung ? this._memory.fail(this._host, this._codec, this.rung) : 0;
            if (this.active) this._markClosed();
            this._log(
                '[MW] Auto ceiling: sick (' +
                    this.sick +
                    '), ' +
                    held +
                    ', ' +
                    (e.atRung ? 'at ' + millions(this.rung) : 'not at its rung') +
                    (fell ? ' — down to ' + millions(fell) : '') +
                    only,
            );
        }
    }

    _markOpen() {
        this._open = true;
        this._memory.open(this._host, this._codec);
    }

    _markClosed() {
        if (!this._open) return;
        this._open = false;
        this._memory.close(this._host, this._codec);
    }

    /** The page is going away or out of sight: a tab killed from there is no crash. */
    pause() {
        if (this._open) this._memory.close(this._host, this._codec);
    }

    /** The page is back, and the stream with it. */
    resume() {
        if (this._open && !this._ended) this._memory.open(this._host, this._codec);
    }

    /** The stream ended on its own terms. */
    end() {
        if (this._ended) return;
        this._ended = true;
        this._markClosed();
        if (this.learns && this._meter)
            this._log(
                '[MW] Auto ceiling: stream over, ' +
                    Math.round(this._meter.loadedMs / 1000) +
                    ' s loaded at ' +
                    millions(this.rung) +
                    (this.sick ? ', sick (' + this.sick + ')' : ''),
            );
    }

    /** A codec fallback carries the same stream on. */
    reopen() {
        this._ended = false;
    }
}
