/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { Toast } from '../js/ui/Toast.js';

describe('Toast', () => {
    beforeEach(() => {
        document.body.innerHTML = '';
        vi.useFakeTimers();
    });
    afterEach(() => {
        vi.useRealTimers();
    });

    it('lazily creates the container and appends a typed toast', () => {
        Toast.success('done');
        const container = document.getElementById('toast-container');
        expect(container).not.toBeNull();
        const toast = container.querySelector('.toast');
        expect(toast.classList.contains('toast-success')).toBe(true);
        expect(toast.textContent).toBe('done');
    });

    it('reuses the existing container across toasts', () => {
        Toast.info('a');
        Toast.error('b');
        expect(document.querySelectorAll('#toast-container').length).toBe(1);
        expect(document.querySelectorAll('.toast').length).toBe(2);
    });

    it('exposes success/error/warning/info shorthands', () => {
        Toast.warning('w');
        expect(document.querySelector('.toast-warning')).not.toBeNull();
    });

    it('auto-removes a toast after its lifetime', () => {
        Toast.info('temp');
        expect(document.querySelectorAll('.toast').length).toBe(1);
        vi.advanceTimersByTime(4000); // auto-fade
        vi.advanceTimersByTime(300); // exit animation
        expect(document.querySelectorAll('.toast').length).toBe(0);
    });

    it('removes a toast on click', () => {
        Toast.info('clickme');
        const toast = document.querySelector('.toast');
        toast.dispatchEvent(new window.MouseEvent('click'));
        vi.advanceTimersByTime(300);
        expect(document.querySelectorAll('.toast').length).toBe(0);
    });

    it('enforces the visible limit, evicting the oldest', () => {
        // Desktop: max 5 (matchMedia unstubbed → falsy → desktop branch).
        for (let i = 0; i < 7; i++) Toast.info('t' + i);
        const visible = document.querySelectorAll('.toast:not(.toast-exit)').length;
        expect(visible).toBeLessThanOrEqual(5);
    });

    it('uses the mobile cap when the media query matches', () => {
        vi.stubGlobal('matchMedia', () => ({ matches: true }));
        for (let i = 0; i < 7; i++) Toast.info('m' + i);
        const visible = document.querySelectorAll('.toast:not(.toast-exit)').length;
        expect(visible).toBeLessThanOrEqual(3);
        vi.unstubAllGlobals();
    });

    it('dismissAll resolves and clears every toast', async () => {
        Toast.info('x');
        Toast.info('y');
        const p = Toast.dismissAll();
        vi.advanceTimersByTime(500);
        await p;
        vi.advanceTimersByTime(300);
        expect(document.querySelectorAll('.toast').length).toBe(0);
    });

    it('dismissAll resolves immediately with no container', async () => {
        const p = Toast.dismissAll();
        vi.advanceTimersByTime(1);
        await expect(p).resolves.toBeUndefined();
    });
});
