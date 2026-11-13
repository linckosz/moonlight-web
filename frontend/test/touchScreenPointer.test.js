/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';

import { TouchScreenPointer } from '../js/stream/TouchScreenPointer.js';

/**
 * Issue #61: in Touch Screen Mode the finger is the pointer, so the host's is
 * hidden. A mouse or a trackpad moving brings it back, the next touch hides it.
 */

describe('the host pointer in Touch Screen Mode', () => {
    it('is hidden from the start', () => {
        const p = new TouchScreenPointer(true);
        expect(p.on()).toBe(true);
        expect(p.hidden()).toBe(true);
    });

    it('comes back while a mouse or a trackpad moves', () => {
        const p = new TouchScreenPointer(true);
        expect(p.moved('mouse')).toBe(true);
        expect(p.hidden()).toBe(false);
        // Still moving: nothing changes.
        expect(p.moved('mouse')).toBe(false);
        expect(p.hidden()).toBe(false);
    });

    it('goes with the first touch after that', () => {
        const p = new TouchScreenPointer(true);
        p.moved('mouse');
        expect(p.touched()).toBe(true);
        expect(p.hidden()).toBe(true);
        // A second touch has nothing more to hide.
        expect(p.touched()).toBe(false);
    });

    it('is not brought back by a finger or a pen', () => {
        const p = new TouchScreenPointer(true);
        expect(p.moved('touch')).toBe(false);
        expect(p.moved('pen')).toBe(false);
        expect(p.hidden()).toBe(true);
    });

    it('is never hidden outside Touch Screen Mode', () => {
        const p = new TouchScreenPointer(false);
        expect(p.on()).toBe(false);
        expect(p.hidden()).toBe(false);
        expect(p.moved('mouse')).toBe(false);
        expect(p.touched()).toBe(false);
        expect(p.hidden()).toBe(false);
    });
});
