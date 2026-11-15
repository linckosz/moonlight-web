/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The PIN prompt of a booth whose streams have PINs (booth_stream_pin, issue
 * #68), and the words of its refusals and of a take-over. Pinned down: the
 * prompt names whose stream and what it streams; nothing goes before six
 * digits; Join hands the digits over and says what the host refused; a prompt
 * held back stays shut, then opens again; "Start … instead" closes it first;
 * a take-over notice reads as its reason, by its machine, and as the plain
 * notice when it says nothing more.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

// No locale is loaded in the TNR: render the key so the assertions can name it.
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, params) => (params ? key + ' ' + JSON.stringify(params) : key),
}));

import { showBoothPinDialog } from '../js/ui/BoothPinDialog.js';
import { boothErrorText, boothTakeoverBody } from '../js/ui/BoothText.js';

/** A BackendError as BackendClient throws it. */
function refusal(status, body) {
    const e = new Error(body.error);
    e.statusCode = status;
    e.responseBody = body;
    return e;
}

const flush = () => new Promise((r) => setTimeout(r, 0));

describe('what a refused PIN says', () => {
    it('names the tries left, the wait, and the other refusals', () => {
        expect(boothErrorText(refusal(401, { error: 'booth_pin_invalid', remaining: 2 }))).toEqual({
            text: 'booth.wrongPin {"n":2}',
            lockoutSeconds: 0,
        });
        expect(
            boothErrorText(refusal(401, { error: 'booth_pin_invalid', remaining: 1 })).text,
        ).toBe('booth.wrongPinOne');
        // The try that shut the prompt says the wait, not the tries.
        expect(
            boothErrorText(
                refusal(401, { error: 'booth_pin_invalid', remaining: 2, lockout_seconds: 30 }),
            ),
        ).toEqual({ text: 'booth.tooManyTries {"s":30}', lockoutSeconds: 30 });
        expect(
            boothErrorText(refusal(429, { error: 'rate_limited', lockout_seconds: 120 })),
        ).toEqual({ text: 'booth.tooManyTries {"s":120}', lockoutSeconds: 120 });
        expect(boothErrorText(refusal(503, { error: 'room_full' })).text).toBe('booth.roomFull');
        expect(boothErrorText(refusal(409, { error: 'no_stream' })).text).toBe('booth.streamEnded');
        for (const code of ['pairing_key_required', 'owner_proof_required', 'owner_proof_refused'])
            expect(boothErrorText(refusal(403, { error: code })).text).toBe('booth.needsPairing');
        expect(boothErrorText(refusal(409, { error: 'booth_owner_only' })).text).toBe(
            'booth.ownerOnly',
        );
        expect(boothErrorText(new Error('offline')).text).toBe('offline');
        expect(boothErrorText(null).text).toBe('launch.failed');
    });
});

describe('why a stream was taken over', () => {
    it('says with the PIN, or after the idle time, and by which machine', () => {
        expect(boothTakeoverBody({ type: 'takeover', reason: 'pin', by: 'LAPTOP-SAM' })).toBe(
            'booth.takenOverPin {"name":"LAPTOP-SAM"}',
        );
        expect(
            boothTakeoverBody({
                type: 'takeover',
                reason: 'idle',
                by: 'LAPTOP-SAM',
                idle_minutes: 15,
            }),
        ).toBe('booth.takenOverIdle {"name":"LAPTOP-SAM","minutes":15}');
        // No machine named: somebody.
        expect(boothTakeoverBody({ type: 'takeover', reason: 'pin' })).toBe(
            'booth.takenOverPin {"name":"booth.someone"}',
        );
        expect(boothTakeoverBody({ type: 'takeover', reason: 'pin', by: 42 })).toBe(
            'booth.takenOverPin {"name":"booth.someone"}',
        );
        // A name past what a notice carries is cut.
        const long = boothTakeoverBody({ reason: 'pin', by: 'x'.repeat(200) });
        expect(long).toBe('booth.takenOverPin ' + JSON.stringify({ name: 'x'.repeat(64) }));
    });

    it('is the plain take-over for a notice that says nothing more', () => {
        for (const notice of [
            undefined,
            null,
            { type: 'takeover' },
            { type: 'takeover', reason: 'admin', by: 'X' },
            { type: 'takeover', reason: 'idle', by: 'X' },
            { type: 'takeover', reason: 'idle', by: 'X', idle_minutes: '15' },
            { type: 'takeover', reason: 'idle', by: 'X', idle_minutes: 0 },
        ])
            expect(boothTakeoverBody(notice)).toBe('stream.takenOverBody');
    });
});

describe('the PIN prompt', () => {
    /** @type {{el: HTMLElement, close: () => void} | null} */
    let prompt = null;
    let onJoin;
    let onStartInstead;
    let onCancel;

    const open = (extra = {}) => {
        prompt = showBoothPinDialog({
            hostName: 'Gaming PC',
            ownerName: 'LAPTOP-SAM',
            theirApp: 'Elden Ring',
            myApp: 'Hades',
            onJoin,
            onStartInstead,
            onCancel,
            ...extra,
        });
        return prompt.el;
    };
    const q = (sel) => /** @type {HTMLElement} */ (document.querySelector(sel));
    const type = (value) => {
        const input = /** @type {HTMLInputElement} */ (q('.booth-prompt .player-pin-input'));
        input.value = value;
        input.dispatchEvent(new Event('input'));
        return input;
    };

    beforeEach(() => {
        document.body.innerHTML = '';
        onJoin = vi.fn().mockResolvedValue(undefined);
        onStartInstead = vi.fn();
        onCancel = vi.fn();
    });
    afterEach(() => {
        prompt?.close();
        prompt = null;
        vi.useRealTimers();
    });

    it('names the host, whose stream it is and what it streams', () => {
        open();
        expect(q('.booth-prompt h3').textContent).toBe('booth.inUseTitle {"host":"Gaming PC"}');
        const body = q('.booth-prompt .pairing-instruction');
        expect(body.querySelector('b').textContent).toBe('Elden Ring');
        expect(body.textContent).toContain('LAPTOP-SAM');
        expect(body.textContent).toContain('booth.inUseHint');
        expect(q('.booth-join').textContent).toBe('booth.join');
        expect(q('.booth-start-instead').textContent).toBe('booth.startInstead {"app":"Hades"}');
    });

    it('draws names as text, never as markup', () => {
        open({ ownerName: '<img src=x onerror=alert(1)>', theirApp: '<b>x</b>' });
        expect(document.querySelector('.booth-prompt img')).toBe(null);
        expect(q('.booth-prompt .pairing-instruction').textContent).toContain('<img');
    });

    it('takes six digits before anything, shown in two groups', () => {
        open();
        const join = /** @type {HTMLButtonElement} */ (q('.booth-join'));
        const start = /** @type {HTMLButtonElement} */ (q('.booth-start-instead'));
        expect(join.disabled).toBe(true);
        expect(start.disabled).toBe(true);
        const input = type('48a19');
        expect(input.value).toBe('481 9');
        expect(join.disabled).toBe(true);
        type('4819020');
        expect(input.value).toBe('481 902');
        expect(join.disabled).toBe(false);
        expect(start.disabled).toBe(false);
    });

    it('joins with the digits, and closes once joined', async () => {
        open();
        type('481902');
        q('.booth-join').click();
        await flush();
        expect(onJoin).toHaveBeenCalledWith('481902');
        expect(document.querySelector('.booth-prompt-overlay')).toBe(null);
        expect(onStartInstead).not.toHaveBeenCalled();
    });

    it('says what the host refused, and lets the PIN be typed again', async () => {
        onJoin.mockRejectedValueOnce(refusal(401, { error: 'booth_pin_invalid', remaining: 2 }));
        open();
        const input = type('481902');
        q('.booth-join').click();
        await flush();
        const msg = q('.booth-msg');
        expect(msg.textContent).toBe('booth.wrongPin {"n":2}');
        expect(msg.classList.contains('err')).toBe(true);
        expect(document.querySelector('.booth-prompt-overlay')).not.toBe(null);
        expect(input.disabled).toBe(false);
        // Typing again clears it.
        type('48190');
        expect(msg.textContent).toBe('');

        onJoin.mockRejectedValueOnce(refusal(503, { error: 'room_full' }));
        type('481903');
        q('.booth-join').click();
        await flush();
        expect(msg.textContent).toBe('booth.roomFull');
    });

    it('stays shut while held back, then opens again', async () => {
        vi.useFakeTimers();
        onJoin.mockRejectedValueOnce(refusal(429, { error: 'rate_limited', lockout_seconds: 3 }));
        open();
        const input = type('481902');
        q('.booth-join').click();
        await vi.advanceTimersByTimeAsync(0);
        const join = /** @type {HTMLButtonElement} */ (q('.booth-join'));
        expect(q('.booth-msg').textContent).toBe('booth.tooManyTries {"s":3}');
        expect(input.disabled).toBe(true);
        expect(join.disabled).toBe(true);
        await vi.advanceTimersByTimeAsync(1000);
        expect(q('.booth-msg').textContent).toBe('booth.tooManyTries {"s":2}');
        // Clicked while shut: nothing goes.
        join.disabled = false;
        join.click();
        expect(onJoin).toHaveBeenCalledTimes(1);
        await vi.advanceTimersByTimeAsync(2500);
        expect(q('.booth-msg').textContent).toBe('');
        expect(input.disabled).toBe(false);
        expect(/** @type {HTMLButtonElement} */ (q('.booth-join')).disabled).toBe(false);
    });

    it('starts this app instead from the click, the prompt closed first', () => {
        open();
        type('481902');
        onStartInstead.mockImplementation(() => {
            expect(document.querySelector('.booth-prompt-overlay')).toBe(null);
        });
        q('.booth-start-instead').click();
        expect(onStartInstead).toHaveBeenCalledWith('481902');
        expect(onJoin).not.toHaveBeenCalled();
    });

    it('shows a refusal it was opened with, held back as it says', () => {
        vi.useFakeTimers();
        open({ initial: { text: 'booth.tooManyTries {"s":30}', lockoutSeconds: 30 } });
        expect(q('.booth-msg').textContent).toBe('booth.tooManyTries {"s":30}');
        type('481902');
        expect(/** @type {HTMLButtonElement} */ (q('.booth-start-instead')).disabled).toBe(true);
    });

    it('goes on Cancel, on Escape and on the backdrop, and says so', () => {
        open();
        q('.booth-cancel').click();
        expect(onCancel).toHaveBeenCalledTimes(1);
        expect(document.querySelector('.booth-prompt-overlay')).toBe(null);
        open();
        document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
        expect(onCancel).toHaveBeenCalledTimes(2);
        const el = open();
        el.dispatchEvent(new MouseEvent('click', { bubbles: true }));
        expect(onCancel).toHaveBeenCalledTimes(3);
        expect(onJoin).not.toHaveBeenCalled();
        expect(onStartInstead).not.toHaveBeenCalled();
    });
});
