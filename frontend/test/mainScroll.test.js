/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The hosts page comes back from Settings or Admin where it was. All three
 * render into one scroller (#main-content, or the document on a TV) that stays
 * put while its content is swapped, so the hosts page came back scrolled as far
 * as Settings had been, although it was never scrolled itself (12/11/2026).
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

// The overlays' own pages are not what this is about.
vi.mock('../js/ui/SettingsView.js', () => ({
    SettingsView: class {
        start() {}
        destroy() {}
    },
}));
vi.mock('../js/ui/AdminView.js', () => ({
    AdminView: class {
        start() {}
        destroy() {}
    },
}));

import { MoonlightApp } from '../js/app.js';
import { mainScroller, restoreScroll, SCROLL_HOLD_MS } from '../js/util/mainScroll.js';

describe('mainScroller', () => {
    afterEach(() => document.documentElement.classList.remove('remote-nav'));

    it('is #main-content, or the document on a TV', () => {
        const main = document.createElement('div');
        expect(mainScroller(main)).toBe(main);
        document.documentElement.classList.add('remote-nav');
        expect(mainScroller(main)).toBe(document.scrollingElement || document.documentElement);
    });
});

describe('restoreScroll', () => {
    afterEach(() => vi.unstubAllGlobals());

    it('puts the scroller back now, and again once the view is laid out', () => {
        const frames = [];
        vi.stubGlobal('requestAnimationFrame', (cb) => frames.push(cb));
        const el = document.createElement('div');
        restoreScroll(el, 240);
        expect(el.scrollTop).toBe(240);
        el.scrollTop = 80; // the first attempt, clamped to a page still filling in
        frames.forEach((cb) => cb());
        expect(el.scrollTop).toBe(240);
    });

    it('needs no second attempt for the top', () => {
        const frames = [];
        vi.stubGlobal('requestAnimationFrame', (cb) => frames.push(cb));
        const el = document.createElement('div');
        el.scrollTop = 500;
        restoreScroll(el, 0);
        expect(el.scrollTop).toBe(0);
        expect(frames).toHaveLength(0);
        expect(el.style.getPropertyValue('overflow-anchor')).toBe('');
    });

    // A host card above gets its app row a few milliseconds after the restore;
    // anchoring would move the view by that row's height (142 px on DualRTX,
    // 12/11/2026), away from where the user had left it.
    it("keeps the browser's scroll anchoring off until the user scrolls", () => {
        vi.stubGlobal('requestAnimationFrame', () => {});
        const el = document.createElement('div');
        restoreScroll(el, 240);
        expect(el.style.getPropertyValue('overflow-anchor')).toBe('none');
        document.dispatchEvent(new Event('mousemove')); // not a scroll
        expect(el.style.getPropertyValue('overflow-anchor')).toBe('none');
        document.dispatchEvent(new Event('wheel'));
        expect(el.style.getPropertyValue('overflow-anchor')).toBe('');
    });

    it('gives it back after 2 s at most, as the page had set it', () => {
        vi.useFakeTimers();
        try {
            vi.stubGlobal('requestAnimationFrame', () => {});
            const el = document.createElement('div');
            el.style.setProperty('overflow-anchor', 'auto');
            restoreScroll(el, 240);
            expect(el.style.getPropertyValue('overflow-anchor')).toBe('none');
            vi.advanceTimersByTime(SCROLL_HOLD_MS - 1);
            expect(el.style.getPropertyValue('overflow-anchor')).toBe('none');
            vi.advanceTimersByTime(1);
            expect(el.style.getPropertyValue('overflow-anchor')).toBe('auto');
            document.dispatchEvent(new Event('keydown')); // released already: no effect
            expect(el.style.getPropertyValue('overflow-anchor')).toBe('auto');
        } finally {
            vi.useRealTimers();
        }
    });

    it('ends the hold when the scroller goes back to the top', () => {
        vi.stubGlobal('requestAnimationFrame', () => {});
        const el = document.createElement('div');
        restoreScroll(el, 240);
        restoreScroll(el, 300); // a second restore holds once, from the page's own value
        restoreScroll(el, 0);
        expect(el.style.getPropertyValue('overflow-anchor')).toBe('');
    });

    it('does nothing without a scroller', () => {
        expect(() => restoreScroll(null, 10)).not.toThrow();
    });
});

describe('the hosts page around Settings and Admin', () => {
    let saved;
    let main;

    beforeEach(() => {
        saved = {
            state: MoonlightApp.state,
            streamView: MoonlightApp.streamView,
            hostListView: MoonlightApp.hostListView,
            nav: { ...MoonlightApp._nav },
        };
        MoonlightApp.state = 'hosts';
        MoonlightApp.streamView = null;
        MoonlightApp.hostListView = null;
        MoonlightApp._nav.overlay = null;
        MoonlightApp._nav.mainScrollTop = null;
        document.body.innerHTML = '<div id="main-content"></div>';
        main = document.getElementById('main-content');
        vi.spyOn(MoonlightApp, '_renderHosts').mockImplementation((el) => {
            el.innerHTML = '<div class="host-list">hosts</div>';
        });
        vi.spyOn(MoonlightApp, '_canReachAdmin').mockReturnValue(true);
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.stubGlobal('requestAnimationFrame', (cb) => cb());
        history.replaceState({ view: 'hosts' }, '', '/');
    });

    afterEach(() => {
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
        MoonlightApp.state = saved.state;
        MoonlightApp.streamView = saved.streamView;
        MoonlightApp.hostListView = saved.hostListView;
        Object.assign(MoonlightApp._nav, saved.nav);
        document.body.innerHTML = '';
    });

    it('comes back at the top from a Settings page scrolled down', () => {
        MoonlightApp._openOverlay('settings');
        main.scrollTop = 1800; // the user reads Settings to the bottom
        MoonlightApp._setMainView('hosts', {});
        expect(main.scrollTop).toBe(0);
    });

    it('comes back where it was scrolled to, through Settings then Admin', () => {
        MoonlightApp._setMainView('hosts', {});
        main.scrollTop = 320;
        MoonlightApp._openOverlay('settings');
        expect(main.scrollTop).toBe(0); // Settings opens at its top
        main.scrollTop = 900;
        MoonlightApp._openOverlay('admin'); // switched to from Settings
        expect(main.scrollTop).toBe(0);
        main.scrollTop = 1500;
        MoonlightApp._setMainView('hosts', {});
        expect(main.scrollTop).toBe(320);
    });

    it('leaves the scroll alone when no overlay covered the page', () => {
        MoonlightApp._setMainView('hosts', {});
        main.scrollTop = 140;
        MoonlightApp._setMainView('hosts', {});
        expect(main.scrollTop).toBe(140);
    });

    it('does the same with the document as the scroller on a TV', () => {
        document.documentElement.classList.add('remote-nav');
        try {
            const page = document.scrollingElement || document.documentElement;
            page.scrollTop = 0;
            MoonlightApp._openOverlay('settings');
            page.scrollTop = 1200;
            MoonlightApp._setMainView('hosts', {});
            expect(page.scrollTop).toBe(0);
        } finally {
            document.documentElement.classList.remove('remote-nav');
        }
    });
});
