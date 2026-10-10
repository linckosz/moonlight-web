/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

import {
    GAME_FILTERS,
    HID_MAX_SLOTS,
    HidReportPump,
    attachMessage,
    detachMessage,
    isGameDevice,
    modelFilters,
} from './hidWire.js';
import { padKind, padName, parsePadId } from '../stream/gamepadMapping.js';
import { HidppFfbPlayer, HidppTransport, findFfbFeature, hasHidpp } from './hidppFfb.js';
import { PidFfbPlayer, hasNativePid } from './pidFfb.js';
import { HidppRelay, hidppInputIds, isHidppReport } from './hidppRelay.js';

/**
 * The HID passthrough in the stream (plan « Passthrough HID », P2): the game
 * devices the viewer chose are sent to the host as themselves — the real
 * wheel or radio, not an Xbox pad — when the host says it can (`hidcaps`).
 *
 * Which devices: the ones Chrome already lets this page open (a click in its
 * chooser, kept by Chrome for a device with a serial number) whose key the
 * viewer switched on, kept in localStorage (`mw_hid_forward`). Settings holds
 * the switches, on each wheel's, radio's or stick's own row: the first click
 * opens Chrome's chooser narrowed to that model (linkModel). The stream applies
 * them, and offers the link for such a device plugged in but never linked
 * (offers(), at most MAX_OFFERS streams per model).
 *
 * What the host asks back (`hidrequest`: output reports, features) is counted
 * and never written to the real device: a host writing to a wheel's vendor
 * interface could reprogram it.
 *
 * Force feedback (P4) takes its own road, and only when the host says it can
 * (`hidcaps.ffb`): the host decodes what the game asks of the recreated wheel
 * into neutral operations (`hidffb`), and this page plays them through a
 * driver of its own for the real wheel's protocol: Logitech's HID++ 0x8123
 * (hidppFfb.js), or the wheel's own USB PID reports for the wheels that
 * speak PID (pidFfb.js: Moza, Simucube, Fanatec…). The host never writes raw
 * bytes to the wheel. When the device stops being sent, the wheel is reset
 * and left free.
 *
 * A Linux host (`hidcaps.hidpp`) instead lets its kernel's Logitech driver
 * talk HID++ to the recreated wheel (its probe waits on the answers): this page
 * relays those requests to the real wheel through a filter (hidppRelay.js)
 * and sends the answers back as `hidreply`. A HID++ device's own HID++ input
 * reports never go on the 'hid' channel: they are answers and notifications
 * for whoever asked on this side, and the channel's repeats would replay them.
 */

export const FORWARD_KEY = 'mw_hid_forward';
const TICK_MS = 100;

/** `vid:pid` in lowercase hex: how a device is remembered. */
export function deviceKey(device) {
    const h = (n) => (n >>> 0).toString(16).padStart(4, '0');
    return `${h(device.vendorId)}:${h(device.productId)}`;
}

const toBase64 = (bytes) => globalThis.btoa(String.fromCharCode(...bytes));
const fromBase64 = (text) => {
    try {
        return Uint8Array.from(globalThis.atob(String(text || '')), (c) => c.charCodeAt(0));
    } catch {
        return new Uint8Array(0);
    }
};

/** The key GamepadManager knows the same device by. */
export function padKeyOf(device) {
    return `usb:${deviceKey(device)}`;
}

export function loadForwarded(storage = globalThis.localStorage) {
    try {
        const v = JSON.parse(storage?.getItem(FORWARD_KEY) || '[]');
        return new Set(Array.isArray(v) ? v.filter((k) => typeof k === 'string') : []);
    } catch {
        return new Set();
    }
}

export function saveForwarded(keys, storage = globalThis.localStorage) {
    try {
        storage?.setItem(FORWARD_KEY, JSON.stringify(Array.from(keys)));
    } catch {
        /* private window: the switch lasts this page */
    }
}

/** The pads a mapping shortchanges most: their rows offer « as itself ». */
export const HID_KINDS = ['wheel', 'rc', 'flightstick'];

const OFFERS_KEY = 'mw_hid_offers';
export const MAX_OFFERS = 3;

/**
 * The model a pad the Gamepad API sees could be sent as, or null: a wheel,
 * radio or stick the browser gives USB ids for. `kind` overrides the guess
 * (the viewer's choice, from the mappings store).
 * @param {Gamepad|{id: string}} gp
 * @param {string} [kind]
 */
export function hidModelOf(gp, kind = padKind(gp?.id)) {
    const { vid, pid } = parsePadId(gp?.id);
    if (!vid || !pid || !HID_KINDS.includes(kind)) return null;
    return { key: `${vid}:${pid}`, vendorId: parseInt(vid, 16), productId: parseInt(pid, 16) };
}

/**
 * Chrome's chooser for one model, game interfaces only; what the viewer
 * picked is switched on. Needs the click that called it.
 * @returns {Promise<HIDDevice[]>}
 */
export async function linkModel(hid, vendorId, productId, storage = globalThis.localStorage) {
    if (!hid) return [];
    let chosen = [];
    try {
        chosen = await hid.requestDevice({ filters: modelFilters(vendorId, productId) });
    } catch {
        chosen = [];
    }
    chosen = chosen.filter((d) => isGameDevice(d.collections));
    if (chosen.length) {
        const keys = loadForwarded(storage);
        for (const d of chosen) keys.add(deviceKey(d));
        saveForwarded(keys, storage);
    }
    return chosen;
}

function loadOffers(storage) {
    try {
        const v = JSON.parse(storage?.getItem(OFFERS_KEY) || '{}');
        return v && typeof v === 'object' && !Array.isArray(v) ? v : {};
    } catch {
        return {};
    }
}

export class HidPassthrough {
    /**
     * @param {object} o
     * @param {HID} [o.hid] navigator.hid
     * @param {(msg: object) => void} o.send JSON on the input channel
     * @param {(frame: Uint8Array) => boolean} o.sendFrame the 'hid' channel
     * @param {() => void} [o.onChange] the device list or a state changed
     * @param {(device: HIDDevice, ok: boolean, why: string) => void} [o.onResult]
     *     the host's answer to an attach, to tell the viewer
     */
    constructor({
        hid = globalThis.navigator?.hid,
        send,
        sendFrame,
        storage = globalThis.localStorage,
        now = () => performance.now(),
        setTimer = (fn, ms) => setInterval(fn, ms),
        clearTimer = (t) => clearInterval(t),
        onChange = null,
        onResult = null,
    }) {
        this._hid = hid || null;
        this._send = send;
        this._sendFrame = sendFrame;
        this._storage = storage;
        this._now = now;
        this._setTimer = setTimer;
        this._clearTimer = clearTimer;
        this._onChange = onChange;
        this._onResult = onResult;
        this._caps = null; // { available, why } once the host said
        this._pump = new HidReportPump();
        this._entries = new Map(); // HIDDevice -> { slot, state, why, listener }
        this._timer = null;
        this._requests = 0;
        this._offered = new Set(); // models offered in this stream
        this._onDisconnect = (e) => this._drop(e.device, 'unplugged');
        if (this._hid) this._hid.addEventListener('disconnect', this._onDisconnect);
    }

    /** WebHID in this browser at all (Chrome and Edge on a computer). */
    get supported() {
        return !!this._hid;
    }

    /** What the host said: null before, then { available, why }. */
    get caps() {
        return this._caps;
    }

    /** Host requests seen (output reports, features), for diagnostics. */
    get requests() {
        return this._requests;
    }

    /** Devices this page may open that are game devices, with their state. */
    async devices() {
        if (!this._hid) return [];
        const forwarded = loadForwarded(this._storage);
        const list = await this._hid.getDevices();
        return list
            .filter((d) => isGameDevice(d.collections))
            .map((d) => {
                const e = this._entries.get(d);
                return {
                    device: d,
                    key: deviceKey(d),
                    name: d.productName || deviceKey(d),
                    wanted: forwarded.has(deviceKey(d)),
                    state: e ? e.state : 'off',
                    why: e ? e.why : '',
                };
            });
    }

    /** Keys GamepadManager must not forward: devices sent, or being sent, as HID. */
    excludedKeys() {
        const out = [];
        for (const [d, e] of this._entries)
            if (e.state === 'on' || e.state === 'pending') out.push(padKeyOf(d));
        return out;
    }

    /** A message from the host; true when it was one of ours. */
    handleMessage(msg) {
        if (!msg || typeof msg.type !== 'string') return false;
        if (msg.type === 'hidcaps') {
            this._caps = {
                available: !!msg.available,
                ffb: !!msg.ffb,
                hidpp: !!msg.hidpp,
                why: String(msg.why || ''),
            };
            if (this._caps.available) this.applyWanted();
            else this._dropAll(this._caps.why);
            this._changed();
            return true;
        }
        if (msg.type === 'hidattached') {
            const entry = this._entryBySlot(msg.slot);
            if (entry) {
                const [device, e] = entry;
                if (msg.ok) {
                    e.state = 'on';
                    e.why = '';
                    if (e.ffb) this._startFfb(e);
                    else if (this._caps?.hidpp && hasHidpp(device)) this._startRelay(device, e);
                } else {
                    this._release(device, e);
                    e.state = 'refused';
                    e.why = String(msg.why || '');
                }
                if (this._onResult) this._onResult(device, !!msg.ok, e.why);
                this._changed();
            }
            return true;
        }
        if (msg.type === 'hidrequest') {
            this._requests++;
            const relay = this._entryBySlot(msg.slot)?.[1].relay;
            if (relay && msg.kind === 'output' && isHidppReport(msg.reportId)) {
                let bytes = fromBase64(msg.data);
                // Linux hands the report with its id first.
                if (bytes[0] === msg.reportId && (bytes.length === 20 || bytes.length === 64))
                    bytes = bytes.subarray(1);
                relay.handle(msg.reportId, bytes);
            }
            return true;
        }
        if (msg.type === 'hidffb') {
            const entry = this._entryBySlot(msg.slot);
            entry?.[1].ffb?.player?.apply(msg);
            return true;
        }
        return false;
    }

    /** Switch a device on or off for this viewer, and apply it now. */
    async setWanted(device, on) {
        const keys = loadForwarded(this._storage);
        if (on) keys.add(deviceKey(device));
        else keys.delete(deviceKey(device));
        saveForwarded(keys, this._storage);
        if (on) await this._attach(device);
        else this._drop(device, '');
        this._changed();
    }

    /** Attach every wanted device this page may open; called when the host can. */
    async applyWanted() {
        if (!this._hid || !this._caps?.available) return;
        const keys = loadForwarded(this._storage);
        for (const d of await this._hid.getDevices()) {
            if (keys.has(deviceKey(d)) && isGameDevice(d.collections)) await this._attach(d);
        }
        this._changed();
    }

    /** Chrome's chooser, game devices only; needs the click that called it. */
    async choose() {
        if (!this._hid) return [];
        try {
            return await this._hid.requestDevice({ filters: GAME_FILTERS });
        } catch {
            return [];
        }
    }

    /**
     * The chooser for one model, from a click (the stream's offer); what the
     * viewer picked is switched on and sent at once when the host can.
     * @returns {Promise<boolean>} something was linked
     */
    async link(vendorId, productId) {
        const chosen = await linkModel(this._hid, vendorId, productId, this._storage);
        for (const d of chosen) await this._attach(d);
        this._changed();
        return chosen.length > 0;
    }

    /**
     * The pads worth offering to send as themselves, when the host can: a
     * wheel, radio or stick model this browser may not open yet. A model the
     * viewer allowed then switched off is never offered: that was a choice.
     * Each model once a stream, MAX_OFFERS streams in all; Settings stays.
     * @param {Array<Gamepad|{id: string}>} pads
     * @returns {Promise<Array<{key: string, vendorId: number, productId: number, name: string}>>}
     */
    async offers(pads) {
        if (!this._hid || !this._caps?.available) return [];
        let permitted;
        try {
            permitted = new Set(
                (await this._hid.getDevices())
                    .filter((d) => isGameDevice(d.collections))
                    .map(deviceKey),
            );
        } catch {
            return [];
        }
        const counts = loadOffers(this._storage);
        const out = [];
        for (const gp of pads) {
            const m = gp && hidModelOf(gp);
            if (!m || permitted.has(m.key) || this._offered.has(m.key)) continue;
            if ((counts[m.key] | 0) >= MAX_OFFERS) continue;
            this._offered.add(m.key);
            counts[m.key] = (counts[m.key] | 0) + 1;
            out.push({ ...m, name: padName(gp) });
        }
        if (out.length) {
            try {
                this._storage?.setItem(OFFERS_KEY, JSON.stringify(counts));
            } catch {
                /* private window: offered again next stream */
            }
        }
        return out;
    }

    stop() {
        this._dropAll('');
        if (this._hid) this._hid.removeEventListener('disconnect', this._onDisconnect);
        this._changed();
    }

    async _attach(device) {
        if (!this._caps?.available) return;
        const existing = this._entries.get(device);
        if (existing && (existing.state === 'on' || existing.state === 'pending')) return;
        const slot = this._freeSlot();
        if (slot < 0) return;
        try {
            if (!device.opened) await device.open();
        } catch (err) {
            this._entries.set(device, {
                slot: -1,
                state: 'refused',
                why: String(err?.message || err),
                listener: null,
            });
            return;
        }
        const hidppIds = hasHidpp(device) ? hidppInputIds(device) : new Set();
        const listener = (e) => {
            if (hidppIds.has(e.reportId)) return;
            const bytes = new Uint8Array(e.data.buffer, e.data.byteOffset, e.data.byteLength);
            this._sendFrame(this._pump.report(slot, e.reportId, bytes, this._now()));
        };
        device.addEventListener('inputreport', listener);
        const ffb = await this._probeFfb(device);
        this._entries.set(device, { slot, state: 'pending', why: '', listener, ffb });
        this._send(attachMessage(slot, device, { forceFeedback: !!ffb }));
        this._armTimer();
    }

    _drop(device, why) {
        const e = this._entries.get(device);
        if (!e) return;
        if (e.state === 'on' || e.state === 'pending') this._send(detachMessage(e.slot));
        this._release(device, e);
        this._entries.delete(device);
        if (why) this._entries.set(device, { slot: -1, state: 'off', why, listener: null });
        this._changed();
    }

    _dropAll(why) {
        for (const d of Array.from(this._entries.keys())) this._drop(d, why);
        this._disarmTimer();
    }

    // A wheel whose motor this page can drive, when the host takes force
    // feedback: how to make its player and what to close after it, or null.
    async _probeFfb(device) {
        if (!this._caps?.ffb) return null;
        if (hasHidpp(device)) {
            const transport = new HidppTransport(device);
            const index = await findFfbFeature(transport);
            if (index)
                return {
                    make: (o) => new HidppFfbPlayer(transport, index, o),
                    close: () => transport.close(),
                    player: null,
                };
            transport.close();
        }
        if (hasNativePid(device))
            return { make: (o) => new PidFfbPlayer(device, o), close: () => {}, player: null };
        return null;
    }

    _startFfb(e) {
        const f = e.ffb;
        f.player = f.make({
            onError: (err) => console.warn('[HID] force feedback:', err?.message || err),
        });
        f.player
            .init()
            .catch((err) => console.warn('[HID] force feedback init:', err?.message || err));
    }

    // The host's HID++ requests to the real wheel, filtered (Linux host).
    _startRelay(device, e) {
        const slot = e.slot;
        e.relay = new HidppRelay(device, {
            reply: (reportId, bytes) =>
                this._send({ type: 'hidreply', slot, reportId, data: toBase64(bytes) }),
            resetMotor: async () => {
                const t = new HidppTransport(device);
                try {
                    const index = await findFfbFeature(t);
                    if (index) await new HidppFfbPlayer(t, index).close();
                } finally {
                    t.close();
                }
            },
        });
    }

    _release(device, e) {
        if (e.listener) device.removeEventListener('inputreport', e.listener);
        e.listener = null;
        if (e.relay) {
            e.relay.close();
            e.relay = null;
        }
        if (e.ffb) {
            const f = e.ffb;
            e.ffb = null;
            // Reset and freed before the transport goes: never a force left on.
            (f.player ? f.player.close() : Promise.resolve()).finally(() => f.close());
        }
        if (e.slot >= 0) this._pump.forget(e.slot);
        e.slot = -1;
    }

    _freeSlot() {
        const used = new Set(Array.from(this._entries.values(), (e) => e.slot));
        for (let s = 0; s < HID_MAX_SLOTS; s++) if (!used.has(s)) return s;
        return -1;
    }

    _entryBySlot(slot) {
        for (const pair of this._entries) if (pair[1].slot === slot) return pair;
        return null;
    }

    _armTimer() {
        if (this._timer) return;
        this._timer = this._setTimer(() => {
            for (const f of this._pump.due(this._now())) this._sendFrame(f);
        }, TICK_MS);
    }

    _disarmTimer() {
        if (!this._timer) return;
        this._clearTimer(this._timer);
        this._timer = null;
    }

    _changed() {
        if (this._onChange) this._onChange();
    }
}
