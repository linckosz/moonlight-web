/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The seal covers the application until it lifts, which makes "does it always
 * lift" a question about whether the application is usable at all. It is driven
 * by a clock that stops in a hidden tab, so the case pinned here is the one
 * that already broke: a page that finishes loading while nobody is looking.
 */

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

let SecuringOverlay;

/* The fake clock has to cover performance.now() as well as the timers: the
   meter measures elapsed time with it, and a frozen reading would leave the bar
   pinned at the work term alone. */
let clock = 0;
function advance(ms, step = 50) {
    // In small steps, so the two clocks stay together: jumping the reading and
    // then running every timer in the window makes callbacks that should fire
    // at different times all see the same, final, elapsed value.
    for (let done = 0; done < ms; done += step) {
        clock += step;
        vi.advanceTimersByTime(step);
    }
}

beforeEach(async () => {
    document.body.innerHTML = '';
    sessionStorage.clear();
    clock = 0;
    vi.stubGlobal('performance', { now: () => clock });
    vi.useFakeTimers();
    vi.resetModules();
    ({ SecuringOverlay } = await import('../js/ui/SecuringOverlay.js'));
});

afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
});

/** A hidden tab: the browser accepts the request and never calls back. */
function hiddenTab() {
    vi.stubGlobal('requestAnimationFrame', () => 0);
}

describe('the securing seal', () => {
    it('never appears on a direct connection', () => {
        SecuringOverlay.show(false);
        expect(document.querySelector('.securing')).toBeNull();
    });

    it('appears when the page came down a tunnel', () => {
        hiddenTab();
        SecuringOverlay.show(true);
        expect(document.querySelector('.securing')).not.toBeNull();
    });

    it('is skipped right after the entry page has just shown its own', () => {
        sessionStorage.setItem('mw-seal', String(Date.now()));
        SecuringOverlay.show(true);
        expect(document.querySelector('.securing')).toBeNull();
        // Read once and consumed, so the next refresh shows it again.
        expect(sessionStorage.getItem('mw-seal')).toBeNull();
    });

    it('ignores a handover note left behind by an older session', () => {
        sessionStorage.setItem('mw-seal', String(Date.now() - 60_000));
        hiddenTab();
        SecuringOverlay.show(true);
        expect(document.querySelector('.securing')).not.toBeNull();
    });

    it('finishes and lifts in a tab that never gets a frame', () => {
        // THE regression: with the meter driven only by requestAnimationFrame,
        // the bar stayed at zero here and the seal never lifted — the
        // application was covered by a dead page until the tab was focused.
        hiddenTab();
        SecuringOverlay.show(true);
        SecuringOverlay.finish();

        advance(1300);
        const el = document.querySelector('.securing');
        expect(el.dataset.state).toBe('secured');
        expect(parseFloat(el.querySelector('.securing-bar').style.width)).toBeGreaterThan(99);

        // The floor, the hold, then the fade.
        advance(2000);
        expect(document.querySelector('.securing')).toBeNull();
    });

    it('cannot reach the end before the connection does', () => {
        // The time term is 0.3 of the bar; work owns the rest. Long after the
        // time term has saturated, an unfinished connection must still show an
        // unfinished bar — the seal reports, it does not perform.
        hiddenTab();
        SecuringOverlay.show(true);
        SecuringOverlay.stage('binding'); // 0.55 of the work

        advance(5000);
        const el = document.querySelector('.securing');
        expect(el).not.toBeNull();
        expect(el.dataset.state).not.toBe('secured');
        expect(parseFloat(el.querySelector('.securing-bar').style.width)).toBeLessThan(99);
    });

    it('gets out of the way when the connection fails', () => {
        hiddenTab();
        SecuringOverlay.show(true);
        SecuringOverlay.abort();
        advance(1000);
        expect(document.querySelector('.securing')).toBeNull();
    });
});
