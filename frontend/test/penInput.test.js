/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import {
    MIN_CONTACT_PRESSURE,
    PEN_BUTTON_PRIMARY,
    PEN_BUTTON_SECONDARY,
    PenStroke,
    penAngles,
    penButtons,
} from '../js/stream/PenInput.js';

const DEG = Math.PI / 180;

/** The browser's tiltX/tiltY for a pen leaning `tilt` degrees, pointing `rotation`. */
function tiltsOf(tilt, rotation) {
    const t = Math.tan(tilt * DEG);
    return {
        tiltX: Math.atan(-Math.sin(rotation * DEG) * t) / DEG,
        tiltY: Math.atan(Math.cos(rotation * DEG) * t) / DEG,
    };
}

describe('the pen packet tilt, from a pointer sample', () => {
    it('says nothing for a pen that reports no tilt, or stands upright', () => {
        expect(penAngles({})).toBeNull();
        expect(penAngles({ tiltX: 0, tiltY: 0 })).toBeNull();
        expect(
            penAngles({ tiltX: 0, tiltY: 0, altitudeAngle: Math.PI / 2, azimuthAngle: 0 }),
        ).toBeNull();
    });

    it('points up the screen for a pen leaning towards the viewer, and turns clockwise', () => {
        expect(penAngles({ tiltX: 0, tiltY: 30 })).toEqual({ tilt: 30, rotation: 0 });
        // Leaning left, the tip points right: a quarter turn.
        expect(penAngles({ tiltX: -30, tiltY: 0 })).toEqual({ tilt: 30, rotation: 90 });
        expect(penAngles({ tiltX: 0, tiltY: -30 })).toEqual({ tilt: 30, rotation: 180 });
        expect(penAngles({ tiltX: 30, tiltY: 0 })).toEqual({ tilt: 30, rotation: 270 });
    });

    it('gives back the angles Sunshine and Wolf turn into tilt along x and y', () => {
        for (const [tilt, rotation] of [
            [40, 120],
            [15, 300],
            [60, 45],
            [5, 200],
        ]) {
            const got = penAngles(tiltsOf(tilt, rotation));
            expect(Math.abs(got.tilt - tilt)).toBeLessThanOrEqual(1);
            expect(Math.abs(got.rotation - rotation)).toBeLessThanOrEqual(1);
        }
    });

    it('holds a pen lying flat to 90 degrees', () => {
        expect(penAngles({ tiltX: 90, tiltY: 0 })).toEqual({ tilt: 90, rotation: 270 });
        expect(penAngles({ tiltX: 120, tiltY: 0 }).tilt).toBe(90);
    });

    it('reads altitude and azimuth when the tilt along each axis is missing', () => {
        // Cap at six o'clock, sixty degrees up: leaning towards the viewer.
        expect(penAngles({ altitudeAngle: 60 * DEG, azimuthAngle: 90 * DEG })).toEqual({
            tilt: 30,
            rotation: 0,
        });
        // Cap at nine o'clock: leaning left, pointing right.
        expect(penAngles({ altitudeAngle: 60 * DEG, azimuthAngle: 180 * DEG })).toEqual({
            tilt: 30,
            rotation: 90,
        });
        expect(penAngles({ altitudeAngle: 60 * DEG, azimuthAngle: 0 })).toEqual({
            tilt: 30,
            rotation: 270,
        });
    });
});

describe('the pen packet buttons', () => {
    it('makes the barrel button the primary one, and leaves the tip and eraser to the event', () => {
        expect(penButtons(0)).toBe(0);
        expect(penButtons(1)).toBe(0);
        expect(penButtons(32)).toBe(0);
        expect(penButtons(2)).toBe(PEN_BUTTON_PRIMARY);
        expect(penButtons(3)).toBe(PEN_BUTTON_PRIMARY);
        expect(penButtons(4)).toBe(PEN_BUTTON_SECONDARY);
        expect(penButtons(6)).toBe(PEN_BUTTON_PRIMARY | PEN_BUTTON_SECONDARY);
    });
});

describe('a pen stroke as the host is told it', () => {
    const sample = (over = {}) => ({
        pointerId: 7,
        buttons: 0,
        x: 0.5,
        y: 0.5,
        pressure: 0,
        angles: null,
        ...over,
    });
    const stroke = () => {
        const sent = [];
        return { pen: new PenStroke((m) => sent.push(m)), sent };
    };
    const events = (sent) => sent.map((m) => m.event);

    it('comes over the picture, goes down, moves, lifts and goes away', () => {
        const { pen, sent } = stroke();
        expect(pen.sample(sample())).toBe(true);
        expect(pen.sample(sample({ buttons: 1, pressure: 0.4 }))).toBe(true);
        expect(pen.contact).toBe(true);
        pen.sample(sample({ buttons: 1, pressure: 0.6, x: 0.6 }));
        pen.sample(sample({ buttons: 0, x: 0.6 }));
        expect(pen.contact).toBe(false);
        expect(pen.inRange).toBe(true);
        pen.leave();
        expect(events(sent)).toEqual(['hover', 'down', 'move', 'up', 'leave']);
        expect(sent[1]).toMatchObject({ type: 'pen', tool: 'pen', pressure: 0.4, x: 0.5, y: 0.5 });
        expect(sent[2]).toMatchObject({ pressure: 0.6, x: 0.6 });
        expect(sent[0].tool).toBeUndefined();
        expect(sent[0].pressure).toBeUndefined();
        expect(pen.active).toBe(false);
    });

    it('never starts a stroke over the bars around the picture', () => {
        const { pen, sent } = stroke();
        expect(pen.sample(sample({ buttons: 1, x: -0.1 }))).toBe(false);
        expect(pen.sample(sample({ x: 1.2 }))).toBe(false);
        expect(sent).toEqual([]);
    });

    it('follows a stroke off the picture, held to its edge', () => {
        const { pen, sent } = stroke();
        pen.sample(sample({ buttons: 1, pressure: 0.5 }));
        pen.sample(sample({ buttons: 1, pressure: 0.5, x: 1.4, y: -0.3 }));
        expect(sent[1]).toMatchObject({ event: 'move', x: 1, y: 0 });
        pen.sample(sample({ buttons: 0, x: 1.4 }));
        expect(sent[2]).toMatchObject({ event: 'up', x: 1 });
    });

    it('leaves the picture when a hovering pen goes over the bars, once', () => {
        const { pen, sent } = stroke();
        pen.sample(sample());
        pen.sample(sample({ x: -0.2 }));
        pen.sample(sample({ x: -0.3 }));
        expect(events(sent)).toEqual(['hover', 'leave']);
        expect(pen.active).toBe(false);
    });

    it('draws with the eraser end through the whole stroke', () => {
        const { pen, sent } = stroke();
        pen.sample(sample({ buttons: 32, pressure: 0.5 }));
        pen.sample(sample({ buttons: 32, pressure: 0.5, x: 0.7 }));
        pen.sample(sample({ buttons: 0, x: 0.7 }));
        expect(sent.map((m) => m.tool)).toEqual(['eraser', 'eraser', 'eraser']);
    });

    it('sends a touch measured at zero as the lightest one, not as unknown', () => {
        const { pen, sent } = stroke();
        pen.sample(sample({ buttons: 1, pressure: 0 }));
        expect(sent[0].pressure).toBe(MIN_CONTACT_PRESSURE);
        pen.sample(sample({ buttons: 1, pressure: Number.NaN }));
        expect(sent[1].pressure).toBe(MIN_CONTACT_PRESSURE);
    });

    it('carries the barrel button while hovering and while down', () => {
        const { pen, sent } = stroke();
        pen.sample(sample({ buttons: 2 }));
        pen.sample(sample({ buttons: 3, pressure: 0.5 }));
        expect(sent[0]).toMatchObject({ event: 'hover', buttons: PEN_BUTTON_PRIMARY });
        expect(sent[1]).toMatchObject({ event: 'down', buttons: PEN_BUTTON_PRIMARY });
    });

    it('carries the tilt only when the pen reports one', () => {
        const { pen, sent } = stroke();
        pen.sample(sample({ buttons: 1, pressure: 0.5, angles: { tilt: 30, rotation: 90 } }));
        pen.sample(sample({ buttons: 1, pressure: 0.5 }));
        expect(sent[0]).toMatchObject({ tilt: 30, rotation: 90 });
        expect('tilt' in sent[1]).toBe(false);
    });

    it('follows one pen: a second one is ignored while the first is there', () => {
        const { pen, sent } = stroke();
        pen.sample(sample({ buttons: 1, pressure: 0.5 }));
        expect(pen.sample(sample({ pointerId: 8, buttons: 1, x: 0.1 }))).toBe(false);
        expect(sent.length).toBe(1);
        pen.leave();
        expect(pen.sample(sample({ pointerId: 8 }))).toBe(true);
    });

    it('lifts a pen still down before it goes, where it last was', () => {
        const { pen, sent } = stroke();
        pen.sample(sample({ buttons: 1, pressure: 0.5, x: 0.25, y: 0.75 }));
        pen.leave();
        expect(events(sent)).toEqual(['down', 'up', 'leave']);
        expect(sent[1]).toMatchObject({ x: 0.25, y: 0.75, tool: 'pen', buttons: 0 });
        expect(sent[2]).toMatchObject({ x: 0.25, y: 0.75 });
    });

    it('says nothing when it goes without having been there', () => {
        const { pen, sent } = stroke();
        pen.leave();
        expect(sent).toEqual([]);
    });
});
