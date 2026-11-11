/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, afterEach } from 'vitest';

import {
    CONFIRM_GAP_MS,
    PadNumberNotice,
    padBehindAnother,
    sessionHere,
} from '../js/stream/HostPadNumber.js';
import { GamepadManager } from '../js/stream/GamepadManager.js';
import { TouchPadState } from '../js/stream/TouchPadState.js';

/**
 * A Windows native host says which player its OS made each of our pads. A
 * Sunshine session on the same PC, only disconnected, keeps player 1: the page
 * says so and offers to close that session, after which the pads are plugged
 * in again and take player 1.
 */

const sunshineHere = {
    uuid: 'sun-1',
    name: 'DualRTX',
    isLocalHost: true,
    backendType: 'sunshine',
    pairState: 'paired',
    state: 'online',
    currentGameId: 42,
};

describe('a pad behind another', () => {
    it('is one numbered past its own controller', () => {
        expect(padBehindAnother({ index: 0, number: 1 })).toBe(true);
        expect(padBehindAnother({ index: 1, number: 3 })).toBe(true);
    });

    it('is not one in its own place, or one numbered before it', () => {
        expect(padBehindAnother({ index: 0, number: 0 })).toBe(false);
        expect(padBehindAnother({ index: 2, number: 1 })).toBe(false);
    });

    it('is not read from a number XInput does not have', () => {
        expect(padBehindAnother({ index: 0, number: 4 })).toBe(false);
        expect(padBehindAnother({ index: 0, number: '1' })).toBe(false);
        expect(padBehindAnother({ index: -1, number: 1 })).toBe(false);
        expect(padBehindAnother({})).toBe(false);
    });
});

describe('the session that may hold player 1', () => {
    it('is a paired host on this PC, not the native one, with an app running', () => {
        expect(sessionHere([sunshineHere])?.uuid).toBe('sun-1');
    });

    it('is none on another PC, idle, unpaired, or the native host itself', () => {
        expect(sessionHere([{ ...sunshineHere, isLocalHost: false }])).toBeNull();
        expect(sessionHere([{ ...sunshineHere, currentGameId: 0 }])).toBeNull();
        expect(sessionHere([{ ...sunshineHere, pairState: 'not_paired' }])).toBeNull();
        expect(sessionHere([{ ...sunshineHere, backendType: 'native' }])).toBeNull();
        expect(sessionHere(undefined)).toBeNull();
    });
});

describe('the notice', () => {
    function deps(hosts = [sunshineHere]) {
        const d = {
            clock: 1000,
            hosts: vi.fn(() => Promise.resolve({ hosts })),
            stopSession: vi.fn(() => Promise.resolve({})),
            replug: vi.fn(),
            warn: vi.fn(),
            error: vi.fn(),
            t: (key, vars) => (vars ? `${key}:${vars.number}` : key),
            now: () => d.clock,
        };
        return d;
    }

    /** The button of the latest toast. */
    const lastAction = (d) => d.warn.mock.calls[d.warn.mock.calls.length - 1][1].action;

    /** Both presses, the second one well after the question. */
    async function pressTwice(d) {
        await lastAction(d).onClick();
        d.clock += CONFIRM_GAP_MS + 100;
        await lastAction(d).onClick();
    }

    it('offers to close the Sunshine session here, then plugs the pads in again', async () => {
        const d = deps();
        await new PadNumberNotice(d).note({ index: 0, number: 1 });
        expect(d.warn).toHaveBeenCalledTimes(1);
        const [text, options] = d.warn.mock.calls[0];
        expect(text).toBe('stream.padNumberTaken:2');
        expect(options.sticky).toBe(true);
        expect(options.action.label).toBe('stream.padNumberClose');
        await pressTwice(d);
        expect(d.stopSession).toHaveBeenCalledWith('sun-1');
        expect(d.replug).toHaveBeenCalledTimes(1);
    });

    it('closes nothing at the first press, and asks instead', async () => {
        const d = deps();
        await new PadNumberNotice(d).note({ index: 0, number: 1 });
        await lastAction(d).onClick();
        expect(d.stopSession).not.toHaveBeenCalled();
        expect(d.warn).toHaveBeenCalledTimes(2);
        const [text, options] = d.warn.mock.calls[1];
        expect(text).toBe('stream.padNumberConfirm');
        expect(options.sticky).toBe(true);
        expect(options.action.label).toBe('stream.padNumberConfirmClose');
    });

    it('takes a second press too soon for a tap meant for the game, and asks again', async () => {
        const d = deps();
        await new PadNumberNotice(d).note({ index: 0, number: 1 });
        await lastAction(d).onClick();
        d.clock += CONFIRM_GAP_MS - 100;
        await lastAction(d).onClick();
        expect(d.stopSession).not.toHaveBeenCalled();
        expect(d.warn).toHaveBeenCalledTimes(3);
        expect(d.warn.mock.calls[2][0]).toBe('stream.padNumberConfirm');
        // The question asked again starts its own wait.
        d.clock += CONFIRM_GAP_MS - 100;
        await lastAction(d).onClick();
        expect(d.stopSession).not.toHaveBeenCalled();
        d.clock += CONFIRM_GAP_MS + 100;
        await lastAction(d).onClick();
        expect(d.stopSession).toHaveBeenCalledTimes(1);
    });

    it('leaves the session alone when the question is dismissed', async () => {
        const d = deps();
        await new PadNumberNotice(d).note({ index: 0, number: 1 });
        await lastAction(d).onClick();
        // A toast dismissed by a tap on its text runs no action.
        d.clock += 60000;
        expect(d.stopSession).not.toHaveBeenCalled();
        expect(d.replug).not.toHaveBeenCalled();
    });

    it('says what it can without a button when no session here holds it', async () => {
        const d = deps([]);
        await new PadNumberNotice(d).note({ index: 0, number: 1 });
        expect(d.warn).toHaveBeenCalledWith('stream.padNumberOther:2', { durationMs: 10000 });
    });

    it('says nothing of a pad in its place', async () => {
        const d = deps();
        await new PadNumberNotice(d).note({ index: 0, number: 0 });
        expect(d.hosts).not.toHaveBeenCalled();
        expect(d.warn).not.toHaveBeenCalled();
    });

    it('says it once a stream', async () => {
        const d = deps();
        const notice = new PadNumberNotice(d);
        await notice.note({ index: 0, number: 1 });
        await notice.note({ index: 0, number: 2 });
        expect(d.warn).toHaveBeenCalledTimes(1);
    });

    it('says so when the session could not be closed, and plugs nothing again', async () => {
        const d = deps();
        d.stopSession = vi.fn(() => Promise.reject(new Error('host gone')));
        await new PadNumberNotice(d).note({ index: 0, number: 1 });
        await pressTwice(d);
        expect(d.error).toHaveBeenCalledWith('stream.padNumberCloseFailed');
        expect(d.replug).not.toHaveBeenCalled();
    });

    it('still speaks when the host list does not answer', async () => {
        const d = deps();
        d.hosts = vi.fn(() => Promise.reject(new Error('offline')));
        await new PadNumberNotice(d).note({ index: 0, number: 1 });
        expect(d.warn).toHaveBeenCalledWith('stream.padNumberOther:2', { durationMs: 10000 });
    });
});

describe('plugging every pad in again', () => {
    afterEach(() => {
        vi.unstubAllGlobals();
    });

    it('takes the on-screen controller off the host and puts it back at once', () => {
        vi.stubGlobal('requestAnimationFrame', () => 1);
        vi.stubGlobal('cancelAnimationFrame', () => {});
        Object.defineProperty(navigator, 'getGamepads', {
            value: () => [],
            configurable: true,
            writable: true,
        });
        const send = vi.fn();
        const gm = new GamepadManager(send);
        gm.start();
        gm.attachVirtual(new TouchPadState());
        const types = () => send.mock.calls.map((c) => c[0].type).filter((x) => x !== 'gamepad');
        expect(types()).toEqual(['gamepadconnect']);
        gm.replugAll();
        expect(types()).toEqual(['gamepadconnect', 'gamepaddisconnect', 'gamepadconnect']);
        expect(gm.hasVirtual()).toBe(true);
        gm.stop();
    });
});
