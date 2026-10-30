/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The "this character needs an input method" warning for the touch keyboard.
 *
 * Reported from an iPhone against Sunshine on Ubuntu: "cyberpunk" arrived as
 * `èç-(èà-eéà` — the hex digits of each code point, typed bare on an AZERTY
 * session because the GTK/IBus prefix had nobody listening. The server now
 * types what a key can type as that key, and names back (`textviaime`) only
 * the characters it had to leave to the host's input method.
 *
 * Three properties matter, and all three are about NOT crying wolf:
 *   - the text always goes out, and typing alone never warns — the letters of
 *     a Wolf host arrived under a warning that they would not (issue #44);
 *   - the warning names the characters the server left to the input method;
 *   - it is said once per view, not once per character.
 */
import { describe, it, expect, vi, beforeEach } from 'vitest';

const warn = vi.fn();
vi.mock('../js/ui/Toast.js', () => ({ Toast: { warning: (...a) => warn(...a) } }));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, vars) => (vars ? `${key}:${vars.chars}` : key),
    applyTranslations: () => {},
}));

const { StreamViewKeyboard } = await import('../js/ui/StreamViewKeyboard.js');

/** A stand-in for the StreamView the mixin's methods run against. */
function view() {
    return {
        webrtc: {
            sent: [],
            send(m) {
                this.sent.push(m);
            },
        },
    };
}

const type = (v, text) => StreamViewKeyboard.prototype._sendTextInput.call(v, text);
const viaIme = (v, text) => StreamViewKeyboard.prototype._noteTextViaIme.call(v, text);

beforeEach(() => warn.mockClear());

describe('touch keyboard: the input-method warning', () => {
    it('sends the text and does not warn on typing alone', () => {
        const v = view();
        type(v, 'cyberpunk');
        expect(v.webrtc.sent).toEqual([{ type: 'textinput', text: 'cyberpunk' }]);
        expect(warn).not.toHaveBeenCalled();
    });

    it('warns when the server left characters to the input method, naming them', () => {
        viaIme(view(), 'é😀');
        expect(warn).toHaveBeenCalledTimes(1);
        expect(warn.mock.calls[0][0]).toBe('stream.textInputNeedsIme:é😀');
    });

    it('names a few characters of a long run, not all of them', () => {
        viaIme(view(), 'àéèùâêîôûç');
        expect(warn.mock.calls[0][0]).toBe('stream.textInputNeedsIme:àéèùâêîô…');
    });

    it('says nothing for an empty run', () => {
        viaIme(view(), '');
        expect(warn).not.toHaveBeenCalled();
    });

    it('says it once per view, not once per character', () => {
        const v = view();
        for (const c of 'éèà') viaIme(v, c);
        expect(warn).toHaveBeenCalledTimes(1);
        viaIme(view(), 'é');
        expect(warn).toHaveBeenCalledTimes(2);
    });
});
