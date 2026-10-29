/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

import { Toast } from './Toast.js';
import { t } from '../i18n/i18n.js';

/** How much of the bottom of the layout viewport the soft keyboard hides
 *  before we consider it open (an accessory bar alone is smaller than this). */
const KB_OPEN_THRESHOLD = 120;

/** What is left between the picture and the special-keys bar while typing. */
const KB_PICTURE_GAP = 4;

/** Typematic for the press-and-hold toolbar keys: how long a finger has to
 *  stay down before the key starts repeating, and how fast it repeats then.
 *  Roughly the Windows defaults, which is what the arrows should feel like. */
const HOLD_REPEAT_DELAY = 400;
const HOLD_REPEAT_INTERVAL = 40;

/**
 * Publish the height of the bottom strip covered by the soft keyboard and the
 * special-keys bar as --kbd-inset on <html>. Bottom-anchored fixed UI (the
 * toast stack) offsets itself by it: iOS anchors position:fixed to the layout
 * viewport, which the keyboard does not shrink, so a toast at bottom:12px
 * renders behind the keys — unreadable exactly when it matters (the
 * quality-switch warning fires while the user is typing).
 * Document-scoped on purpose: it survives the StreamView swap of a quality
 * switch, so the successor republishes it instead of the toast dropping back
 * behind the keyboard mid-transition.
 */
function publishKbdInset(px) {
    const root = document.documentElement;
    if (px > 0) root.style.setProperty('--kbd-inset', Math.round(px) + 'px');
    else root.style.removeProperty('--kbd-inset');
}

/** @typedef {import('./StreamView.js').StreamView} StreamViewInstance */

/**
 * Virtual-keyboard / physical-keyboard capture subsystem for StreamView.
 *
 * Extracted from StreamView to keep that file focused. These are plain
 * prototype methods (they use `this` = the StreamView instance); they are
 * copied onto StreamView.prototype at the bottom of StreamView.js.
 */
export class StreamViewKeyboard {
    /**
     * Send typed text to the host, warning once when it may arrive as garbage.
     *
     * Why the warning exists
     * ----------------------
     * Reported from an iPhone against Sunshine on Ubuntu: typing "cyberpunk"
     * arrived as `èç-(èà-eéà`. Those are the HEX DIGITS of each code point,
     * landing on the top row of an AZERTY session — Sunshine and Wolf enter a
     * Unicode character with the GTK/IBus sequence (Ctrl+Shift+U, hex, space),
     * and with no input method listening in the host's session the prefix is
     * swallowed and the digits are typed bare.
     *
     * The defect is on the host, and nothing we send can work around it: the
     * alternative — sending key events instead of text — needs the host's
     * KEYBOARD LAYOUT, which no part of the GameStream protocol carries. So the
     * honest thing is to say it, once, to the person who is about to be
     * confused by it.
     *
     * Who gets warned, and who deliberately does not
     * ----------------------------------------------
     *  - MoonlightWeb's own host: never. Its Linux backend resolves the
     *    character in the session's real layout (XkbTextMap), so text works.
     *  - A host positively identified as Windows or macOS: never. Both inject
     *    text directly. HostOsProbe names Windows from its IP TTL, from
     *    MultiSeat, and from the original NVIDIA software.
     *  - Everything else — Linux, and Unknown: warned. Unknown is mostly Linux
     *    or macOS (a TTL of 64 cannot separate them), and a false warning on a
     *    rare macOS host costs one sentence, while staying silent costs a user
     *    a keyboard that types nonsense with no explanation.
     *
     * @param {string} text the characters the soft keyboard produced
     */
    _sendTextInput(text) {
        if (!this._textInputHostWarned) {
            this._textInputHostWarned = true;
            const os = (this.host?.hostOs || '').toLowerCase();
            const mayMangle = !this._nativeHost && os !== 'windows' && os !== 'macos';
            if (mayMangle) Toast.warning(t('stream.textInputNeedsIme'), { durationMs: 10000 });
        }
        this.webrtc.send({ type: 'textinput', text });
    }

    /**
     * Wire the hidden capture <textarea>:
     *   - input: diff the textarea value against a filler sentinel and forward
     *     inserted text (UTF-8 event) / removed chars (Backspace). Diffing on
     *     `input` — not `beforeinput` + inputType — is what makes it work on
     *     Android Gboard (which routes everything through composition, so
     *     inputType is `insertCompositionText`, never `insertText`).
     *     The sentinel keeps the field non-empty so iOS Safari fires a
     *     deletion event on Backspace (an empty field has nothing to delete).
     *   - keydown: navigation/control keys that produce no input (arrows, Tab,
     *     Escape, Home/End/PageUp/Down, Delete).
     *   - focus/blur: keep _kbdVisible and the button state in sync (the OS may
     *     dismiss the keyboard on its own).
     * @this {StreamViewInstance}
     */
    _setupKeyboardCapture() {
        const cap = this._kbdCapture;
        if (!cap) return;

        this._resetKbdCapture();

        cap.addEventListener('input', (/** @type {InputEvent} */ e) => {
            // Preferred path: InputEvent exposes an explicit type + data. On iOS
            // Safari `cap.value` can lag inside the handler (so the sentinel diff
            // misses characters like "*"), but `e.data`/`e.inputType` are exact.
            // Gboard composition has no usable data → falls through to the diff.
            const it = e && e.inputType;
            // With a latched toolbar modifier, route a typed character through a
            // key event so the modifier applies (Ctrl+C, Alt+F4, Win+R, …). The
            // character rides along as a chord: the host presses the key that
            // carries it on its own layout. Its US position alone was Ctrl+W
            // for Ctrl+z on an AZERTY host, and Shift+a typed Q (22/10/2026).
            if (this._anyModHeld() && it === 'insertText' && e.data != null) {
                const vk = StreamViewKeyboard.charToVk(e.data);
                if (vk != null) {
                    this._sendToolbarKey(vk, e.data);
                    this._resetKbdCapture();
                    return;
                }
            }
            if (it === 'insertText' && e.data != null) {
                this._sendTextInput(e.data);
                this._resetKbdCapture();
                return;
            }
            if (it === 'insertLineBreak' || it === 'insertParagraph') {
                this._sendKey(0x0d); // Enter
                this._resetKbdCapture();
                return;
            }
            if (it === 'deleteContentBackward') {
                this._sendKey(0x08); // Backspace
                this._resetKbdCapture();
                return;
            }
            if ((it === 'insertFromPaste' || it === 'insertReplacementText') && e.data != null) {
                this._sendTextInput(e.data);
                this._resetKbdCapture();
                return;
            }

            // Fallback: diff the sentinel (Android Gboard composition, etc.).
            const val = cap.textContent;
            const sent = StreamViewKeyboard.KBD_SENTINEL;
            // Common prefix / suffix between the sentinel and the new value.
            let p = 0;
            const maxP = Math.min(sent.length, val.length);
            while (p < maxP && sent[p] === val[p]) p++;
            let s = 0;
            const maxS = Math.min(sent.length - p, val.length - p);
            while (s < maxS && sent[sent.length - 1 - s] === val[val.length - 1 - s]) s++;

            const deleted = sent.length - p - s; // chars removed from sentinel
            const inserted = val.slice(p, val.length - s); // chars added by the user

            if (deleted > 0) {
                for (let i = 0; i < deleted; i++) this._sendKey(0x08); // Backspace
            }
            if (inserted) {
                // Enter shows up as an inserted line break — send it as a key.
                if (inserted === '\n' || inserted === '\r' || inserted === '\r\n') {
                    this._sendKey(0x0d); // Enter
                } else {
                    this._sendTextInput(inserted);
                }
            }
            // Restore the sentinel so the next keystroke diffs cleanly.
            this._resetKbdCapture();
        });

        // Navigation/control keys not covered by the input diff.
        const navKeys = {
            Tab: 0x09,
            Escape: 0x1b,
            Delete: 0x2e,
            ArrowUp: 0x26,
            ArrowDown: 0x28,
            ArrowLeft: 0x25,
            ArrowRight: 0x27,
            Home: 0x24,
            End: 0x23,
            PageUp: 0x21,
            PageDown: 0x22,
        };
        cap.addEventListener('keydown', (e) => {
            const vk = navKeys[e.code] ?? navKeys[e.key];
            if (vk !== undefined) {
                e.preventDefault();
                this._sendKey(vk);
            }
        });

        cap.addEventListener('focus', () => {
            this._kbdVisible = true;
            if (this._kbdBtn) this._kbdBtn.classList.add('active');
        });
        cap.addEventListener('blur', () => {
            this._kbdVisible = false;
            this._kbdBlurAt = performance.now(); // for the toggle-button race
            if (this._kbdBtn) this._kbdBtn.classList.remove('active');
            // A genuine dismiss hides the bar; a tap on a toolbar button refocuses
            // immediately, so defer to let _refocusCapture cancel the hide.
            setTimeout(() => {
                if (!this._kbdVisible) this._hideKbToolbar();
            }, 50);
        });
    }

    /** Seed the capture element with the filler sentinel and put the caret at
     *  the end, so Backspace always has something to delete and inserted text
     * @this {StreamViewInstance}
     *  appends after the sentinel. (contenteditable: textContent + Range.) */
    _resetKbdCapture() {
        const cap = this._kbdCapture;
        if (!cap) return;
        const sent = StreamViewKeyboard.KBD_SENTINEL;
        if (cap.textContent !== sent) cap.textContent = sent;
        // Only move the caret while focused, to avoid stealing focus on setup.
        if (document.activeElement !== cap) return;
        try {
            const sel = window.getSelection();
            const range = document.createRange();
            range.selectNodeContents(cap);
            range.collapse(false); // caret at end
            sel.removeAllRanges();
            sel.addRange(range);
        } catch (e) {
            /* ignore */
        }
    }

    /** Send a single key press (down+up), applying any latched toolbar mods,
     * @this {StreamViewInstance}
     *  then release those mods (one-shot, see _releaseLatchedMods). */
    _sendKey(vk) {
        const base = { keyCode: vk, code: '', key: '', ...this._modFlags() };
        // _sendKeyEvent (not webrtc.send) so the held-key set stays exact: the
        // host releases anything the heartbeat stops reporting.
        this._sendKeyEvent({ type: 'keydown', ...base });
        this._sendKeyEvent({ type: 'keyup', ...base });
        this._releaseLatchedMods();
    }

    // =========================================================================
    // On-screen special-keys toolbar (touch) — Win/Esc/Tab/mods/Del + arrows
    // =========================================================================

    // Windows virtual-key codes for the latching modifier buttons.
    static KBD_SENTINEL = '    ';
    static MOD_VK = { ctrl: 0x11, shift: 0x10, alt: 0x12, meta: 0x5b };

    /** Map a single printable character to a Windows VK (letters/digits only),
     *  so a character typed with a latched modifier becomes Ctrl/Alt/… + key. */
    static charToVk(ch) {
        if (!ch || ch.length !== 1) return null;
        const c = ch.toUpperCase().charCodeAt(0);
        if (c >= 0x30 && c <= 0x39) return c; // 0-9
        if (c >= 0x41 && c <= 0x5a) return c; // A-Z
        return null;
    }

    /**
     * Current latched modifier state as DOM-event-style flags.
     *
     * @this {StreamViewInstance}
     */
    _modFlags() {
        /** @type {StreamViewInstance['_heldMods']} */
        const m = this._heldMods || {};
        return { ctrlKey: !!m.ctrl, shiftKey: !!m.shift, altKey: !!m.alt, metaKey: !!m.meta };
    }

    /** @this {StreamViewInstance} */
    _anyModHeld() {
        /** @type {StreamViewInstance['_heldMods']} */
        const m = this._heldMods || {};
        return m.ctrl || m.shift || m.alt || m.meta;
    }

    /**
     * Build the special-keys bar (hidden until the soft keyboard opens).
     *
     * @this {StreamViewInstance}
     */
    _buildKbToolbar() {
        this._heldMods = { ctrl: false, shift: false, alt: false, meta: false };
        // Locked modifiers stay held across keys (fast double-tap) until tapped
        // off, unlike the one-shot latch which auto-releases after the next key.
        this._lockedMods = { ctrl: false, shift: false, alt: false, meta: false };
        // Timestamp of the latch tap, to tell a fast double-tap (lock) from a
        // slow second tap (just turn it off).
        this._modLastTap = { ctrl: 0, shift: 0, alt: 0, meta: 0 };
        this._modBtns = {}; // id → button, to clear the latch highlight on release

        const bar = document.createElement('div');
        bar.id = 'stream-kbd-toolbar';
        bar.className = 'stream-kbd-toolbar';

        // [label, kind, id]. kind: 'mod' | 'key'.
        // [label, kind, id] — `id` is a virtual-key code for 'key' entries and a
        // modifier name for 'mod' entries, so the tuple is deliberately mixed.
        /** @type {Array<[string, 'key', number] | [string, 'mod', string]>} */
        const items = [
            // Win latches as Shift, Ctrl and Alt do, for Win+L or Win+R from a
            // phone (Bruno, 26/10/2026); latched and let go alone, it opens Start.
            ['Win', 'mod', 'meta'],
            ['Esc', 'key', 0x1b],
            ['Tab', 'key', 0x09],
            ['Shift', 'mod', 'shift'],
            ['Ctrl', 'mod', 'ctrl'],
            ['Alt', 'mod', 'alt'],
            ['Del', 'key', 0x2e], // with Ctrl and Alt down: Ctrl+Alt+Suppr, see below
            ['←', 'key', 0x25],
            ['↑', 'key', 0x26],
            ['↓', 'key', 0x28],
            ['→', 'key', 0x27],
        ];

        // Arrow + Del VKs use a press-and-hold model (see below).
        const HOLD_REPEAT_VKS = new Set([0x25, 0x26, 0x27, 0x28, 0x2e]);

        for (const [label, kind, id] of items) {
            // `_held` is an expando: the press-and-hold branch below tracks the
            // pressed state on the element itself rather than in a side map.
            const btn = /** @type {HTMLButtonElement & { _held?: boolean }} */ (
                document.createElement('button')
            );
            btn.className = 'stream-kbd-key' + (kind === 'mod' ? ' is-mod' : '');
            btn.textContent = label;
            btn.tabIndex = -1;

            if (kind === 'mod') {
                // pointerdown + preventDefault: act immediately and keep the
                // hidden capture focused so the soft keyboard does not dismiss.
                btn.addEventListener('pointerdown', (e) => {
                    e.preventDefault();
                    e.stopPropagation();
                    this._toggleMod(id, btn);
                    this._refocusCapture();
                });
                this._modBtns[id] = btn;
            } else if (HOLD_REPEAT_VKS.has(id)) {
                // Press-and-hold: keydown while pressed, keyup on release, and a
                // repeat of our own in between. Nothing on the host would produce
                // that repeat otherwise — typematic comes from the keyboard
                // controller, so an injected key gives one step however long it is
                // held — and a held arrow that moves the caret once is not an
                // arrow key. Sending down+up on tap would have the same flaw.
                const flags = () => ({ keyCode: id, code: '', key: '', ...this._modFlags() });
                let holdDelay = null;
                let holdTick = null;
                const stopRepeat = () => {
                    clearTimeout(holdDelay);
                    clearInterval(holdTick);
                    holdDelay = null;
                    holdTick = null;
                };
                const release = (e) => {
                    if (e) e.preventDefault();
                    stopRepeat();
                    if (!btn._held) return;
                    btn._held = false;
                    this._sendKeyEvent({ type: 'keyup', ...flags() });
                    this._releaseLatchedMods();
                    this._refocusCapture();
                };
                btn.addEventListener('pointerdown', (e) => {
                    e.preventDefault();
                    e.stopPropagation();
                    if (btn._held) return;
                    // Del with Ctrl and Alt down is Ctrl+Alt+Suppr, which no
                    // injected key can forge: Windows reserves it in the
                    // kernel, and the viewer's own OS swallows the physical one
                    // before the page sees it. It goes as a message of its own,
                    // which the host turns into a real SAS when it can. Every
                    // modifier is let go first, locked ones included: the host
                    // is about to switch to the secure desktop, where anything
                    // still held would stay held with nobody to release it.
                    if (id === 0x2e && this._heldMods.ctrl && this._heldMods.alt) {
                        this._releaseAllMods();
                        this.webrtc.send({ type: 'secureattention' });
                        this._refocusCapture();
                        return;
                    }
                    btn._held = true;
                    this._sendKeyEvent({ type: 'keydown', ...flags() });
                    // The key stays in the held set throughout, so the repeat is a
                    // plain re-press — the host reads it exactly as it reads the
                    // browser's own auto-repeat on a physical keyboard.
                    stopRepeat();
                    holdDelay = setTimeout(() => {
                        holdTick = setInterval(() => {
                            if (!btn._held) return stopRepeat();
                            this._sendKeyEvent({ type: 'keydown', ...flags() });
                        }, HOLD_REPEAT_INTERVAL);
                    }, HOLD_REPEAT_DELAY);
                    // Capture so we still get the release if the finger slides off.
                    try {
                        btn.setPointerCapture(e.pointerId);
                    } catch (_) {}
                    this._refocusCapture();
                });
                btn.addEventListener('pointerup', release);
                btn.addEventListener('pointercancel', release);
                btn.addEventListener('lostpointercapture', release);
            } else {
                btn.addEventListener('pointerdown', (e) => {
                    e.preventDefault();
                    e.stopPropagation();
                    this._sendToolbarKey(id);
                    this._refocusCapture();
                });
            }
            bar.appendChild(btn);
        }

        (this._rootEl || document.getElementById('stream-view')).appendChild(bar);
        this._kbToolbar = bar;
    }

    /** Cycle a modifier through off → one-shot latch → locked → off.
     *  - tap 1: latch (held, auto-released after the next key) — cyan.
     *  - tap 2 within 300ms (fast double-tap): lock (stays held until tapped
     *    off) — solid blue. A slow second tap just turns it off.
     *  - tap while locked: off.
     *  A real key down/up is sent so a bare press works (e.g. Win alone opens
     * @this {StreamViewInstance}
     *  the Start menu) and the flag carries over to subsequent keys. */
    _toggleMod(name, btn) {
        const vk = StreamViewKeyboard.MOD_VK[name];
        const now = performance.now();
        if (!this._heldMods[name]) {
            // off → latched (one-shot)
            this._heldMods[name] = true;
            this._lockedMods[name] = false;
            this._modLastTap[name] = now;
            // Flags reflect already-held mods (this one now included).
            this._sendKeyEvent({
                type: 'keydown',
                keyCode: vk,
                code: '',
                key: '',
                ...this._modFlags(),
            });
            btn.classList.add('active');
            btn.classList.remove('locked');
        } else if (!this._lockedMods[name] && now - (this._modLastTap[name] || 0) <= 300) {
            // latched → locked (fast double-tap): already held, just mark sticky.
            this._lockedMods[name] = true;
            btn.classList.add('locked');
        } else {
            // locked → off, or a slow second tap on a latched mod → off.
            this._heldMods[name] = false;
            this._lockedMods[name] = false;
            this._sendKeyEvent({
                type: 'keyup',
                keyCode: vk,
                code: '',
                key: '',
                ...this._modFlags(),
            });
            btn.classList.remove('active', 'locked');
        }
    }

    /** Send a toolbar action key (down+up) with the current latched mods, then
     *  release the latches (one-shot sticky keys: Shift/Ctrl/Alt/Win apply to
     *  the next key only and auto-clear once it fires).
     *  `ch`, for a letter or a digit typed under a latched modifier, makes it a
     *  chord: `char` and `chord: true` beside the US position, so a host that
     *  can resolves the shortcut by its character (InputMsg::resolveKey).
     * @this {StreamViewInstance}
     * @param {number} vk
     * @param {string} [ch] */
    _sendToolbarKey(vk, ch) {
        /** @type {Record<string, unknown>} */
        const base = { keyCode: vk, code: '', key: '', ...this._modFlags() };
        if (ch) {
            base.char = ch;
            base.chord = true;
        }
        this._sendKeyEvent({ type: 'keydown', ...base });
        this._sendKeyEvent({ type: 'keyup', ...base });
        this._releaseLatchedMods();
    }

    /** Release any latched toolbar modifiers, sending the matching keyup and
     *  clearing each button highlight. Called after a key action so the user
     * @this {StreamViewInstance}
     *  never has to tap a modifier a second time to turn it off. */
    _releaseLatchedMods() {
        const m = this._heldMods;
        if (!m) return;
        for (const name of ['ctrl', 'shift', 'alt', 'meta']) {
            if (!m[name]) continue;
            // Locked modifiers stay held across keys until explicitly tapped off.
            if (this._lockedMods && this._lockedMods[name]) continue;
            m[name] = false; // clear before flags so this one reads as released
            this._sendKeyEvent({
                type: 'keyup',
                keyCode: StreamViewKeyboard.MOD_VK[name],
                code: '',
                key: '',
                ...this._modFlags(),
            });
            const btn = this._modBtns && this._modBtns[name];
            if (btn) btn.classList.remove('active');
        }
    }

    /**
     * Re-focus the hidden capture so the soft keyboard stays open after a tap.
     *
     * @this {StreamViewInstance}
     */
    _refocusCapture() {
        const cap = this._kbdCapture;
        if (!cap) return;
        try {
            cap.focus({ preventScroll: true });
        } catch (e) {
            cap.focus();
        }
    }

    /** @this {StreamViewInstance} */
    _toggleVirtualKeyboard() {
        // Tapping the toggle button blurs the capture <textarea> first, which
        // flips _kbdVisible to false before this runs. Treat a very recent blur
        // as "keyboard was open" so the second tap actually hides it.
        const justBlurred = performance.now() - (this._kbdBlurAt || 0) < 350;
        if (this._kbdVisible || justBlurred) this._hideVirtualKeyboard();
        else this._showVirtualKeyboard();
    }

    /** Focus the hidden capture element — opens the OS soft keyboard.
     * @this {StreamViewInstance}
     *  Must run inside a user gesture (button tap / 3-finger tap). */
    _showVirtualKeyboard() {
        // A guest without keyboard: the 3-finger tap opens nothing either.
        if (!this._kbdCapture || !this._mouseAllowed) return;
        this._resetKbdCapture();
        try {
            this._kbdCapture.focus({ preventScroll: true });
        } catch (e) {
            this._kbdCapture.focus();
        }
    }

    /** @this {StreamViewInstance} */
    _hideVirtualKeyboard() {
        if (this._kbdCapture) this._kbdCapture.blur();
    }

    /**
     * Keep the stream fully visible above the soft keyboard.
     * VisualViewport shrinks when the keyboard opens (iOS Safari + Android
     * Chrome); we resize the overlay to the visible area so the centered video
     * fits above the keyboard instead of hiding behind it.
     * @this {StreamViewInstance}
     */
    _handleViewportResize() {
        const vv = window.visualViewport;
        if (!vv || !this.streamEl) return;
        // What the soft keyboard hides of the bottom of the layout viewport: all
        // a bottom-anchored fixed element (the toast stack) has to clear.
        const covered = Math.max(0, window.innerHeight - vv.height - vv.offsetTop);
        // Not every browser keeps the layout viewport when the keyboard opens.
        // Chrome on an iPhone (27/10/2026) shrinks it with the visible area:
        // `covered` then stays near 0 and the bar never showed, or, read before
        // the layout caught up, the bar went that far above a bottom that had
        // already moved — to the top of the screen. So the keyboard is told by
        // the height the page had while no field held the focus, and the bar is
        // placed from the top of the visible area, which holds either way.
        const ae = document.activeElement;
        const typing =
            this._kbdVisible ||
            (ae instanceof HTMLElement &&
                (ae.isContentEditable || ae.tagName === 'TEXTAREA' || ae.tagName === 'INPUT'));
        if (!typing || !this._layoutHeight) this._layoutHeight = window.innerHeight;
        const kbHeight =
            Math.max(this._layoutHeight, window.innerHeight) - vv.height - vv.offsetTop;
        if (kbHeight > KB_OPEN_THRESHOLD) {
            // Release `bottom` (set by inset:0) so `height` actually applies.
            this.streamEl.style.bottom = 'auto';
            this.streamEl.style.top = vv.offsetTop + 'px';
            // Park the special-keys bar just above the soft keyboard, then shrink
            // the stream by its height too so the bottom of the picture (e.g. the
            // Windows taskbar) stays visible above the bar instead of behind it.
            // The pan the shorter area is about to clamp, for the way back, and
            // where the clamp left it: a pan that moves from there is the
            // user's own, kept when the keyboard closes (iPhone, 29/10/2026).
            if (!this._panBeforeKbd) {
                this._panBeforeKbd = { x: this._panX, y: this._panY, zoom: this._zoom };
            }
            const kept = this._panBeforeKbd;
            if (kept.atX !== undefined && Math.abs(this._panX - kept.atX) > 0.5) kept.movedX = true;
            if (kept.atY !== undefined && Math.abs(this._panY - kept.atY) > 0.5) kept.movedY = true;
            let tbHeight = 0;
            if (this._kbToolbar) {
                this._kbToolbar.classList.add('visible');
                tbHeight = this._kbToolbar.offsetHeight;
                this._kbToolbar.style.bottom = 'auto';
                this._kbToolbar.style.top = vv.offsetTop + vv.height - tbHeight + 'px';
            }
            this.streamEl.style.height = Math.max(0, vv.height - tbHeight) + 'px';
            // Re-clamp the pan to the shorter area, then seat the picture.
            if (this._applyZoomTransform) this._applyZoomTransform();
            else this._seatPictureOnKbToolbar();
            kept.atX = this._panX;
            kept.atY = this._panY;
            // A hidden standby leg measures the same viewport as the live one;
            // let the visible view own the document-wide inset.
            if (!this._standby) publishKbdInset(covered + tbHeight);
        } else {
            // iOS Safari can hand over a page whose visible area starts inside
            // the layout viewport rather than at its top — seen after an update,
            // when the page arrives through the entry page. Every fixed element
            // then sits that far up, the header off the screen, and nothing
            // fires until a rotation resets it. Scroll it back if the window
            // owns the offset; otherwise follow the visible area, as for the
            // keyboard, so the header at least stays on screen.
            if (vv.offsetTop > 1 && (window.scrollX || window.scrollY)) window.scrollTo(0, 0);
            const shifted = vv.offsetTop > 1;
            this.streamEl.style.bottom = shifted ? 'auto' : '';
            this.streamEl.style.top = shifted ? vv.offsetTop + 'px' : '';
            this.streamEl.style.height = shifted ? vv.height + 'px' : '';
            this._hideKbToolbar();
            // Closing the keyboard puts the picture back where it was before:
            // the clamp to the shorter area moved it, not the user (iPhone,
            // 27/10/2026). Only what the clamp moved, axis by axis: a pan the
            // user made while typing stays, or closing the keyboard slides the
            // desktop back sideways (iPhone, 29/10/2026). A zoom changed while
            // typing keeps its own pan.
            const before = this._panBeforeKbd;
            this._panBeforeKbd = null;
            if (before && Math.abs((before.zoom ?? 1) - (this._zoom ?? 1)) < 0.001) {
                const untouched = (moved, at, now) =>
                    !moved && (at === undefined || Math.abs(now - at) <= 0.5);
                if (untouched(before.movedX, before.atX, this._panX)) this._panX = before.x;
                if (untouched(before.movedY, before.atY, this._panY)) this._panY = before.y;
            }
            if (this._applyZoomTransform) this._applyZoomTransform();
            else this._seatPictureOnKbToolbar(false);
            if (!this._standby) publishKbdInset(0);
        }
        // The header moves with the visible area; the stats card sits under it.
        this._positionStatsOverlay();
    }

    /**
     * With the soft keyboard up, the picture rests a few pixels above the
     * special-keys bar instead of in the middle of the area left to it: a
     * landscape desktop on a portrait phone otherwise leaves half the spare
     * height as a band of black between the picture and the bar (27/10/2026).
     * The area is moved, not the picture inside it: every mapping of a touch
     * or the cursor reads the area through getBoundingClientRect, which
     * carries the move, so the letterbox arithmetic stays centred. A zoomed
     * picture is measured at its zoom; once taller than the area it needs no
     * move, the pan clamp (_applyZoomTransform) keeps its bottom on the bar.
     * @this {StreamViewInstance}
     * @param {boolean} [seat] false puts the area back where the layout has it.
     */
    _seatPictureOnKbToolbar(seat = true) {
        const area = this.canvasArea;
        if (!area) return;
        let shift = 0;
        if (seat && typeof this._surfaceIntrinsic === 'function') {
            const [iw, ih] = this._surfaceIntrinsic(this._displayEl());
            const w = area.offsetWidth,
                h = area.offsetHeight;
            if (iw > 0 && ih > 0 && w > 0 && h > 0) {
                const zoom = this._zoom > 1.001 ? this._zoom : 1;
                const picH = ih * Math.min(w / iw, h / ih) * zoom;
                shift = Math.max(0, Math.floor((h - picH) / 2) - KB_PICTURE_GAP);
            }
        }
        // Moved by its relative offset, never by a transform: a transformed
        // ancestor becomes the containing block of its fixed descendants, and
        // the pointer drawn for the trackpad (.stream-client-cursor, fixed,
        // placed in viewport coordinates) then landed as far down as the area
        // itself starts — the top of the picture out of its reach (iPhone,
        // 28/10/2026).
        const top = shift ? shift + 'px' : '';
        if (area.style.top === top) return;
        area.style.top = top;
        this._invalidateMediaRect?.();
        this._placeClientCursor?.();
    }

    /**
     * Teardown counterpart: drop the inset this view published — unless the
     * soft keyboard is still up. A quality switch tears the view down while a
     * bridge element holds the keyboard open (see StreamView.bridgeSoftKeyboard),
     * and the successor only republishes on its first frame; clearing here would
     * drop the toast stack behind the keys for the whole transition. This
     * view's own bar is already gone by now; a bar still standing belongs to
     * the successor, which the inset must keep clearing.
     * @this {StreamViewInstance}
     */
    _releaseKbdInset() {
        const vv = window.visualViewport;
        const kbHeight = vv ? window.innerHeight - vv.height - vv.offsetTop : 0;
        if (kbHeight <= KB_OPEN_THRESHOLD) {
            publishKbdInset(0);
            return;
        }
        const bar = /** @type {HTMLElement} */ (
            document.querySelector('.stream-kbd-toolbar.visible')
        );
        publishKbdInset(kbHeight + (bar ? bar.offsetHeight : 0));
    }

    /**
     * Hide the special-keys bar and release any latched modifiers.
     *
     * @this {StreamViewInstance}
     */
    _hideKbToolbar() {
        if (!this._kbToolbar) return;
        this._kbToolbar.classList.remove('visible');
        // Release stuck modifiers so the host doesn't keep them held.
        this._releaseAllMods();
    }

    /**
     * Release every toolbar modifier, latched or locked, and clear the
     * highlights.
     *
     * @this {StreamViewInstance}
     */
    _releaseAllMods() {
        if (!this._heldMods || !this._kbToolbar) return;
        for (const name of Object.keys(this._heldMods)) {
            if (!this._heldMods[name]) continue;
            this._heldMods[name] = false;
            if (this._lockedMods) this._lockedMods[name] = false;
            this._sendKeyEvent({
                type: 'keyup',
                keyCode: StreamViewKeyboard.MOD_VK[name],
                code: '',
                key: '',
                ctrlKey: false,
                shiftKey: false,
                altKey: false,
                metaKey: false,
            });
        }
        this._kbToolbar
            .querySelectorAll('.stream-kbd-key.active, .stream-kbd-key.locked')
            .forEach((b) => b.classList.remove('active', 'locked'));
    }
}
