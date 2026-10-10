/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

import { describe, expect, it, vi } from 'vitest';
import {
    FORWARD_KEY,
    HID_KINDS,
    HidPassthrough,
    MAX_OFFERS,
    deviceKey,
    hidModelOf,
    linkModel,
    loadForwarded,
    padKeyOf,
    saveForwarded,
} from '../js/hid/HidPassthrough.js';
import { decodeReportFrame, modelFilters } from '../js/hid/hidWire.js';

function fakeDevice({
    vendorId = 0x046d,
    productId = 0xc26e,
    usagePage = 1,
    usage = 4,
    openFails = false,
} = {}) {
    const listeners = new Set();
    return {
        vendorId,
        productId,
        productName: 'G923',
        opened: false,
        collections: [
            {
                usagePage,
                usage,
                type: 1,
                children: [],
                inputReports: [],
                outputReports: [],
                featureReports: [],
            },
        ],
        async open() {
            if (openFails) throw new Error('busy');
            this.opened = true;
        },
        addEventListener: (t, fn) => t === 'inputreport' && listeners.add(fn),
        removeEventListener: (t, fn) => t === 'inputreport' && listeners.delete(fn),
        report(reportId, bytes) {
            const data = new DataView(Uint8Array.from(bytes).buffer);
            for (const fn of listeners) fn({ reportId, data });
        },
        listening: () => listeners.size,
    };
}

function fakeHid(devices) {
    const listeners = new Set();
    return {
        getDevices: async () => devices,
        requestDevice: vi.fn(async () => devices.slice(0, 1)),
        addEventListener: (t, fn) => t === 'disconnect' && listeners.add(fn),
        removeEventListener: (t, fn) => t === 'disconnect' && listeners.delete(fn),
        unplug: (device) => listeners.forEach((fn) => fn({ device })),
    };
}

function memoryStorage(initial = {}) {
    const m = new Map(Object.entries(initial));
    return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, v) };
}

function setup(devices, wanted = []) {
    const hid = fakeHid(devices);
    const sent = [];
    const frames = [];
    let tick = null;
    let now = 0;
    const storage = memoryStorage({ [FORWARD_KEY]: JSON.stringify(wanted) });
    const onChange = vi.fn();
    const onResult = vi.fn();
    const hp = new HidPassthrough({
        hid,
        send: (m) => sent.push(m),
        sendFrame: (f) => frames.push(f) && true,
        storage,
        now: () => now,
        setTimer: (fn) => (tick = fn),
        clearTimer: () => (tick = null),
        onChange,
        onResult,
    });
    return {
        hp,
        onResult,
        hid,
        sent,
        frames,
        storage,
        onChange,
        advance: (ms) => {
            now += ms;
            if (tick) tick();
        },
        ticking: () => !!tick,
    };
}

describe('HidPassthrough (HID passthrough in the stream, P2)', () => {
    it('remembers switched-on devices by vid:pid, and maps them to pad keys', () => {
        const d = fakeDevice();
        expect(deviceKey(d)).toBe('046d:c26e');
        expect(padKeyOf(d)).toBe('usb:046d:c26e');
        const storage = memoryStorage();
        saveForwarded(new Set(['046d:c26e']), storage);
        expect(Array.from(loadForwarded(storage))).toEqual(['046d:c26e']);
        expect(loadForwarded(memoryStorage({ [FORWARD_KEY]: 'not json' })).size).toBe(0);
        expect(
            loadForwarded({
                getItem: () => {
                    throw new Error('blocked');
                },
            }).size,
        ).toBe(0);
        saveForwarded(new Set(), {
            setItem: () => {
                throw new Error('blocked');
            },
        }); // no throw
    });

    it('attaches the wanted devices once the host can, then streams their reports', async () => {
        const wheel = fakeDevice();
        const t = setup([wheel], ['046d:c26e']);
        expect(t.hp.supported).toBe(true);
        t.hp.handleMessage({ type: 'hidcaps', available: true, why: '' });
        await vi.waitFor(() => expect(t.sent).toHaveLength(1));
        expect(t.sent[0]).toMatchObject({
            type: 'hidattach',
            slot: 0,
            vendorId: 0x046d,
            productId: 0xc26e,
        });
        expect(wheel.opened).toBe(true);
        expect(t.hp.excludedKeys()).toEqual(['usb:046d:c26e']);

        t.hp.handleMessage({ type: 'hidattached', slot: 0, ok: true });
        expect((await t.hp.devices())[0]).toMatchObject({
            key: '046d:c26e',
            wanted: true,
            state: 'on',
        });

        wheel.report(1, [8, 0, 0x80]);
        expect(t.frames).toHaveLength(1);
        expect(decodeReportFrame(t.frames[0])).toMatchObject({ slot: 0, reportId: 1, seq: 0 });
        // An unchanged report goes again after 500 ms.
        t.advance(400);
        expect(t.frames).toHaveLength(1);
        t.advance(100);
        expect(t.frames).toHaveLength(2);
    });

    it('does nothing for a host that cannot, and lets go when it says it no longer can', async () => {
        const wheel = fakeDevice();
        const t = setup([wheel], ['046d:c26e']);
        t.hp.handleMessage({ type: 'hidcaps', available: false, why: 'needs the native host' });
        await Promise.resolve();
        expect(t.sent).toHaveLength(0);
        expect(t.hp.caps).toEqual({
            available: false,
            ffb: false,
            hidpp: false,
            why: 'needs the native host',
        });

        t.hp.handleMessage({ type: 'hidcaps', available: true });
        await vi.waitFor(() => expect(t.sent).toHaveLength(1));
        t.hp.handleMessage({ type: 'hidcaps', available: false, why: 'gone' });
        expect(t.sent[1]).toEqual({ type: 'hiddetach', slot: 0 });
        expect(wheel.listening()).toBe(0);
        expect(t.ticking()).toBe(false);
    });

    it('shows a refusal by the device, and frees its slot', async () => {
        const wheel = fakeDevice();
        const t = setup([wheel], ['046d:c26e']);
        t.hp.handleMessage({ type: 'hidcaps', available: true });
        await vi.waitFor(() => expect(t.sent).toHaveLength(1));
        t.hp.handleMessage({ type: 'hidattached', slot: 0, ok: false, why: 'not a game device' });
        expect(t.onResult).toHaveBeenCalledWith(wheel, false, 'not a game device');
        const [d] = await t.hp.devices();
        expect(d).toMatchObject({ state: 'refused', why: 'not a game device' });
        expect(t.hp.excludedKeys()).toEqual([]);
        expect(wheel.listening()).toBe(0);
        expect(t.hp.handleMessage({ type: 'hidattached', slot: 5, ok: true })).toBe(true); // unknown slot
    });

    it('switches a device on and off for this viewer', async () => {
        const wheel = fakeDevice();
        const radio = fakeDevice({ vendorId: 0x1209, productId: 0x4f54, usage: 5 });
        const t = setup([wheel, radio]);
        t.hp.handleMessage({ type: 'hidcaps', available: true });
        await Promise.resolve();
        expect(t.sent).toHaveLength(0);
        await t.hp.setWanted(radio, true);
        expect(t.sent[0]).toMatchObject({ type: 'hidattach', slot: 0, vendorId: 0x1209 });
        await t.hp.setWanted(wheel, true);
        expect(t.sent[1]).toMatchObject({ type: 'hidattach', slot: 1 });
        expect(Array.from(loadForwarded(t.storage))).toEqual(['1209:4f54', '046d:c26e']);
        await t.hp.setWanted(radio, false);
        expect(t.sent[2]).toEqual({ type: 'hiddetach', slot: 0 });
        expect(Array.from(loadForwarded(t.storage))).toEqual(['046d:c26e']);
        expect(t.onChange).toHaveBeenCalled();
    });

    it('drops an unplugged device, keeps keyboards out, and survives a device it cannot open', async () => {
        const wheel = fakeDevice();
        const keyboard = fakeDevice({ vendorId: 1, productId: 2, usage: 6 });
        const busy = fakeDevice({ vendorId: 3, productId: 4, openFails: true });
        const t = setup([wheel, keyboard, busy], ['046d:c26e', '0003:0004', '0001:0002']);
        expect((await t.hp.devices()).map((d) => d.key)).toEqual(['046d:c26e', '0003:0004']);
        t.hp.handleMessage({ type: 'hidcaps', available: true });
        await vi.waitFor(() => expect(t.sent).toHaveLength(1));
        const states = await t.hp.devices();
        expect(states[1]).toMatchObject({ state: 'refused', why: 'busy' });
        t.hid.unplug(wheel);
        expect(t.sent[1]).toEqual({ type: 'hiddetach', slot: 0 });
        expect((await t.hp.devices())[0]).toMatchObject({ state: 'off', why: 'unplugged' });
    });

    it('counts host requests, ignores other messages, opens the chooser and stops cleanly', async () => {
        const wheel = fakeDevice();
        const t = setup([wheel], ['046d:c26e']);
        expect(t.hp.handleMessage({ type: 'hidrequest', slot: 0, kind: 'output' })).toBe(true);
        expect(t.hp.requests).toBe(1);
        expect(t.hp.handleMessage({ type: 'rumble' })).toBe(false);
        expect(t.hp.handleMessage(null)).toBe(false);
        expect(await t.hp.choose()).toEqual([wheel]);
        t.hp.handleMessage({ type: 'hidcaps', available: true });
        await vi.waitFor(() => expect(t.sent).toHaveLength(1));
        t.hp.stop();
        expect(t.sent[1]).toEqual({ type: 'hiddetach', slot: 0 });
    });

    it('knows which pads a row offers « as itself », by kind and USB ids', () => {
        const id = (name, vid, pid) => `${name} (STANDARD GAMEPAD Vendor: ${vid} Product: ${pid})`;
        expect(hidModelOf({ id: id('G923 Racing Wheel', '046d', 'c26e') })).toEqual({
            key: '046d:c26e',
            vendorId: 0x046d,
            productId: 0xc26e,
        });
        expect(hidModelOf({ id: id('Radiomaster TX12 Joystick', '1209', '4f54') })?.key).toBe(
            '1209:4f54',
        );
        // A plain pad, unless the viewer said what it is; no ids, no offer.
        expect(hidModelOf({ id: id('Xbox Wireless Controller', '045e', '0b13') })).toBeNull();
        expect(
            hidModelOf({ id: id('Xbox Wireless Controller', '045e', '0b13') }, 'wheel'),
        ).not.toBeNull();
        expect(hidModelOf({ id: 'Racing Wheel' })).toBeNull();
        expect(HID_KINDS).toEqual(['wheel', 'rc', 'flightstick']);
    });

    it('narrows the chooser to one model, game interfaces only, and switches it on', async () => {
        expect(modelFilters(0x046d, 0xc26e)).toContainEqual({
            vendorId: 0x046d,
            productId: 0xc26e,
            usagePage: 0x01,
            usage: 0x04,
        });
        const wheel = fakeDevice();
        const vendorOnly = fakeDevice({ usagePage: 0xfffd, usage: 0xfd01 });
        const hid = fakeHid([wheel]);
        hid.requestDevice.mockResolvedValueOnce([wheel, vendorOnly]);
        const storage = memoryStorage();
        expect(await linkModel(hid, 0x046d, 0xc26e, storage)).toEqual([wheel]);
        expect(hid.requestDevice).toHaveBeenCalledWith({ filters: modelFilters(0x046d, 0xc26e) });
        expect(Array.from(loadForwarded(storage))).toEqual(['046d:c26e']);
        // Closed without a choice, or no WebHID: nothing switched on.
        hid.requestDevice.mockRejectedValueOnce(new Error('no gesture'));
        expect(await linkModel(hid, 1, 2, memoryStorage())).toEqual([]);
        expect(await linkModel(null, 1, 2)).toEqual([]);
    });

    it('links a model from the stream and sends it at once', async () => {
        const wheel = fakeDevice();
        const t = setup([wheel]);
        t.hp.handleMessage({ type: 'hidcaps', available: true });
        await Promise.resolve();
        expect(t.sent).toHaveLength(0);
        expect(await t.hp.link(0x046d, 0xc26e)).toBe(true);
        expect(t.sent[0]).toMatchObject({ type: 'hidattach', vendorId: 0x046d });
        expect(Array.from(loadForwarded(t.storage))).toEqual(['046d:c26e']);
        t.hid.requestDevice.mockResolvedValueOnce([]);
        expect(await t.hp.link(0x046d, 0xc26e)).toBe(false);
    });

    it('offers a wheel never linked, once a stream and three streams at most', async () => {
        const wheelPad = { id: 'G923 (STANDARD GAMEPAD Vendor: 046d Product: c26e)' };
        const pad = { id: 'Xbox (STANDARD GAMEPAD Vendor: 045e Product: 0b13)' };
        const radioPad = { id: 'TX12 (STANDARD GAMEPAD Vendor: 1209 Product: 4f54)' };
        const radio = fakeDevice({ vendorId: 0x1209, productId: 0x4f54, usage: 5 });
        // The radio is allowed already (switched off by the viewer): never offered.
        const t = setup([radio]);
        expect(await t.hp.offers([wheelPad])).toEqual([]); // host has not said yet
        t.hp.handleMessage({ type: 'hidcaps', available: true });
        const first = await t.hp.offers([wheelPad, pad, radioPad, null]);
        expect(first).toEqual([
            { key: '046d:c26e', vendorId: 0x046d, productId: 0xc26e, name: 'G923' },
        ]);
        expect(await t.hp.offers([wheelPad])).toEqual([]); // once a stream

        const streamAgain = () => {
            const hp = new HidPassthrough({
                hid: t.hid,
                send: () => {},
                sendFrame: () => true,
                storage: t.storage,
            });
            hp.handleMessage({ type: 'hidcaps', available: true });
            return hp.offers([wheelPad]);
        };
        expect(await streamAgain()).toHaveLength(1);
        expect(await streamAgain()).toHaveLength(1);
        expect(MAX_OFFERS).toBe(3);
        expect(await streamAgain()).toEqual([]);
    });

    it('says WebHID is missing rather than failing', async () => {
        const hp = new HidPassthrough({
            hid: null,
            send: () => {},
            sendFrame: () => true,
            storage: memoryStorage(),
        });
        expect(hp.supported).toBe(false);
        expect(await hp.devices()).toEqual([]);
        expect(await hp.choose()).toEqual([]);
        expect(await hp.offers([{ id: 'G923 (Vendor: 046d Product: c26e)' }])).toEqual([]);
        expect(await hp.link(0x046d, 0xc26e)).toBe(false);
        await hp.applyWanted();
        hp.stop();
    });
});
