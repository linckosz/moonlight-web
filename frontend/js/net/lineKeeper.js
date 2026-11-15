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
 * Keeps the page on its machine when the line to it drops.
 *
 * Through the rendezvous every request the application makes rides one data
 * channel, and that channel does not survive a phone putting the browser aside.
 * iOS suspends Chrome a couple of seconds after it leaves the screen — going to
 * the home screen is enough — and the connection is gone when it comes back.
 * Going back through the entry page then cost a whole start: the loading
 * screen, the handshake, the interface rebuilt from nothing, where the user
 * expected the page they had left.
 *
 * So the line is dialled again from here, under the page: same machine, same
 * pairing key, and the same cookie jar — the transport keeps it in storage, not
 * in the line — so the session the old line carried is the one the new line
 * carries. Requests made in the meantime wait for it instead of failing.
 *
 * The entry page stays the answer when no new line comes within
 * REDIAL_BUDGET_MS of the page being back in front, or after REDIAL_ATTEMPTS
 * failures. The entry page says what is happening and keeps trying, where this
 * page has nothing on screen to say it with.
 *
 * Under a stream it is not the answer. Going there ends the stream, and the
 * stream does not need the line: its own connection goes straight between the
 * two machines, and its signalling socket is closed on purpose once that
 * connection is up. So the line is dialled again behind the stream, for as
 * long as the stream lasts, and the page says so — `notice` — once it has been
 * away NOTICE_DELAY_MS: reconnecting, then, after the first budget, out of
 * reach, with one attempt every STREAM_REDIAL_EVERY_MS. Requests made while it
 * is out of reach fail at once rather than wait for it. A stream that ends with
 * the line still down goes to the entry page, as before.
 *
 * Nothing is dialled while the page is hidden. A suspended page cannot finish a
 * handshake, and one started in the seconds before the suspension would be found
 * half done on return, holding every request until its own deadline.
 */

/** How long the page may wait, back in front of the user, for a new line. */
export const REDIAL_BUDGET_MS = 10000;
/** Between two attempts: a network coming back with the page often needs a
 *  moment, and the introduction server limits connections per address. */
export const REDIAL_PAUSE_MS = 1000;
/** Attempts per return of the page. */
export const REDIAL_ATTEMPTS = 3;
/** Less time than this left in the budget is not worth another attempt. */
const MIN_ATTEMPT_MS = 2000;
/** Under a stream: how long the line may be away before the page says so. A
 *  line back sooner is a hiccup nobody needs to read about. */
export const NOTICE_DELAY_MS = 3000;
/** Under a stream, past the first budget: one attempt this often. Each counts
 *  against the introduction server's limit on attempts per address. */
export const STREAM_REDIAL_EVERY_MS = 10000;
/** The longest one of those attempts may take. */
const STREAM_ATTEMPT_MS = 8000;
/** How often a wait under a stream looks whether the stream is still on. */
const STREAM_CHECK_MS = 1000;
/** Hidden this long, the line is asked whether it still carries on return. */
export const LONG_HIDE_MS = 60000;
/** How long that question may take: requests wait for it, so it is short. */
export const RETURN_CHECK_MS = 1500;

/** Methods that can be asked again on a new line without doing anything twice. */
const SAFE_METHODS = new Set(['GET', 'HEAD']);

/** The page went behind something during an attempt: not a failure. */
const HIDDEN = Symbol('hidden');

/**
 * Put a line down without hearing about it. A transport from before
 * Tunnel.close() is put down by hand.
 * @param {any} line
 */
export function hangUp(line) {
    if (!line) return;
    if (typeof line.close === 'function') {
        line.close();
        return;
    }
    line.onclosed = null;
    for (const part of [line._dc, line._pc, line._ws]) {
        try {
            part?.close();
        } catch {
            /* already gone */
        }
    }
}

/** @param {Document} doc */
function whenVisible(doc) {
    return new Promise((resolve) => {
        if (doc.visibilityState === 'visible') {
            resolve(undefined);
            return;
        }
        const onChange = () => {
            if (doc.visibilityState !== 'visible') return;
            doc.removeEventListener('visibilitychange', onChange);
            resolve(undefined);
        };
        doc.addEventListener('visibilitychange', onChange);
    });
}

const sleep = (/** @type {number} */ ms) => new Promise((resolve) => setTimeout(resolve, ms));

export class LineKeeper {
    /**
     * @param {object} opts
     * @param {() => any} opts.dial          a new line to the same machine, not yet connected
     * @param {() => void} opts.giveUp       hand the page over to the entry page
     * @param {() => boolean} opts.streaming whether a stream is on screen
     * @param {(state: 'reconnecting' | 'out-of-reach' | null) => void} [opts.notice]
     *   what to say over a stream about the line; null takes it away
     * @param {Document} [opts.doc]
     */
    constructor({ dial, giveUp, streaming, notice = () => {}, doc = document }) {
        this._dial = dial;
        this._giveUp = giveUp;
        this._streaming = streaming;
        this._notice = notice;
        this._doc = doc;
        /** The line requests go out on. */
        this.line = null;
        /** @type {Promise<any> | null} the next line, while it is being dialled */
        this._redial = null;
        this._leaving = false;
        /** Under a stream, past the first budget: requests fail at once. */
        this._outOfReach = false;

        /** When the page last went behind something; null while in front. */
        this._hiddenAt = null;
        /** @type {Promise<void> | null} the question to the line on return */
        this._checking = null;

        // Back in front with a line that died without a word (its close still
        // queued behind the page's return): dial now, before a request finds out.
        doc.addEventListener('visibilitychange', () => {
            if (doc.visibilityState !== 'visible') {
                if (this._hiddenAt === null) this._hiddenAt = Date.now();
                return;
            }
            const hiddenFor = this._hiddenAt === null ? 0 : Date.now() - this._hiddenAt;
            this._hiddenAt = null;
            if (!this.line || this._redial) return;
            if (!this.line.connected) {
                this.lost(this.line, 'found closed on return');
                return;
            }
            // A line can still look open after a long absence and carry nothing:
            // iOS kept Safari's page for 35 minutes, its channel still "open",
            // and the launch pressed a second after the return went out on it
            // and failed four seconds later, when the transport gave up on it
            // (15/11/2026). Asked first, it is dialled again before the press.
            if (hiddenFor >= LONG_HIDE_MS && !this._checking)
                this._checking = this._checkOnReturn(this.line);
        });
    }

    /**
     * Whether a line that looks open after a long absence still carries: one
     * small request, RETURN_CHECK_MS at most. No answer in time, it is dialled
     * again. Requests wait for the question (ready()), so none goes out on a
     * line about to be found dead.
     * @param {any} line
     */
    async _checkOnReturn(line) {
        let timer = null;
        const late = new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error('no answer')), RETURN_CHECK_MS);
        });
        try {
            await Promise.race([line.fetch('/api/health'), late]);
        } catch (e) {
            if (line === this.line && !this._redial)
                this.lost(line, `silent after a long absence (${e.message})`);
        } finally {
            clearTimeout(timer);
            this._checking = null;
        }
    }

    /** The line that is up now, watched from here on. */
    adopt(line) {
        this.line = line;
        line.onclosed = (why) => this.lost(line, why);
    }

    /**
     * A line went away. Anything but the line in use, or a loss already being
     * dealt with, is old news.
     * @param {any} line
     * @param {string} [why]
     */
    lost(line, why) {
        if (line !== this.line || this._redial || this._leaving) return;
        const underStream = this._streaming();
        console.warn(
            `[MW] The line to the host dropped (${why || 'no reason'}) — dialling it again` +
                (underStream ? ' behind the stream' : ''),
        );
        // What is left of it (a socket the far end still counts) goes too.
        hangUp(line);
        this._redial = underStream ? this._dialUnderStream() : this._dialAgain();
    }

    /** The line to send on: the one in use, or the next one while it is dialled. */
    async ready() {
        // The line is being asked whether it still carries (back from a long
        // absence): wait for the answer, at most RETURN_CHECK_MS.
        if (this._checking) await this._checking;
        if (!this._redial && this.line && !this.line.connected)
            this.lost(this.line, 'found closed');
        // The stream this was waiting behind is over: the page has nothing to
        // wait for any more.
        if (this._outOfReach && !this._streaming()) this._leave();
        if (this._leaving) return new Promise(() => {});
        if (this._outOfReach) throw new Error('the connection to your machine is not open');
        return this._redial || this.line;
    }

    /**
     * One request, carried on the line. A read the line took down with it is
     * asked again on the next line. Anything else may have been carried out
     * before the line went, so its failure is reported as it is.
     * @param {string} url
     * @param {RequestInit} [init]
     */
    async fetch(url, init = {}) {
        const line = await this.ready();
        try {
            return await line.fetch(url, init);
        } catch (e) {
            if (!SAFE_METHODS.has((init.method || 'GET').toUpperCase())) throw e;
            if (line === this.line && !this._redial) {
                if (line.connected) throw e; // the line is fine; the request failed
                this.lost(line, e.message); // a close not reported yet
                if (!this._redial) throw e; // the page is leaving
            }
            return (await this.ready()).fetch(url, init);
        }
    }

    _leave() {
        if (this._leaving) return;
        this._leaving = true;
        this._giveUp();
    }

    async _dialAgain() {
        const started = Date.now();
        let attempts = 0;
        let deadline = 0;
        for (;;) {
            if (this._doc.visibilityState !== 'visible') {
                await whenVisible(this._doc);
                deadline = 0; // the budget counts from the page's return
            }
            if (!deadline) deadline = Date.now() + REDIAL_BUDGET_MS;
            attempts++;
            try {
                const line = await this._attempt(deadline - Date.now());
                this._redial = null;
                this.adopt(line);
                console.info(`[MW] The line to the host is back after ${Date.now() - started} ms`);
                return line;
            } catch (e) {
                if (e === HIDDEN) {
                    attempts--;
                    continue;
                }
                const left = deadline - Date.now();
                if (attempts >= REDIAL_ATTEMPTS || left < REDIAL_PAUSE_MS + MIN_ATTEMPT_MS) {
                    console.warn(
                        `[MW] The line to the host could not be dialled again (${e.message}) — ` +
                            'going back through the entry page',
                    );
                    this._leave();
                    return new Promise(() => {}); // the page is leaving
                }
                await sleep(REDIAL_PAUSE_MS);
            }
        }
    }

    /**
     * The line dialled again behind a stream, for as long as the stream lasts.
     * The first budget is spent as _dialAgain() spends it; then one attempt
     * every STREAM_REDIAL_EVERY_MS, with the page saying the machine is out of
     * reach. Hidden, nothing is dialled, as there.
     */
    async _dialUnderStream() {
        const started = Date.now();
        const noticeTimer = setTimeout(() => this._notice('reconnecting'), NOTICE_DELAY_MS);
        let budgetEnd = started + REDIAL_BUDGET_MS;
        while (this._streaming()) {
            if (this._doc.visibilityState !== 'visible') {
                await whenVisible(this._doc);
                // The budget counts from the page's return.
                if (!this._outOfReach) budgetEnd = Date.now() + REDIAL_BUDGET_MS;
                continue;
            }
            const attemptStarted = Date.now();
            try {
                const line = await this._attempt(
                    this._outOfReach ? STREAM_ATTEMPT_MS : budgetEnd - attemptStarted,
                );
                clearTimeout(noticeTimer);
                this._outOfReach = false;
                this._redial = null;
                this.adopt(line);
                this._notice(null);
                console.info(
                    `[MW] The line to the host is back after ${Date.now() - started} ms, ` +
                        'behind the stream',
                );
                return line;
            } catch (e) {
                if (e === HIDDEN) continue;
                if (
                    !this._outOfReach &&
                    budgetEnd - Date.now() < REDIAL_PAUSE_MS + MIN_ATTEMPT_MS
                ) {
                    this._outOfReach = true;
                    clearTimeout(noticeTimer);
                    this._notice('out-of-reach');
                    console.warn(
                        `[MW] The line to the host could not be dialled again (${e.message}) — ` +
                            `the stream carries on; trying every ${STREAM_REDIAL_EVERY_MS / 1000} s`,
                    );
                }
                await this._pauseUnderStream(
                    this._outOfReach
                        ? attemptStarted + STREAM_REDIAL_EVERY_MS - Date.now()
                        : REDIAL_PAUSE_MS,
                );
            }
        }
        clearTimeout(noticeTimer);
        this._notice(null);
        console.warn(
            '[MW] The stream ended with the line to the host still down — ' +
                'going back through the entry page',
        );
        this._leave();
        return new Promise(() => {}); // the page is leaving
    }

    /** Wait, but no longer than the stream lasts. @param {number} ms */
    async _pauseUnderStream(ms) {
        const until = Date.now() + ms;
        while (this._streaming() && Date.now() < until) {
            await sleep(Math.min(STREAM_CHECK_MS, until - Date.now()));
        }
    }

    /**
     * One dial. Abandoned, and the half-made line put down, when the page goes
     * behind something or the time runs out.
     * @param {number} ms
     */
    _attempt(ms) {
        const line = this._dial();
        return new Promise((resolve, reject) => {
            let settled = false;
            const settle = (/** @type {any} */ err) => {
                if (settled) return;
                settled = true;
                clearTimeout(timer);
                this._doc.removeEventListener('visibilitychange', onHide);
                if (err) {
                    hangUp(line);
                    reject(err);
                } else {
                    resolve(line);
                }
            };
            const onHide = () => {
                if (this._doc.visibilityState !== 'visible') settle(HIDDEN);
            };
            const timer = setTimeout(() => settle(new Error('no answer in time')), ms);
            this._doc.addEventListener('visibilitychange', onHide);
            line.connect().then(
                () => settle(null),
                (/** @type {any} */ e) => settle(e instanceof Error ? e : new Error(String(e))),
            );
        });
    }
}
