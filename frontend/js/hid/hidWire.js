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
 * The HID passthrough's wire (plan « Passthrough HID », P2): what the page
 * sends the host about a device it reads through WebHID.
 *
 * - Once per device, on the reliable `input` channel, `hidattach` with the
 *   device's identity and its `collections` as plain objects — the host
 *   rebuilds the report descriptor from them (HidDescriptor.cpp) and checks it
 *   before creating anything.
 * - Then each input report on the `hid` channel (id 4, unordered, no
 *   retransmission), binary: [slot u8][report id u8][seq u16 LE][bytes]. The
 *   host keeps only the newest per slot and report id.
 * - A report that has not changed is sent again every REPEAT_MS, so a lost
 *   datagram never leaves an axis where it was: an absolute report carries the
 *   whole state, the next one repairs any loss.
 */

export const HID_CHANNEL_LABEL = 'hid';
export const HID_CHANNEL_ID = 4;
export const HID_FRAME_HEADER = 4;
export const HID_MAX_SLOTS = 8;
export const REPEAT_MS = 500;

// Usages a game device declares at the top of its report descriptor: the
// chooser's filters, and what the host accepts as a game collection.
export const GAME_FILTERS = [
    { usagePage: 0x01, usage: 0x04 }, // Joystick
    { usagePage: 0x01, usage: 0x05 }, // Game Pad
    { usagePage: 0x01, usage: 0x08 }, // Multi-axis Controller
    { usagePage: 0x02 }, // Simulation Controls (wheels, pedals)
];

const ITEM_FIELDS = [
    'isAbsolute',
    'isArray',
    'isBufferedBytes',
    'isConstant',
    'isLinear',
    'isRange',
    'isVolatile',
    'hasNull',
    'hasPreferredState',
    'wrap',
    'usages',
    'usageMinimum',
    'usageMaximum',
    'reportSize',
    'reportCount',
    'unitExponent',
    'unitSystem',
    'unitFactorLengthExponent',
    'unitFactorMassExponent',
    'unitFactorTimeExponent',
    'unitFactorTemperatureExponent',
    'unitFactorCurrentExponent',
    'unitFactorLuminousIntensityExponent',
    'logicalMinimum',
    'logicalMaximum',
    'physicalMinimum',
    'physicalMaximum',
];

// WebHID's objects keep their fields on the prototype: JSON.stringify sees none.
function plainItem(item) {
    const o = {};
    for (const k of ITEM_FIELDS) {
        const v = item[k];
        if (v === undefined) continue;
        o[k] = Array.isArray(v) ? [...v] : v;
    }
    return o;
}

function plainReport(r) {
    return { reportId: r.reportId, items: (r.items || []).map(plainItem) };
}

/** A HIDCollectionInfo as a plain object, children included. */
export function plainCollection(c) {
    return {
        usagePage: c.usagePage,
        usage: c.usage,
        type: c.type,
        children: (c.children || []).map(plainCollection),
        inputReports: (c.inputReports || []).map(plainReport),
        outputReports: (c.outputReports || []).map(plainReport),
        featureReports: (c.featureReports || []).map(plainReport),
    };
}

/** True when one of the device's top-level collections is a game one. */
export function isGameDevice(collections) {
    return (collections || []).some((c) =>
        GAME_FILTERS.some(
            (f) => f.usagePage === c.usagePage && (f.usage === undefined || f.usage === c.usage),
        ),
    );
}

/**
 * Chrome's chooser narrowed to one model's game interfaces: the row of a
 * wheel in Settings, or the stream's offer, lists that wheel and nothing else
 * (not its vendor-only interface either).
 */
export function modelFilters(vendorId, productId) {
    return GAME_FILTERS.map((f) => ({ vendorId, productId, ...f }));
}

/** The `hidattach` message for a device opened in this page. */
export function attachMessage(slot, device, { forceFeedback = false } = {}) {
    const m = {
        type: 'hidattach',
        slot,
        vendorId: device.vendorId,
        productId: device.productId,
        productName: device.productName || '',
        collections: (device.collections || []).map(plainCollection),
    };
    // The page can play force feedback on this device: the host gives the
    // recreated one a PID block and sends what games ask as hidffb.
    if (forceFeedback) m.forceFeedback = true;
    return m;
}

export function detachMessage(slot) {
    return { type: 'hiddetach', slot };
}

/** One input report as the `hid` channel carries it. */
export function encodeReportFrame(slot, reportId, seq, bytes) {
    const frame = new Uint8Array(HID_FRAME_HEADER + bytes.length);
    frame[0] = slot & 0xff;
    frame[1] = reportId & 0xff;
    frame[2] = seq & 0xff;
    frame[3] = (seq >> 8) & 0xff;
    frame.set(bytes, HID_FRAME_HEADER);
    return frame;
}

/** The reverse, or null for a frame too short to hold the header. */
export function decodeReportFrame(frame) {
    const b = frame instanceof Uint8Array ? frame : new Uint8Array(frame);
    if (b.length < HID_FRAME_HEADER) return null;
    return {
        slot: b[0],
        reportId: b[1],
        seq: b[2] | (b[3] << 8),
        bytes: b.subarray(HID_FRAME_HEADER),
    };
}

/**
 * Numbers each report and repeats the last one of every (slot, report id)
 * that stayed unchanged for REPEAT_MS. The caller sends what it returns;
 * `now` is injected so the policy is testable.
 */
export class HidReportPump {
    constructor(repeatMs = REPEAT_MS) {
        this._repeatMs = repeatMs;
        this._seq = new Map(); // slot -> next seq
        this._last = new Map(); // `${slot}:${id}` -> { slot, reportId, bytes, at }
    }

    /** A report just read from the device: its frame. */
    report(slot, reportId, bytes, now) {
        this._last.set(`${slot}:${reportId}`, {
            slot,
            reportId,
            bytes: Uint8Array.from(bytes),
            at: now,
        });
        return this._frame(slot, reportId, bytes);
    }

    /** Frames to send again: reports unchanged for at least REPEAT_MS. */
    due(now) {
        const out = [];
        for (const r of this._last.values()) {
            if (now - r.at < this._repeatMs) continue;
            r.at = now;
            out.push(this._frame(r.slot, r.reportId, r.bytes));
        }
        return out;
    }

    /** The device left: nothing of it is repeated any more. */
    forget(slot) {
        for (const [k, r] of this._last) if (r.slot === slot) this._last.delete(k);
        this._seq.delete(slot);
    }

    _frame(slot, reportId, bytes) {
        const seq = this._seq.get(slot) || 0;
        this._seq.set(slot, (seq + 1) & 0xffff);
        return encodeReportFrame(slot, reportId, seq, bytes);
    }
}

/**
 * The 'hid' channel on a transport's peer connection: negotiated (no SDP of
 * its own), unordered, never retransmitted. Not one of the channels the
 * connection waits for — a host without it simply drops what lands on id 4.
 */
export function createHidChannel(pc) {
    try {
        const dc = pc.createDataChannel(HID_CHANNEL_LABEL, {
            negotiated: true,
            id: HID_CHANNEL_ID,
            ordered: false,
            maxRetransmits: 0,
        });
        dc.binaryType = 'arraybuffer';
        return dc;
    } catch {
        return null;
    }
}

/** Sends one frame when the channel is open; false otherwise. */
export function sendHidFrame(dc, frame) {
    if (!dc || dc.readyState !== 'open') return false;
    try {
        dc.send(frame);
        return true;
    } catch {
        return false;
    }
}

export function closeHidChannel(dc) {
    try {
        if (dc && dc.readyState !== 'closed') dc.close();
    } catch {
        /* closing anyway */
    }
}
