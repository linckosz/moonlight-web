/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { isViewMessage } from '../js/api/hostMessages.js';

/** The message types StreamView._handleStatsMessage answers, read from its source. */
function handledByStreamView() {
    const src = readFileSync(
        join(dirname(fileURLToPath(import.meta.url)), '..', 'js', 'ui', 'StreamView.js'),
        'utf8',
    );
    const start = src.indexOf('    _handleStatsMessage(msg) {');
    const end = src.indexOf('\n    }', start);
    const body = src.slice(start, end);
    return [...body.matchAll(/(?<!typeof )msg\.type === '([a-z]+)'/g)].map((m) => m[1]);
}

describe('hostMessages — what reaches the stream view', () => {
    it('forwards every message StreamView handles, the pointer position included', () => {
        for (const type of [
            'stats',
            'pong',
            'rumble',
            'padnumber',
            'clipboard',
            'clipboardcaps',
            'cursor',
            'cursorpos',
            'inputgate',
            'displayformat',
            'feedcodec',
            'fpsstep',
            'inputstamp',
            'textviaime',
        ]) {
            expect(isViewMessage(type)).toBe(true);
        }
    });

    it('lets through every type StreamView answers, so none is dropped on the way (issue #44)', () => {
        // The touch keyboard's "needs an input method" warning (`textviaime`)
        // was sent by the host and answered by StreamView, but never reached
        // it: the transports drop what this list does not name.
        const types = handledByStreamView();
        expect(types).toContain('textviaime');
        expect(types.length).toBeGreaterThan(10);
        for (const type of types) expect(isViewMessage(type), type).toBe(true);
    });

    it('keeps what the transports handle themselves', () => {
        for (const type of ['takeover', 'revoked', 'exit', '', undefined]) {
            expect(isViewMessage(type)).toBe(false);
        }
    });
});
