/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

/**
 * Force feedback on a wheel that speaks USB PID 1.0 itself (plan « Passthrough
 * HID », P3): Moza, Simucube, Fanatec, Thrustmaster, VRS, OpenFFBoard…
 *
 * The host never shows the wheel's own PID block to its OS: it gives the
 * recreated wheel a block of its own, answers the game's synchronous questions
 * there, and sends what the game asks as `hidffb` operations (HidPid.h). This
 * file writes them back into the real wheel's PID reports, found by usage in
 * the collections WebHID shows:
 *  - a new effect: Create New Effect (feature) then Block Load (feature, read
 *    back) gives the wheel's own effect block, mapped to the game's;
 *  - parameters, start, stop, free, gain and device control: the matching
 *    output report, each value rescaled from the host's ranges (forces
 *    -10000..10000, gains 0..255, times in ms, angles in 1/100°) to the
 *    wheel's logical bounds and units;
 *  - one command at a time, keyed as for HID++ (hidppFfb.js): newer
 *    parameters for an effect replace the older ones still waiting.
 *
 * Bit layout. WebHID hands collections, not the descriptor. Chrome's own
 * descriptor parser (macOS, Linux) lists each item in its collection and in
 * every ancestor, in descriptor order: a top-level collection holds each
 * report whole, and the layout is read there; a PID collection (Set Effect,
 * Block Load…) names its report. Chrome on Windows rebuilds one flat
 * collection from Windows' preparsed data: no PID collection names a report,
 * and the usages of an array (Effect Type) come in an order it does not keep.
 * So a wheel on a Windows client gets no force feedback from here
 * (hasNativePid() is false there). To check on the first real PID wheel.
 */

import { sameItem } from './hidWire.js';

const PAGE_PID = 0x0f;
const PAGE_DESKTOP = 0x01;
const PAGE_ORDINAL = 0x0a;
const pid = (u) => (PAGE_PID << 16) | u;

const U = {
    setEffect: 0x21,
    blockIndex: 0x22,
    paramBlockOffset: 0x23,
    duration: 0x50,
    samplePeriod: 0x51,
    gain: 0x52,
    triggerButton: 0x53,
    directionEnable: 0x56,
    setEnvelope: 0x5a,
    attackLevel: 0x5b,
    attackTime: 0x5c,
    fadeLevel: 0x5d,
    fadeTime: 0x5e,
    setCondition: 0x5f,
    cpOffset: 0x60,
    positiveCoefficient: 0x61,
    negativeCoefficient: 0x62,
    positiveSaturation: 0x63,
    negativeSaturation: 0x64,
    deadBand: 0x65,
    setPeriodic: 0x6e,
    offset: 0x6f,
    magnitude: 0x70,
    phase: 0x71,
    period: 0x72,
    setConstant: 0x73,
    setRamp: 0x74,
    rampStart: 0x75,
    rampEnd: 0x76,
    effectOperationReport: 0x77,
    opStart: 0x79,
    opSolo: 0x7a,
    opStop: 0x7b,
    loopCount: 0x7c,
    deviceGainReport: 0x7d,
    deviceGain: 0x7e,
    blockLoad: 0x89,
    loadSuccess: 0x8c,
    blockFree: 0x90,
    deviceControlReport: 0x95,
    startDelay: 0xa7,
    createNewEffect: 0xab,
};

const EFFECT_TYPES = {
    constant: 0x26,
    ramp: 0x27,
    square: 0x30,
    sine: 0x31,
    triangle: 0x32,
    sawtoothUp: 0x33,
    sawtoothDown: 0x34,
    spring: 0x40,
    damper: 0x41,
    inertia: 0x42,
    friction: 0x43,
};

const CONTROLS = {
    enable: 0x97,
    disable: 0x98,
    stopAll: 0x99,
    reset: 0x9a,
    pause: 0x9b,
    continue: 0x9c,
};

const KINDS = ['inputReports', 'outputReports', 'featureReports'];
const READ_TIMEOUT_MS = 500;

// Where `run` sits in `list` from `from` on, or -1.
function findRun(list, run, from) {
    for (let i = from; i + run.length <= list.length; i++)
        if (run.every((item, k) => sameItem(list[i + k], item))) return i;
    return -1;
}

// True when `c` lists its children's items too, each child's as one run, in
// order: Chrome's own descriptor parser. False with no children (Chrome on
// Windows), or when a tree lists each collection's own items only.
function listsChildren(c) {
    const kids = c.children || [];
    if (!kids.length) return false;
    for (const kind of KINDS) {
        const mine = c[kind] || [];
        const next = new Map();
        for (const ch of kids)
            for (const r of ch[kind] || []) {
                const items = r.items || [];
                if (!items.length) continue;
                const m = mine.find((x) => x.reportId === r.reportId);
                const at = m ? findRun(m.items || [], items, next.get(r.reportId) || 0) : -1;
                if (at < 0) return false;
                next.set(r.reportId, at + items.length);
            }
    }
    return true;
}

// Every item of a collection's reports, whole and in order: [{ kind, id, item }].
function wholeReports(c) {
    const own = [];
    for (const kind of KINDS)
        for (const r of c[kind] || [])
            for (const item of r.items || []) own.push({ kind, id: r.reportId, item });
    if (listsChildren(c)) return own;
    return [...own, ...(c.children || []).flatMap(wholeReports)];
}

// The report each PID collection names, by its usage: the first it holds.
function pidReports(c, ids) {
    if (c.usagePage === PAGE_PID && !ids.has(c.usage)) {
        const first = wholeReports(c)[0];
        if (first) ids.set(c.usage, { kind: first.kind, id: first.id });
    }
    for (const ch of c.children || []) pidReports(ch, ids);
}

// Chrome's own parser makes no range of a one-usage range (Usage Minimum and
// Maximum alike), and gives it no usage: its minimum is the usage.
function usagesOf(item) {
    const usages = item.usages || [];
    if (usages.length || item.isRange || !item.usageMinimum) return usages;
    return item.usageMinimum === item.usageMaximum ? [item.usageMinimum] : usages;
}

/**
 * The layout of a device's reports from its WebHID collections: fields with
 * their bit offsets by `${kind}:${id}`, and the report of each PID collection
 * by its usage.
 */
export function reportLayout(collections) {
    const ids = new Map();
    const reports = new Map();
    for (const c of collections || []) {
        pidReports(c, ids);
        for (const { kind, id, item } of wholeReports(c)) {
            const key = `${kind}:${id}`;
            let r = reports.get(key);
            if (!r) reports.set(key, (r = { id, bits: 0, fields: [] }));
            r.fields.push({
                usages: usagesOf(item),
                isRange: !!item.isRange,
                usageMinimum: item.usageMinimum,
                usageMaximum: item.usageMaximum,
                isArray: !!item.isArray,
                isConstant: !!item.isConstant,
                size: item.reportSize,
                count: item.reportCount,
                offset: r.bits,
                min: item.logicalMinimum,
                max: item.logicalMaximum,
                unitExponent: item.unitExponent || 0,
                timeUnit: (item.unitFactorTimeExponent || 0) === 1,
            });
            r.bits += item.reportSize * item.reportCount;
        }
    }
    return { reports, ids };
}

/** True when the device carries the PID reports this file drives. */
export function hasNativePid(device) {
    const { ids } = reportLayout(device?.collections);
    return [U.setEffect, U.effectOperationReport, U.createNewEffect, U.blockLoad].every((u) =>
        ids.has(u),
    );
}

const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

function setBits(body, offset, size, value) {
    for (let b = 0; b < size; b++) {
        const bit = offset + b;
        const mask = 1 << (bit % 8);
        if (Math.floor(value / 2 ** b) % 2) body[bit >> 3] |= mask;
        else body[bit >> 3] &= ~mask;
    }
}

function getBits(body, offset, size) {
    let v = 0;
    for (let b = 0; b < size; b++) {
        const bit = offset + b;
        if ((body[bit >> 3] >> (bit % 8)) & 1) v += 2 ** b;
    }
    return v;
}

/** One PID report being written: fields set by usage, missing ones skipped. */
class ReportWriter {
    constructor(report) {
        this.report = report;
        this.body = new Uint8Array(Math.ceil(report.bits / 8));
    }

    // A variable field holding `usage`: its place and bounds, or null.
    _var(usage) {
        for (const f of this.report.fields) {
            if (f.isArray || f.isConstant) continue;
            let i = -1;
            if (f.isRange) {
                if (usage >= f.usageMinimum && usage <= f.usageMaximum) i = usage - f.usageMinimum;
            } else {
                i = f.usages.indexOf(usage);
            }
            if (i >= 0 && i < f.count) return { ...f, offset: f.offset + i * f.size };
        }
        return null;
    }

    has(usage) {
        return !!this._var(usage);
    }

    /** A raw value, clamped to the field's bounds (two's complement when signed). */
    raw(usage, v) {
        const f = this._var(usage);
        if (!f) return this;
        const n = clamp(Math.round(v), Math.min(f.min, f.max), Math.max(f.min, f.max));
        setBits(this.body, f.offset, f.size, n < 0 ? n + 2 ** f.size : n);
        return this;
    }

    /** Every bit set: the null value, "infinite" for a duration. */
    ones(usage) {
        const f = this._var(usage);
        if (f) setBits(this.body, f.offset, f.size, 2 ** f.size - 1);
        return this;
    }

    /** -10000..10000 onto the field's bounds. */
    signed(usage, v) {
        const f = this._var(usage);
        if (!f) return this;
        const n = f.min < 0 ? (v * f.max) / 10000 : f.min + ((v + 10000) * (f.max - f.min)) / 20000;
        return this.raw(usage, n);
    }

    /** 0..`top` onto the field's bounds. */
    level(usage, v, top = 10000) {
        const f = this._var(usage);
        if (!f) return this;
        return this.raw(usage, Math.max(0, f.min) + (v * (f.max - Math.max(0, f.min))) / top);
    }

    /** Hundredths of a degree onto the field's whole turn. */
    angle(usage, v) {
        const f = this._var(usage);
        if (!f) return this;
        const turn = ((v % 36000) + 36000) % 36000;
        return this.raw(usage, f.min + Math.floor((turn * (f.max - f.min + 1)) / 36000));
    }

    /** Milliseconds in the field's time unit (milliseconds when it has none). */
    ms(usage, v) {
        const f = this._var(usage);
        if (!f) return this;
        const scale = f.timeUnit ? 10 ** (-3 - f.unitExponent) : 1;
        return this.raw(usage, (v || 0) * scale);
    }

    /** An array field: the index of `usage` among its usages. */
    select(usage) {
        for (const f of this.report.fields) {
            if (!f.isArray || f.isConstant) continue;
            let i = -1;
            if (f.isRange) {
                if (usage >= f.usageMinimum && usage <= f.usageMaximum) i = usage - f.usageMinimum;
            } else {
                i = f.usages.indexOf(usage);
            }
            if (i < 0) continue;
            setBits(this.body, f.offset, f.size, f.min + i);
            return this;
        }
        return this;
    }

    /** Reads a variable field from `body` (as the device sent it). */
    read(usage) {
        const f = this._var(usage);
        return f ? getBits(this.body, f.offset, f.size) : 0;
    }

    /** Reads which usage an array field holding `anyUsage` selects. */
    selected(anyUsage) {
        for (const f of this.report.fields) {
            if (!f.isArray || !f.usages.includes(anyUsage)) continue;
            return f.usages[getBits(this.body, f.offset, f.size) - f.min] || 0;
        }
        return 0;
    }
}

/**
 * The `hidffb` operations of one recreated wheel, written into the real
 * wheel's PID reports. `onError` hears about a failed command.
 */
export class PidFfbPlayer {
    constructor(device, { onError = null, timeoutMs = READ_TIMEOUT_MS } = {}) {
        this._device = device;
        this._layout = reportLayout(device.collections);
        this._onError = onError;
        this._timeoutMs = timeoutMs;
        this._effects = new Map(); // game effect block -> state
        this._queue = [];
        this._busy = false;
        this._closed = false;
        this._frees = 0;
        this._ready = null; // init() while it runs
    }

    /**
     * No effect left, actuators on, full gain. The host's operations wait for
     * it, as on HidppFfbPlayer: a kept wheel's replay must not come first.
     */
    init() {
        this._ready = (async () => {
            await this._control('reset');
            await this._control('enable');
            await this._gain(255);
        })().finally(() => {
            this._ready = null; // done: nothing waits on it any more
        });
        return this._ready;
    }

    /** One `hidffb` message from the host. */
    apply(msg) {
        if (this._closed || !msg) return;
        const id = msg.effect | 0;
        const e = id ? this._effect(id) : null;
        switch (msg.op) {
            case 'effect':
                e.header = msg;
                if (!e.block && !e.creating) {
                    e.creating = true;
                    this._enqueue(`create:${id}`, () => this._create(e));
                } else {
                    this._enqueue(`effect:${id}`, () => this._send(e, 'effect'));
                }
                break;
            case 'envelope':
            case 'periodic':
            case 'constant':
            case 'ramp':
                e.params[msg.op] = msg;
                this._enqueue(`${msg.op}:${id}`, () => this._send(e, msg.op));
                break;
            case 'condition': {
                const axis = msg.axis | 0;
                e.params[`condition:${axis}`] = msg;
                this._enqueue(`condition:${axis}:${id}`, () => this._send(e, `condition:${axis}`));
                break;
            }
            case 'start':
            case 'solo':
            case 'stop':
                this._enqueue(`op:${id}`, () => this._operate(e, msg.op, msg.loops));
                break;
            case 'free':
                // Out of the table at once: the game may reuse the block for a
                // new effect before the wheel has answered.
                this._effects.delete(id);
                this._enqueue(`free:${id}:${++this._frees}`, () => this._free(e));
                break;
            case 'gain':
                this._enqueue('gain', () => this._gain(msg.gain | 0));
                break;
            case 'control':
                if (msg.kind === 'reset') this._effects.clear();
                if (CONTROLS[msg.kind])
                    this._enqueue(`control:${msg.kind}`, () => this._control(msg.kind));
                break;
            default:
                break;
        }
    }

    /** Leaves the wheel silent: every effect stopped and freed. */
    async close() {
        if (this._closed) return;
        this._closed = true;
        this._queue = [];
        try {
            await this._control('stopAll');
            await this._control('reset');
        } catch (err) {
            this._error(err);
        }
    }

    _effect(id) {
        let e = this._effects.get(id);
        if (!e) {
            e = { block: 0, creating: false, header: null, params: {} };
            this._effects.set(id, e);
        }
        return e;
    }

    _writer(usage) {
        const where = this._layout.ids.get(usage);
        const report = where && this._layout.reports.get(`${where.kind}:${where.id}`);
        return report ? new ReportWriter(report) : null;
    }

    _out(w) {
        return this._device.sendReport(w.report.id, w.body);
    }

    // Create New Effect, then Block Load read back: the wheel's own block.
    async _create(e) {
        e.creating = false;
        const type = EFFECT_TYPES[e.header?.kind];
        const create = this._writer(U.createNewEffect);
        const load = this._writer(U.blockLoad);
        if (!type || !create || !load) return;
        create.select(pid(type));
        await this._device.sendFeatureReport(create.report.id, create.body);
        const view = await this._read(load.report.id);
        let bytes = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
        // Chrome puts the report id first on some platforms.
        if (bytes.length === load.body.length + 1 && bytes[0] === load.report.id)
            bytes = bytes.subarray(1);
        load.body.set(bytes.subarray(0, load.body.length));
        if (load.selected(pid(U.loadSuccess)) !== pid(U.loadSuccess))
            throw new Error('the wheel has no free effect block');
        e.block = load.read(pid(U.blockIndex));
        if (!e.block) throw new Error('the wheel gave no effect block');
        // What the game sent before the block existed, then the header.
        for (const what of Object.keys(e.params)) await this._send(e, what);
        await this._send(e, 'effect');
        if (e.playing) await this._operate(e, e.playing.op, e.playing.loops);
    }

    _read(reportId) {
        return new Promise((resolve, reject) => {
            const timer = globalThis.setTimeout(
                () => reject(new Error('the wheel did not answer Block Load')),
                this._timeoutMs,
            );
            this._device.receiveFeatureReport(reportId).then(
                (v) => {
                    globalThis.clearTimeout(timer);
                    resolve(v);
                },
                (err) => {
                    globalThis.clearTimeout(timer);
                    reject(err);
                },
            );
        });
    }

    async _send(e, what) {
        if (!e.block) return;
        const w = this._report(e, what);
        if (w) await this._out(w);
    }

    // The output report for one kind of parameters, filled from the host's values.
    _report(e, what) {
        const P = pid;
        if (what === 'effect') {
            const h = e.header;
            const type = EFFECT_TYPES[h?.kind];
            const w = this._writer(U.setEffect);
            if (!w || !type) return null;
            w.raw(P(U.blockIndex), e.block).select(P(type));
            if (h.duration < 0) w.ones(P(U.duration));
            else w.ms(P(U.duration), h.duration);
            w.ms(P(U.startDelay), h.delay)
                .level(P(U.gain), h.gain ?? 255, 255)
                .ones(P(U.triggerButton));
            w.raw(P(U.directionEnable), h.directionEnable ? 1 : 0);
            w.raw((PAGE_DESKTOP << 16) | 0x30, (h.axes ?? 1) & 1);
            w.raw((PAGE_DESKTOP << 16) | 0x31, ((h.axes ?? 0) >> 1) & 1);
            w.angle((PAGE_ORDINAL << 16) | 1, h.direction || 0);
            // A wheel turns on one axis: the second angle stays at zero.
            return w;
        }
        const p = e.params[what];
        if (!p) return null;
        if (what === 'envelope') {
            const w = this._writer(U.setEnvelope);
            return w
                ?.raw(P(U.blockIndex), e.block)
                .level(P(U.attackLevel), p.attackLevel || 0)
                .level(P(U.fadeLevel), p.fadeLevel || 0)
                .ms(P(U.attackTime), p.attackTime)
                .ms(P(U.fadeTime), p.fadeTime);
        }
        if (what.startsWith('condition:')) {
            const w = this._writer(U.setCondition);
            return w
                ?.raw(P(U.blockIndex), e.block)
                .raw(P(U.paramBlockOffset), p.axis | 0)
                .signed(P(U.cpOffset), p.offset || 0)
                .signed(P(U.positiveCoefficient), p.positiveCoefficient || 0)
                .signed(P(U.negativeCoefficient), p.negativeCoefficient || 0)
                .level(P(U.positiveSaturation), p.positiveSaturation ?? 10000)
                .level(P(U.negativeSaturation), p.negativeSaturation ?? 10000)
                .level(P(U.deadBand), p.deadBand || 0);
        }
        if (what === 'periodic') {
            const w = this._writer(U.setPeriodic);
            return w
                ?.raw(P(U.blockIndex), e.block)
                .level(P(U.magnitude), p.magnitude || 0)
                .signed(P(U.offset), p.offset || 0)
                .angle(P(U.phase), p.phase || 0)
                .ms(P(U.period), p.period);
        }
        if (what === 'constant') {
            const w = this._writer(U.setConstant);
            return w?.raw(P(U.blockIndex), e.block).signed(P(U.magnitude), p.magnitude || 0);
        }
        if (what === 'ramp') {
            const w = this._writer(U.setRamp);
            return w
                ?.raw(P(U.blockIndex), e.block)
                .signed(P(U.rampStart), p.start || 0)
                .signed(P(U.rampEnd), p.end || 0);
        }
        return null;
    }

    async _operate(e, op, loops) {
        e.playing = op === 'stop' ? null : { op, loops };
        if (!e.block) return; // started once the block exists
        const w = this._writer(U.effectOperationReport);
        if (!w) return;
        const usage = op === 'start' ? U.opStart : op === 'solo' ? U.opSolo : U.opStop;
        w.raw(pid(U.blockIndex), e.block).select(pid(usage));
        w.raw(pid(U.loopCount), op === 'stop' ? 0 : (loops ?? 1));
        await this._out(w);
    }

    async _free(e) {
        if (!e.block) return;
        const w = this._writer(U.blockFree);
        if (!w) return;
        w.raw(pid(U.blockIndex), e.block);
        e.block = 0;
        await this._out(w);
    }

    async _gain(gain) {
        const w = this._writer(U.deviceGainReport);
        if (!w) return;
        w.level(pid(U.deviceGain), clamp(gain, 0, 255), 255);
        await this._out(w);
    }

    async _control(kind) {
        const w = this._writer(U.deviceControlReport);
        if (!w) return;
        w.select(pid(CONTROLS[kind]));
        await this._out(w);
    }

    // Keyed: a task already waiting under the same key is replaced in place,
    // keeping its turn, so the newest parameters go out and nothing piles up.
    _enqueue(key, run) {
        const waiting = this._queue.find((q) => q.key === key);
        if (waiting) waiting.run = run;
        else this._queue.push({ key, run });
        this._pump();
    }

    async _pump() {
        if (this._busy) return;
        this._busy = true;
        if (this._ready) await this._ready.catch(() => {});
        while (this._queue.length && !this._closed) {
            const { run } = this._queue.shift();
            try {
                await run();
            } catch (err) {
                this._error(err);
            }
        }
        this._busy = false;
    }

    _error(err) {
        if (this._onError) this._onError(err);
    }
}
