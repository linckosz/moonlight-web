/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

import { describe, expect, it } from 'vitest';
import { PidFfbPlayer, hasNativePid, reportLayout } from '../js/hid/pidFfb.js';
import { HidPassthrough } from '../js/hid/HidPassthrough.js';

const P = (u) => 0x000f0000 | u;
const D = (u) => 0x00010000 | u;
const O = (u) => 0x000a0000 | u;

const item = (usages, size, count, min, max, extra = {}) => ({
    usages,
    reportSize: size,
    reportCount: count,
    logicalMinimum: min,
    logicalMaximum: max,
    isArray: false,
    isConstant: false,
    isRange: false,
    ...extra,
});
const array = (usages) => item(usages, 8, 1, 1, usages.length, { isArray: true });
const pad = (bits) => item([], bits, 1, 0, 0, { isConstant: true });
const ms = { unitExponent: -3, unitFactorTimeExponent: 1 };

const TYPES = [0x26, 0x27, 0x30, 0x31, 0x32, 0x33, 0x34, 0x40, 0x41, 0x42, 0x43].map(P);
const INDEX = item([P(0x22)], 8, 1, 1, 40);

// A collection as its descriptor declares it: items of one report and nested
// collections, in order.
const node = (usage, entries, usagePage = 0x0f, type = 2) => ({ usage, usagePage, type, entries });
const at = (kind, id, ...items) => items.map((it) => ({ kind, id, item: it }));
const LISTS = { in: 'inputReports', out: 'outputReports', feat: 'featureReports' };
const subtree = (n) => n.entries.flat().flatMap((e) => (e.entries ? subtree(e) : [e]));

/**
 * What Chrome on macOS hands a page (Chromium's own descriptor parser): each
 * item in its collection and in every ancestor, in descriptor order.
 */
function onMac(n) {
    const c = {
        usagePage: n.usagePage,
        usage: n.usage,
        type: n.type,
        children: n.entries
            .flat()
            .filter((e) => e.entries)
            .map(onMac),
        inputReports: [],
        outputReports: [],
        featureReports: [],
    };
    for (const { kind, id, item: it } of subtree(n)) {
        const list = c[LISTS[kind]];
        let r = list.find((x) => x.reportId === id);
        if (!r) list.push((r = { reportId: id, items: [] }));
        r.items.push(it);
    }
    return c;
}

/** What Chrome on Windows hands a page: one flat collection, every report whole. */
const onWindows = (n) => ({ ...onMac(n), children: [] });

/**
 * A PID wheel laid out as the USB PID spec's example descriptor. Set Effect
 * (id 1) nests Effect Type, Axes Enable and Direction between its own fields;
 * forces are -255..255 and times in 1/10 ms on purpose.
 */
function pidWheel() {
    const setEffect = node(0x21, [
        at('out', 1, INDEX),
        node(0x25, [at('out', 1, array(TYPES))]),
        at(
            'out',
            1,
            item([P(0x50), P(0x54), P(0x51)], 16, 3, 0, 0x7fff, {
                unitExponent: -4,
                unitFactorTimeExponent: 1,
            }),
            item([P(0x52)], 8, 1, 0, 255),
            item([P(0x53)], 8, 1, 1, 8),
        ),
        node(0x55, [at('out', 1, item([D(0x30), D(0x31)], 1, 2, 0, 1))]),
        at('out', 1, item([P(0x56)], 1, 1, 0, 1), pad(5)),
        node(0x57, [at('out', 1, item([O(1), O(2)], 8, 2, 0, 255))]),
        at('out', 1, item([P(0xa7)], 16, 1, 0, 0x7fff, ms)),
    ]);
    const setConstant = node(0x73, [at('out', 5, INDEX, item([P(0x70)], 16, 1, -255, 255))]);
    const setCondition = node(0x5f, [
        at(
            'out',
            3,
            INDEX,
            item([P(0x23)], 4, 1, 0, 1),
            pad(4),
            item([P(0x60), P(0x61), P(0x62)], 8, 3, -128, 127),
            item([P(0x63), P(0x64), P(0x65)], 8, 3, 0, 255),
        ),
    ]);
    const operation = node(0x77, [
        at('out', 10, INDEX),
        node(0x78, [at('out', 10, array([P(0x79), P(0x7a), P(0x7b)]))]),
        at('out', 10, item([P(0x7c)], 8, 1, 0, 255)),
    ]);
    const blockFree = node(0x90, [at('out', 11, INDEX)]);
    const control = node(0x95, [
        node(0x96, [at('out', 12, array([0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c].map(P)))]),
    ]);
    const gain = node(0x7d, [at('out', 13, item([P(0x7e)], 8, 1, 0, 255))]);
    const create = node(0xab, [
        node(0x25, [at('feat', 17, array(TYPES))]),
        at('feat', 17, item([D(0x3b)], 16, 1, 0, 511)),
    ]);
    const load = node(0x89, [
        at('feat', 18, INDEX),
        node(0x8b, [at('feat', 18, array([P(0x8c), P(0x8d), P(0x8e)]))]),
        at('feat', 18, item([P(0xac)], 16, 1, 0, 0xffff)),
    ]);
    return node(
        4,
        [
            at('in', 2, item([D(0x30)], 16, 1, 0, 65535)),
            setEffect,
            setConstant,
            setCondition,
            operation,
            blockFree,
            control,
            gain,
            create,
            load,
        ],
        1,
        1,
    );
}

function fakePidWheel({ block = 5, status = 1 } = {}) {
    const out = [];
    const features = [];
    return {
        vendorId: 0x346e,
        productId: 0x0006,
        productName: 'PID Wheel',
        opened: false,
        collections: [onMac(pidWheel())],
        out,
        features,
        async open() {
            this.opened = true;
        },
        addEventListener() {},
        removeEventListener() {},
        async sendReport(id, body) {
            out.push({ id, body: Array.from(body) });
        },
        async sendFeatureReport(id, body) {
            features.push({ id, body: Array.from(body) });
        },
        async receiveFeatureReport(id) {
            // Block Load, its id first as Chrome gives it on Windows.
            const b = [id, block, status, 0x00, 0x10];
            return new DataView(Uint8Array.from(b).buffer);
        },
    };
}

const settle = () => new Promise((r) => globalThis.setTimeout(r, 0));

describe('pidFfb reportLayout', () => {
    it('reads each report whole from the top-level collection, each field once', () => {
        const { reports, ids } = reportLayout([onMac(pidWheel())]);
        const set = reports.get('outputReports:1');
        expect(set.bits).toBe(120);
        const offset = (usage) => set.fields.find((f) => f.usages.includes(usage)).offset;
        expect(offset(P(0x22))).toBe(0);
        expect(offset(P(0x26))).toBe(8); // Effect Type, a child
        expect(offset(P(0x50))).toBe(16);
        expect(offset(P(0x52))).toBe(64);
        expect(offset(P(0x53))).toBe(72);
        expect(offset(D(0x30))).toBe(80); // Axes Enable, a child
        expect(offset(P(0x56))).toBe(82);
        expect(offset(O(1))).toBe(88); // Direction, a child after the padding
        expect(offset(P(0xa7))).toBe(104);
        expect(ids.get(0x21)).toEqual({ kind: 'outputReports', id: 1 });
        expect(ids.get(0x89)).toEqual({ kind: 'featureReports', id: 18 });
        expect(reports.get('featureReports:18').bits).toBe(32);
        expect(reports.get('inputReports:2').bits).toBe(16);
    });

    it('lays a tree that lists only its own items out child after parent', () => {
        const child = onWindows(node(0x01, [at('in', 0, item([D(0x30)], 8, 1, 0, 255))], 1, 0));
        const top = { ...onWindows(node(5, [], 1, 1)), children: [child] };
        expect(reportLayout([top]).reports.get('inputReports:0').bits).toBe(8);
    });

    it('takes a one-usage range for its usage', () => {
        const one = item([], 1, 1, 0, 1, { usageMinimum: 0x90001, usageMaximum: 0x90001 });
        const pad7 = pad(7);
        const { reports } = reportLayout([onMac(node(5, [at('in', 0, one, pad7)], 1, 1))]);
        expect(reports.get('inputReports:0').fields[0].usages).toEqual([0x90001]);
    });

    it('tells a PID wheel from a wheel without PID, and finds none from Windows', () => {
        expect(hasNativePid({ collections: [onMac(pidWheel())] })).toBe(true);
        expect(hasNativePid({ collections: [onWindows(pidWheel())] })).toBe(false);
        expect(hasNativePid({ collections: [onMac(pidWheel()).children[0]] })).toBe(false);
        expect(hasNativePid({ collections: [] })).toBe(false);
    });
});

describe('PidFfbPlayer', () => {
    it('resets, enables and sets full gain at start', async () => {
        const dev = fakePidWheel();
        const p = new PidFfbPlayer(dev);
        await p.init();
        expect(dev.out).toEqual([
            { id: 12, body: [4] }, // reset
            { id: 12, body: [1] }, // enable actuators
            { id: 13, body: [255] },
        ]);
    });

    it('creates the effect on the wheel, then sends its parameters, header and start', async () => {
        const dev = fakePidWheel({ block: 7 });
        const errors = [];
        const p = new PidFfbPlayer(dev, { onError: (e) => errors.push(e) });
        // pid.dll's order: the type-specific block before the header.
        p.apply({ op: 'constant', effect: 1, magnitude: -5000 });
        p.apply({
            op: 'effect',
            effect: 1,
            kind: 'constant',
            duration: -1,
            delay: 20,
            gain: 255,
            direction: 9000,
            directionEnable: 1,
            axes: 1,
        });
        p.apply({ op: 'start', effect: 1, loops: 1 });
        await settle();
        await settle();
        expect(errors).toEqual([]);
        expect(dev.features).toEqual([{ id: 17, body: [1, 0, 0] }]); // constant = type 1
        const [constant, header, start] = dev.out;
        expect(constant).toEqual({ id: 5, body: [7, ...le16(-127)] }); // -5000 of ±10000 → -127.5 of ±255
        expect(header.id).toBe(1);
        const b = header.body;
        expect(b[0]).toBe(7); // the wheel's block, not the game's
        expect(b[1]).toBe(1); // constant
        expect(b[2] | (b[3] << 8)).toBe(0xffff); // infinite
        expect(b[8]).toBe(255); // gain
        expect(b[10] & 0x07).toBe(0b101); // X enabled, Y not, direction enabled
        expect(b[11]).toBe(64); // 90° of 0..255
        expect(b[13] | (b[14] << 8)).toBe(20); // start delay, ms
        expect(start).toEqual({ id: 10, body: [7, 1, 1] });
    });

    it('rescales times to the wheel unit and conditions to its bounds', async () => {
        const dev = fakePidWheel({ block: 2 });
        const p = new PidFfbPlayer(dev);
        p.apply({ op: 'effect', effect: 3, kind: 'spring', duration: 100, delay: 0, gain: 255 });
        p.apply({
            op: 'condition',
            effect: 3,
            axis: 0,
            offset: 0,
            positiveCoefficient: 10000,
            negativeCoefficient: -10000,
            positiveSaturation: 10000,
            negativeSaturation: 5000,
            deadBand: 0,
        });
        await settle();
        await settle();
        const header = dev.out.find((o) => o.id === 1).body;
        expect(header[2] | (header[3] << 8)).toBe(1000); // 100 ms in 1/10 ms
        const cond = dev.out.find((o) => o.id === 3).body;
        expect(cond).toEqual([2, 0, 0, 127, 0x81, 255, 128, 0]);
    });

    it('frees the wheel block, and a refused block is reported', async () => {
        const dev = fakePidWheel({ block: 4 });
        const p = new PidFfbPlayer(dev);
        p.apply({ op: 'effect', effect: 1, kind: 'sine', duration: 10, gain: 255 });
        p.apply({ op: 'free', effect: 1 });
        await settle();
        await settle();
        expect(dev.out.at(-1)).toEqual({ id: 11, body: [4] });

        const full = fakePidWheel({ status: 2 });
        const errors = [];
        const q = new PidFfbPlayer(full, { onError: (e) => errors.push(e.message) });
        q.apply({ op: 'effect', effect: 1, kind: 'sine', duration: 10, gain: 255 });
        q.apply({ op: 'start', effect: 1 });
        await settle();
        await settle();
        expect(errors).toEqual(['the wheel has no free effect block']);
        expect(full.out).toEqual([]); // nothing sent for a block it never gave
    });

    it('stops and resets the wheel on close', async () => {
        const dev = fakePidWheel();
        const p = new PidFfbPlayer(dev);
        await p.close();
        expect(dev.out).toEqual([
            { id: 12, body: [3] }, // stop all
            { id: 12, body: [4] }, // reset
        ]);
        p.apply({ op: 'gain', gain: 10 });
        await settle();
        expect(dev.out.length).toBe(2);
    });
});

describe('HidPassthrough with a PID wheel', () => {
    it('asks the host for force feedback and plays hidffb on the wheel', async () => {
        const dev = fakePidWheel({ block: 9 });
        const sent = [];
        const hid = {
            addEventListener() {},
            removeEventListener() {},
            getDevices: async () => [dev],
        };
        const storage = new Map([['mw_hid_forward', JSON.stringify(['346e:0006'])]]);
        const hp = new HidPassthrough({
            hid,
            send: (m) => sent.push(m),
            sendFrame: () => true,
            storage: {
                getItem: (k) => storage.get(k) ?? null,
                setItem: (k, v) => storage.set(k, v),
            },
            setTimer: () => 1,
            clearTimer: () => {},
        });
        hp.handleMessage({ type: 'hidcaps', available: true, ffb: true });
        await settle();
        await settle();
        const attach = sent.find((m) => m.type === 'hidattach');
        expect(attach.forceFeedback).toBe(true);
        hp.handleMessage({ type: 'hidattached', slot: attach.slot, ok: true });
        await settle();
        hp.handleMessage({
            type: 'hidffb',
            slot: attach.slot,
            op: 'effect',
            effect: 1,
            kind: 'constant',
            duration: -1,
            gain: 255,
        });
        await settle();
        await settle();
        expect(dev.features).toEqual([{ id: 17, body: [1, 0, 0] }]);
        expect(dev.out.some((o) => o.id === 1 && o.body[0] === 9)).toBe(true);
        hp.stop();
        await settle();
        await settle();
        expect(dev.out.at(-1)).toEqual({ id: 12, body: [4] }); // reset when it stops
    });
});

function le16(v) {
    const n = v < 0 ? v + 0x10000 : v;
    return [n & 0xff, n >> 8];
}
