/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';

import {
    CANVAS_H,
    CANVAS_W,
    DEVICE_IDS,
    WALLPAPERS,
    hashKey,
    hostArtFor,
    hostArtHtml,
    hostArtSvg,
    renderDevice,
} from '../js/ui/HostArt.js';

const dev = (os, display, extra = {}) => ({
    os,
    display,
    model: '',
    key: 'k',
    battery: false,
    ...extra,
});

/**
 * The picture a native host's card draws for each of its displays. What can
 * silently go wrong is the rule (a MacBook drawn as an iMac), the stability of
 * the "random" picks (a card changing picture between two visits) and the
 * drawing itself (a device spilling out of the shared canvas).
 */
describe('HostArt', () => {
    describe('which machine a display is drawn as', () => {
        it('draws Windows as a laptop, a monitor or a mini-PC', () => {
            expect(hostArtFor(dev('windows', 'builtin')).id).toBe('windows-laptop');
            expect(['windows-monitor', 'windows-monitor-4x3']).toContain(
                hostArtFor(dev('windows', 'external')).id,
            );
            expect(hostArtFor(dev('windows', 'virtual')).id).toBe('windows-mini');
        });

        it('draws Linux as a laptop, a monitor or a mini-PC', () => {
            expect(hostArtFor(dev('linux', 'builtin')).id).toBe('linux-laptop');
            expect(['linux-monitor', 'linux-monitor-terminal']).toContain(
                hostArtFor(dev('linux', 'external')).id,
            );
            expect(hostArtFor(dev('linux', 'virtual')).id).toBe('linux-mini');
        });

        it('tells a MacBook from an iMac by the battery', () => {
            expect(hostArtFor(dev('macos', 'builtin', { battery: true })).id).toBe('macbook');
            expect(['imac-g3', 'imac-alu']).toContain(hostArtFor(dev('macos', 'builtin')).id);
        });

        it('draws a Studio Display as itself, whatever machine drives it', () => {
            const studio = dev('macos', 'external', { model: 'Studio Display' });
            expect(hostArtFor(studio).id).toBe('studio-display');
            expect(hostArtFor({ ...studio, battery: true }).id).toBe('studio-display');
        });

        it('draws a monitor on a Mac without a screen of its own as a Mac mini', () => {
            const monitor = dev('macos', 'external', { model: 'LG UltraFine' });
            expect(hostArtFor(monitor, [monitor]).id).toBe('mac-mini');
            expect(hostArtFor(dev('macos', 'virtual')).id).toBe('mac-mini');
            // …and as an old iMac on a MacBook, or next to an iMac's own panel.
            expect(['imac-g3', 'imac-alu']).toContain(hostArtFor({ ...monitor, battery: true }).id);
            const panel = dev('macos', 'builtin');
            expect(['imac-g3', 'imac-alu']).toContain(hostArtFor(monitor, [panel, monitor]).id);
        });

        it('falls back to the default screen when nothing is known', () => {
            expect(hostArtFor(dev('linux', 'unknown')).id).toBe('default-monitor');
            expect(hostArtFor(dev('unknown', 'external')).id).toBe('default-monitor');
            expect(hostArtFor(null).id).toBe('default-monitor');
            expect(hostArtFor({}).id).toBe('default-monitor');
        });

        it('puts an arcade sprite on Windows screens only', () => {
            expect(WALLPAPERS).toEqual(['invader', 'ghost', 'joystick', 'ship', 'coin']);
            expect(WALLPAPERS).toContain(hostArtFor(dev('windows', 'builtin')).wallpaper);
            expect(WALLPAPERS).toContain(hostArtFor(dev('windows', 'external')).wallpaper);
            expect(hostArtFor(dev('windows', 'virtual')).wallpaper).toBeNull();
            expect(hostArtFor(dev('linux', 'external')).wallpaper).toBeNull();
            expect(hostArtFor(dev('macos', 'builtin', { battery: true })).wallpaper).toBeNull();
        });
    });

    describe('the random picks', () => {
        it('are drawn from the display key, so the same screen always gets the same picture', () => {
            const a = dev('windows', 'external', {
                key: '\\\\?\\DISPLAY#GBT270D#5&1f2a&0&UID4352',
            });
            expect(hostArtFor(a)).toEqual(hostArtFor({ ...a }));
            // Its position in the list (the app id) plays no part.
            expect(hostArtFor(a, [dev('windows', 'builtin'), a])).toEqual(hostArtFor(a, [a]));
            expect(hashKey('abc')).toBe(hashKey('abc'));
        });

        it('reach every monitor and every sprite across machines', () => {
            const ids = new Set();
            const sprites = new Set();
            for (let i = 0; i < 200; i++) {
                const art = hostArtFor(
                    dev('windows', 'external', { key: `\\\\?\\DISPLAY#MON${i}` }),
                );
                ids.add(art.id);
                sprites.add(art.wallpaper);
            }
            expect(ids).toEqual(new Set(['windows-monitor', 'windows-monitor-4x3']));
            expect(sprites).toEqual(new Set(WALLPAPERS));
        });
    });

    describe('the drawings', () => {
        it('keep every device, with every wallpaper, inside the shared canvas', () => {
            for (const id of DEVICE_IDS) {
                for (const wallpaper of id.startsWith('windows-') ? WALLPAPERS : [null]) {
                    const label = `${id} ${wallpaper}`;
                    const svg = hostArtSvg(id, wallpaper);
                    expect(svg, label).toMatch(/^<svg class="app-icon-sprite"/);
                    const runs = [...svg.matchAll(/M(-?\d+) (-?\d+)h(\d+)/g)].map((m) => [
                        +m[1],
                        +m[2],
                        +m[3],
                    ]);
                    expect(runs.length, label).toBeGreaterThan(50);
                    for (const [x, y, w] of runs) {
                        expect(x, label).toBeGreaterThanOrEqual(0);
                        expect(y, label).toBeGreaterThanOrEqual(0);
                        expect(x + w, label).toBeLessThanOrEqual(CANVAS_W);
                        expect(y + 1, label).toBeLessThanOrEqual(CANVAS_H);
                    }
                }
            }
        });

        it('shows the arcade sprite on the screen, so two sprites make two pictures', () => {
            expect(hostArtSvg('windows-laptop', 'ghost')).not.toBe(
                hostArtSvg('windows-laptop', 'coin'),
            );
            // A device without a wallpaper ignores the parameter.
            expect(hostArtSvg('linux-laptop', 'ghost')).toBe(hostArtSvg('linux-laptop', null));
        });

        it('draws a box as a lit top, a front and a darker side, outlined, on its shadow', () => {
            const cube = { x0: 0, x1: 8, y0: 0, y1: 8, z0: 0, z1: 8, mat: 'gray', r: 0 };
            const { pixels, shadows } = renderDevice([cube]);
            const colours = new Set(pixels.values());
            for (const c of ['#56616c', '#434d58', '#2f3740', '#77838e', '#080b0f']) {
                expect(colours, c).toContain(c);
            }
            expect(shadows.length).toBeGreaterThan(0);
            expect(renderDevice([]).pixels.size).toBe(0);
        });

        it('draws only the picture: no label, no frame, and nothing a screen reader reads', () => {
            const html = hostArtHtml(dev('linux', 'builtin'));
            expect(html).toContain('app-icon--host');
            expect(html).toContain('aria-hidden="true"');
            expect(html).not.toContain('app-icon-label');
            expect(html.replace(/<[^>]*>/g, '')).toBe('');
        });
    });
});
