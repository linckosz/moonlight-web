/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';
import { armAudioPlayRetry } from '../js/util/audioAutoplay.js';

/**
 * Minimal stand-in for an <audio> element: only `paused` and `play()` matter.
 * `play()` resolves or rejects on demand so a test can model a browser that
 * refuses the first replay and accepts the second.
 */
function fakeAudio({ playResults = [] } = {}) {
    const el = {
        paused: true,
        play: vi.fn(() => {
            const outcome = playResults.length ? playResults.shift() : 'ok';
            if (outcome === 'ok') {
                el.paused = false;
                return Promise.resolve();
            }
            return Promise.reject(new DOMException('blocked', 'NotAllowedError'));
        }),
    };
    return el;
}

/** Fire a gesture the way a real one reaches the capture listeners on window. */
function gesture(type = 'pointerdown') {
    window.dispatchEvent(new Event(type, { bubbles: true }));
}

/** Let the play() promise chain settle. */
const settle = () => new Promise((r) => setTimeout(r, 0));

describe('armAudioPlayRetry', () => {
    beforeEach(() => {
        vi.restoreAllMocks();
    });

    it('replays play() on the next user gesture', async () => {
        const el = fakeAudio();
        armAudioPlayRetry(el);

        expect(el.play).not.toHaveBeenCalled();
        gesture();
        expect(el.play).toHaveBeenCalledTimes(1);
    });

    it('accepts any of the gesture families', async () => {
        for (const type of ['pointerdown', 'mousedown', 'keydown', 'touchstart', 'click']) {
            const el = fakeAudio();
            armAudioPlayRetry(el);
            gesture(type);
            expect(el.play, `${type} should replay play()`).toHaveBeenCalledTimes(1);
            await settle();
        }
    });

    it('stays armed while the element is still paused, then disarms once it plays', async () => {
        // First replay refused, second accepted.
        const el = fakeAudio({ playResults: ['reject', 'ok'] });
        armAudioPlayRetry(el);

        gesture();
        await settle();
        expect(el.play).toHaveBeenCalledTimes(1);
        expect(el.paused).toBe(true);

        gesture();
        await settle();
        expect(el.play).toHaveBeenCalledTimes(2);
        expect(el.paused).toBe(false);

        // Disarmed: further gestures must not touch the element again.
        gesture();
        await settle();
        expect(el.play).toHaveBeenCalledTimes(2);
    });

    it('does not stack listeners when the same element is armed twice', async () => {
        const el = fakeAudio();
        const cleanup1 = armAudioPlayRetry(el);
        const cleanup2 = armAudioPlayRetry(el);

        expect(cleanup2).toBe(cleanup1);
        gesture();
        expect(el.play).toHaveBeenCalledTimes(1);
    });

    it('re-arms after the element has been released', async () => {
        const el = fakeAudio({ playResults: ['ok'] });
        armAudioPlayRetry(el);
        gesture();
        await settle();
        expect(el.play).toHaveBeenCalledTimes(1);

        // A later ontrack (renegotiation) can arm the same element again.
        el.paused = true;
        armAudioPlayRetry(el);
        gesture();
        await settle();
        expect(el.play).toHaveBeenCalledTimes(2);
    });

    it('removes its listeners when the returned cleanup runs', async () => {
        const el = fakeAudio();
        const cleanup = armAudioPlayRetry(el);

        cleanup();
        gesture();
        await settle();
        expect(el.play).not.toHaveBeenCalled();
    });

    it('skips a play() it does not need — element already running', async () => {
        const el = fakeAudio();
        armAudioPlayRetry(el);
        el.paused = false; // something else started it in the meantime

        gesture();
        await settle();
        expect(el.play).not.toHaveBeenCalled();

        // And it disarmed itself.
        el.paused = true;
        gesture();
        await settle();
        expect(el.play).not.toHaveBeenCalled();
    });

    it('tolerates a null element', () => {
        expect(() => armAudioPlayRetry(null)()).not.toThrow();
    });
});
