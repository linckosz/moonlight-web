/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The "a Linux host needs an input method" warning for the touch keyboard.
 *
 * Reported from an iPhone against Sunshine on Ubuntu: "cyberpunk" arrived as
 * `èç-(èà-eéà` — the hex digits of each code point, typed bare on an AZERTY
 * session because the GTK/IBus prefix had nobody listening. Nothing we send can
 * fix that (sending key events instead would need the host's layout, which the
 * protocol does not carry), so the only useful act is to say it once.
 *
 * Three properties matter, and all three are about NOT crying wolf:
 *   - the text always goes out, warning or not — the notice must never become a
 *     gate on typing;
 *   - hosts that inject text correctly are never warned: MoonlightWeb's own
 *     engine (its Linux backend resolves the character in the session layout),
 *     and any host positively identified as Windows or macOS;
 *   - it is said once per view, not once per character.
 */
import { describe, it, expect, vi, beforeEach } from 'vitest';

const warn = vi.fn();
vi.mock('../js/ui/Toast.js', () => ({ Toast: { warning: (...a) => warn(...a) } }));
vi.mock('../js/i18n/i18n.js', () => ({ t: (key) => key, applyTranslations: () => {} }));

const { StreamViewKeyboard } = await import('../js/ui/StreamViewKeyboard.js');

/** A stand-in for the StreamView the mixin's methods run against. */
function view({ hostOs, nativeHost = false } = {}) {
    return {
        _nativeHost: nativeHost,
        host: hostOs === undefined ? {} : { hostOs },
        webrtc: {
            sent: [],
            send(m) {
                this.sent.push(m);
            },
        },
    };
}

const type = (v, text) => StreamViewKeyboard.prototype._sendTextInput.call(v, text);

beforeEach(() => warn.mockClear());

describe('touch keyboard: the IBus warning', () => {
    it('always sends the text, warning or not', () => {
        const linux = view({ hostOs: 'linux' });
        type(linux, 'cyberpunk');
        expect(linux.webrtc.sent).toEqual([{ type: 'textinput', text: 'cyberpunk' }]);

        const win = view({ hostOs: 'windows' });
        type(win, 'ok');
        expect(win.webrtc.sent).toEqual([{ type: 'textinput', text: 'ok' }]);
    });

    it('warns on a Linux host', () => {
        type(view({ hostOs: 'linux' }), 'a');
        expect(warn).toHaveBeenCalledTimes(1);
        expect(warn.mock.calls[0][0]).toBe('stream.textInputNeedsIme');
    });

    it('warns when the OS could not be established', () => {
        // A TTL of 64 cannot separate Linux from macOS, so most Sunshine hosts
        // on a LAN land here — including the one that produced the bug report.
        type(view({ hostOs: 'unknown' }), 'a');
        type(view({}), 'a'); // no hostOs field at all
        expect(warn).toHaveBeenCalledTimes(2);
    });

    it('stays silent on hosts that inject text correctly', () => {
        type(view({ hostOs: 'windows' }), 'a');
        type(view({ hostOs: 'macos' }), 'a');
        expect(warn).not.toHaveBeenCalled();
    });

    it('stays silent on our own engine, whatever the OS', () => {
        // The native Linux host resolves the character in the real session
        // layout (XkbTextMap), dead keys included.
        type(view({ hostOs: 'linux', nativeHost: true }), 'a');
        type(view({ hostOs: 'unknown', nativeHost: true }), 'a');
        expect(warn).not.toHaveBeenCalled();
    });

    it('says it once, not once per character', () => {
        const v = view({ hostOs: 'linux' });
        for (const c of 'cyberpunk') type(v, c);
        expect(warn).toHaveBeenCalledTimes(1);
        expect(v.webrtc.sent).toHaveLength('cyberpunk'.length);
    });
});
