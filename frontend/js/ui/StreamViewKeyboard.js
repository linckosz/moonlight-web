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

/**
 * Virtual-keyboard / physical-keyboard capture subsystem for StreamView.
 *
 * Extracted from StreamView to keep that file focused. These are plain
 * prototype methods (they use `this` = the StreamView instance); they are
 * copied onto StreamView.prototype at the bottom of StreamView.js.
 */
export class StreamViewKeyboard {
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
     */
    _setupKeyboardCapture() {
        const cap = this._kbdCapture;
        if (!cap) return;

        this._resetKbdCapture();

        cap.addEventListener('input', (e) => {
            // Preferred path: InputEvent exposes an explicit type + data. On iOS
            // Safari `cap.value` can lag inside the handler (so the sentinel diff
            // misses characters like "*"), but `e.data`/`e.inputType` are exact.
            // Gboard composition has no usable data → falls through to the diff.
            const it = e && e.inputType;
            // With a latched toolbar modifier, route a typed character through a
            // key event so the modifier applies (Ctrl+C, Alt+F4, Win+R, …).
            if (this._anyModHeld() && it === 'insertText' && e.data != null) {
                const vk = StreamViewKeyboard.charToVk(e.data);
                if (vk != null) {
                    this._sendToolbarKey(vk);
                    this._resetKbdCapture();
                    return;
                }
            }
            if (it === 'insertText' && e.data != null) {
                this.webrtc.send({ type: 'textinput', text: e.data });
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
                this.webrtc.send({ type: 'textinput', text: e.data });
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
                    this.webrtc.send({ type: 'textinput', text: inserted });
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
     *  then release those mods (one-shot, see _releaseLatchedMods). */
    _sendKey(vk) {
        const base = { keyCode: vk, code: '', key: '', ...this._modFlags() };
        this.webrtc.send({ type: 'keydown', ...base });
        this.webrtc.send({ type: 'keyup', ...base });
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

    /** Current latched modifier state as DOM-event-style flags. */
    _modFlags() {
        const m = this._heldMods || {};
        return { ctrlKey: !!m.ctrl, shiftKey: !!m.shift, altKey: !!m.alt, metaKey: !!m.meta };
    }

    _anyModHeld() {
        const m = this._heldMods || {};
        return m.ctrl || m.shift || m.alt || m.meta;
    }

    /** Build the special-keys bar (hidden until the soft keyboard opens). */
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
        const items = [
            ['Win', 'key', 0x5b], // momentary tap → Start menu (single press)
            ['Esc', 'key', 0x1b],
            ['Tab', 'key', 0x09],
            ['Shift', 'mod', 'shift'],
            ['Ctrl', 'mod', 'ctrl'],
            ['Alt', 'mod', 'alt'],
            ['Del', 'key', 0x2e],
            ['←', 'key', 0x25],
            ['↑', 'key', 0x26],
            ['↓', 'key', 0x28],
            ['→', 'key', 0x27],
        ];

        // Arrow + Del VKs use a press-and-hold model (see below).
        const HOLD_REPEAT_VKS = new Set([0x25, 0x26, 0x27, 0x28, 0x2e]);

        for (const [label, kind, id] of items) {
            const btn = document.createElement('button');
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
                // Press-and-hold: keydown while pressed, keyup on release. We send
                // a single keydown and let the GUEST OS generate typematic repeat
                // (one step, then continuous after ~1s) — exactly like a physical
                // arrow/Del key. Sending down+up on tap would only ever move/delete
                // one step.
                const flags = () => ({ keyCode: id, code: '', key: '', ...this._modFlags() });
                const release = (e) => {
                    if (e) e.preventDefault();
                    if (!btn._held) return;
                    btn._held = false;
                    this.webrtc.send({ type: 'keyup', ...flags() });
                    this._releaseLatchedMods();
                    this._refocusCapture();
                };
                btn.addEventListener('pointerdown', (e) => {
                    e.preventDefault();
                    e.stopPropagation();
                    if (btn._held) return;
                    btn._held = true;
                    this.webrtc.send({ type: 'keydown', ...flags() });
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

        document.getElementById('stream-view').appendChild(bar);
        this._kbToolbar = bar;
    }

    /** Cycle a modifier through off → one-shot latch → locked → off.
     *  - tap 1: latch (held, auto-released after the next key) — cyan.
     *  - tap 2 within 300ms (fast double-tap): lock (stays held until tapped
     *    off) — solid blue. A slow second tap just turns it off.
     *  - tap while locked: off.
     *  A real key down/up is sent so a bare press works (e.g. Win alone opens
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
            this.webrtc.send({
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
            this.webrtc.send({
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
     *  release the latches (one-shot sticky keys: Shift/Ctrl/Alt apply to the
     *  next key only and auto-clear once it fires). */
    _sendToolbarKey(vk) {
        const base = { keyCode: vk, code: '', key: '', ...this._modFlags() };
        this.webrtc.send({ type: 'keydown', ...base });
        this.webrtc.send({ type: 'keyup', ...base });
        this._releaseLatchedMods();
    }

    /** Release any latched toolbar modifiers, sending the matching keyup and
     *  clearing each button highlight. Called after a key action so the user
     *  never has to tap a modifier a second time to turn it off. */
    _releaseLatchedMods() {
        const m = this._heldMods;
        if (!m) return;
        for (const name of ['ctrl', 'shift', 'alt', 'meta']) {
            if (!m[name]) continue;
            // Locked modifiers stay held across keys until explicitly tapped off.
            if (this._lockedMods && this._lockedMods[name]) continue;
            m[name] = false; // clear before flags so this one reads as released
            this.webrtc.send({
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

    /** Re-focus the hidden capture so the soft keyboard stays open after a tap. */
    _refocusCapture() {
        const cap = this._kbdCapture;
        if (!cap) return;
        try {
            cap.focus({ preventScroll: true });
        } catch (e) {
            cap.focus();
        }
    }

    _toggleVirtualKeyboard() {
        // Tapping the toggle button blurs the capture <textarea> first, which
        // flips _kbdVisible to false before this runs. Treat a very recent blur
        // as "keyboard was open" so the second tap actually hides it.
        const justBlurred = performance.now() - (this._kbdBlurAt || 0) < 350;
        if (this._kbdVisible || justBlurred) this._hideVirtualKeyboard();
        else this._showVirtualKeyboard();
    }

    /** Focus the hidden capture element — opens the OS soft keyboard.
     *  Must run inside a user gesture (button tap / 3-finger tap). */
    _showVirtualKeyboard() {
        if (!this._kbdCapture) return;
        this._resetKbdCapture();
        try {
            this._kbdCapture.focus({ preventScroll: true });
        } catch (e) {
            this._kbdCapture.focus();
        }
    }

    _hideVirtualKeyboard() {
        if (this._kbdCapture) this._kbdCapture.blur();
    }

    /**
     * Keep the stream fully visible above the soft keyboard.
     * VisualViewport shrinks when the keyboard opens (iOS Safari + Android
     * Chrome); we resize the overlay to the visible area so the centered video
     * fits above the keyboard instead of hiding behind it.
     */
    _handleViewportResize() {
        const vv = window.visualViewport;
        if (!vv || !this.streamEl) return;
        // In real fullscreen the keyboard rarely opens; skip to avoid fighting it.
        const kbHeight = window.innerHeight - vv.height - vv.offsetTop;
        if (kbHeight > 120) {
            // Release `bottom` (set by inset:0) so `height` actually applies.
            this.streamEl.style.bottom = 'auto';
            this.streamEl.style.top = vv.offsetTop + 'px';
            // Park the special-keys bar just above the soft keyboard, then shrink
            // the stream by its height too so the bottom of the picture (e.g. the
            // Windows taskbar) stays visible above the bar instead of behind it.
            let tbHeight = 0;
            if (this._kbToolbar) {
                this._kbToolbar.style.bottom = kbHeight + 'px';
                this._kbToolbar.classList.add('visible');
                tbHeight = this._kbToolbar.offsetHeight;
            }
            this.streamEl.style.height = Math.max(0, vv.height - tbHeight) + 'px';
        } else {
            this.streamEl.style.bottom = '';
            this.streamEl.style.top = '';
            this.streamEl.style.height = '';
            this._hideKbToolbar();
        }
    }

    /** Hide the special-keys bar and release any latched modifiers. */
    _hideKbToolbar() {
        if (!this._kbToolbar) return;
        this._kbToolbar.classList.remove('visible');
        // Release stuck modifiers so the host doesn't keep them held.
        if (this._heldMods) {
            for (const name of Object.keys(this._heldMods)) {
                if (!this._heldMods[name]) continue;
                this._heldMods[name] = false;
                if (this._lockedMods) this._lockedMods[name] = false;
                this.webrtc.send({
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
}
