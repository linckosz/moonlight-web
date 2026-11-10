/* MoonlightWeb — session input preferences. GPLv3 — see repository LICENSE. */

export const MODIFIER_FLAGS = {
    ControlLeft: 'ctrlKey',
    ControlRight: 'ctrlKey',
    ShiftLeft: 'shiftKey',
    ShiftRight: 'shiftKey',
    AltLeft: 'altKey',
    AltRight: 'altKey',
    MetaLeft: 'metaKey',
    MetaRight: 'metaKey',
};

/** @param {(code: string) => boolean} validCode */
export function normalizeInput(value = {}, validCode = () => true) {
    const mappings = {};
    for (const [from, to] of Object.entries(value?.mappings || {}).slice(0, 64)) {
        if (from !== to && validCode(from) && (to === '' || validCode(to))) mappings[from] = to;
    }
    return {
        mappings,
    };
}

export function inputStorageKey(host, player = false) {
    return host?.uuid ? `mw-session-input:${player ? 'guest' : 'owner'}:${host.uuid}` : null;
}

export function loadInput(key, validCode) {
    try {
        return normalizeInput(key ? JSON.parse(localStorage.getItem(key) || '{}') : {}, validCode);
    } catch {
        return normalizeInput();
    }
}

export function saveInput(key, settings) {
    if (!key) return;
    localStorage.setItem(key, JSON.stringify(settings));
}

/** Map wire messages, leaving browser shortcuts and physical-key bookkeeping intact. */
export class SessionKeyMapper {
    constructor(mappings, keyCode, holdsThroughStall = null) {
        this.mappings = mappings;
        this.keyCode = keyCode;
        this.holdsThroughStall = holdsThroughStall;
        this.pressed = new Map();
        this.physicalModifiers = new Set();
    }

    _key(msg) {
        if (Object.hasOwn(this.mappings, msg.code) && this.mappings[msg.code] === '')
            return { ...msg, code: '', keyCode: 0, ignored: true };
        const code = this.mappings[msg.code];
        if (!code) {
            // Character injection can discard mapped Option/Shift on the host.
            // Freeze the chord as physical keys, including its matching release.
            const mappedModifier = [
                ...new Set([...this.pressed.keys(), ...this.physicalModifiers]),
            ].some(
                (source) =>
                    Object.hasOwn(this.mappings, source) &&
                    (MODIFIER_FLAGS[source] || MODIFIER_FLAGS[this.mappings[source]]),
            );
            return mappedModifier && !MODIFIER_FLAGS[msg.code]
                ? {
                      ...msg,
                      char: null,
                      nonUs: false,
                      dead: false,
                      chord: false,
                      altGraph: false,
                      altGrCtrl: false,
                  }
                : { ...msg };
        }
        return {
            ...msg,
            code,
            keyCode: this.keyCode(code),
            key: code,
            char: null,
            nonUs: false,
            dead: false,
            chord: false,
            altGraph: false,
            altGrCtrl: false,
            hold: this.holdsThroughStall ? this.holdsThroughStall(code) : !!msg.hold,
        };
    }

    observeModifiers(msg, down) {
        // Browser state survives a synthetic release around clipboard injection.
        // Only an observed physical event identifies the left/right modifier.
        for (const code of this.physicalModifiers) {
            if (!msg[MODIFIER_FLAGS[code]]) this.physicalModifiers.delete(code);
        }
        if (MODIFIER_FLAGS[msg.code]) {
            if (down) this.physicalModifiers.add(msg.code);
            else this.physicalModifiers.delete(msg.code);
        }
    }

    _modifiers(msg) {
        const flags = { ctrlKey: false, shiftKey: false, altKey: false, metaKey: false };
        for (const flag of new Set(Object.values(MODIFIER_FLAGS))) {
            if (!msg[flag]) continue;
            const siblings = Object.keys(MODIFIER_FLAGS).filter((c) => MODIFIER_FLAGS[c] === flag);
            const held = siblings.filter(
                (c) => this.physicalModifiers.has(c) || this.pressed.has(c),
            );
            // A modifier held before capture has no press; preserve the native flag.
            if (!held.length) flags[flag] = true;
            else
                for (const c of held) {
                    const target = Object.hasOwn(this.mappings, c) ? this.mappings[c] : c;
                    const targetFlag = MODIFIER_FLAGS[target];
                    if (targetFlag) flags[targetFlag] = true;
                }
        }
        // Non-modifier keys can themselves be mapped to a modifier.
        for (const [source, target] of this.pressed) {
            if (!MODIFIER_FLAGS[source] && MODIFIER_FLAGS[target.code])
                flags[MODIFIER_FLAGS[target.code]] = true;
        }
        return flags;
    }

    preview(msg) {
        const mapped = this._key(msg);
        return { ...mapped, ...this._modifiers(msg) };
    }

    send(msg) {
        if (!msg.code) return msg; // Buttons and synthetic clipboard chords already name host keys.
        const previous = this.pressed.get(msg.code);
        const mapped = previous ? { ...previous, type: msg.type } : this._key(msg);
        if (msg.type === 'keydown') this.pressed.set(msg.code, mapped);
        else this.pressed.delete(msg.code);
        const flags = this._modifiers(msg);
        if (msg.type === 'keyup' && [...this.pressed.values()].some((p) => p.code === mapped.code))
            return null;
        return mapped.ignored ? null : { ...mapped, ...flags };
    }

    held(keys) {
        const result = new Map();
        for (const msg of keys) {
            const mapped = this.pressed.get(msg.code) || this._key(msg);
            if (mapped.ignored) continue;
            result.set(mapped.code || mapped.keyCode, {
                ...msg,
                ...mapped,
                ...this._modifiers(msg),
            });
        }
        return [...result.values()];
    }
}
