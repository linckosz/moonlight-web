/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
/* global localStorage, document */
import { describe, it, expect, afterEach } from 'vitest';
import { MWSR_SPLIT_VIEWS, mwsrSplitWanted, nextMwsrSplitView } from '../js/util/BrowserDetect.js';
import { StreamView, mwsrSplitLabel } from '../js/ui/StreamView.js';

/**
 * The eye test's split screen (31/10/2026): a lab key, off by default, only
 * with the zones themselves; its view goes round from the button or
 * Ctrl+Alt+Shift+G and stays for the next streams, and nothing on screen ever
 * says which side the grid is on.
 */
describe("mwsr's split screen", () => {
    afterEach(() => localStorage.clear());

    it('is off without its key, without the zones, or with a side it does not know', () => {
        expect(mwsrSplitWanted()).toBe(null);
        localStorage.setItem('mw_mwsr_split', 'left');
        expect(mwsrSplitWanted()).toBe(null);
        localStorage.setItem('mw_mwsr_map', '1');
        localStorage.setItem('mw_mwsr_split', 'middle');
        expect(mwsrSplitWanted()).toBe(null);
    });

    it('takes the side from its key, and the first view unless one was chosen', () => {
        localStorage.setItem('mw_mwsr_map', '1');
        localStorage.setItem('mw_mwsr_split', 'right');
        expect(mwsrSplitWanted()).toEqual({ gridLeft: false, view: 'centre-twice' });
        localStorage.setItem('mw_mwsr_split_view', 'halves');
        localStorage.setItem('mw_mwsr_split', 'left');
        expect(mwsrSplitWanted()).toEqual({ gridLeft: true, view: 'halves' });
        localStorage.setItem('mw_mwsr_split_view', 'bogus');
        expect(mwsrSplitWanted().view).toBe('centre-twice');
    });

    it('goes round the three views, and keeps the last', () => {
        expect(nextMwsrSplitView('centre-twice')).toBe('left-twice');
        expect(nextMwsrSplitView('left-twice')).toBe('halves');
        expect(nextMwsrSplitView('halves')).toBe('centre-twice');
        expect(localStorage.getItem('mw_mwsr_split_view')).toBe('centre-twice');
    });

    it('names the view on its button, never the side', () => {
        const labels = MWSR_SPLIT_VIEWS.map(mwsrSplitLabel);
        expect(labels).toEqual([
            'Split 1/3 · centre ×2',
            'Split 2/3 · left half ×2',
            'Split 3/3 · left | right',
        ]);
        for (const l of labels) expect(l).not.toMatch(/grid|law|loi|grille/i);
    });

    it('changes the view of the renderer and of the button, the side kept', () => {
        const btn = document.createElement('button');
        btn.id = 'btn-mwsr-split';
        const streamEl = document.createElement('div');
        streamEl.appendChild(btn);
        const set = [];
        const view = {
            _mwsrSplit: { gridLeft: true, view: 'centre-twice' },
            _renderer: { setMwsrSplit: (s) => set.push(s) },
            streamEl,
        };
        StreamView.prototype._cycleMwsrSplit.call(view);
        expect(set).toEqual([{ gridLeft: true, view: 'left-twice' }]);
        expect(btn.textContent).toBe('Split 2/3 · left half ×2');
        StreamView.prototype._cycleMwsrSplit.call({ _mwsrSplit: null });
    });
});
