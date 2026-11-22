/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The upscaler a launch asks for (MWRELALGO): one named by hand is a debug
 * build's, so a release build launches 'auto' whatever was saved — a choice a
 * debug build left behind included — and a debug build its own choice when the
 * list has it, 'auto' for anything else. Debug is the settings page's signal,
 * read from the same streaming settings, at the launch.
 */
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const built = vi.hoisted(() => []);
vi.mock('../js/ui/StreamView.js', () => ({
    StreamView: class {
        constructor(...args) {
            built.push(args);
        }
    },
}));

import { MoonlightApp } from '../js/app.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { resetStreamLimits } from '../js/util/StreamLimits.js';
import { isDebugBuild, launchEnhancerAlgo } from '../js/ui/SettingsView.js';

const SAVED = ['mwsr-q', 'gl-mwsr-q', 'mwsr-db', 'fsr1', 'gl-mwsr'];
const NOT_LISTED = ['no-such-algo', '', undefined, null, 42, 'auto', 'off'];

// Where StreamView takes the algorithm, read off its constructor.
const ALGO_ARG = (() => {
    const here = dirname(fileURLToPath(import.meta.url));
    const src = readFileSync(join(here, '..', 'js', 'ui', 'StreamView.js'), 'utf8');
    const params = src.match(/export class StreamView \{\s*constructor\(([\s\S]*?)\) \{/)[1];
    return params
        .split(',')
        .map((p) => p.split('=')[0].trim())
        .filter(Boolean)
        .indexOf('videoEnhancementAlgo');
})();

describe('launchEnhancerAlgo', () => {
    it("is 'auto' in a release build, whatever was saved", () => {
        for (const saved of [...SAVED, ...NOT_LISTED]) {
            expect(launchEnhancerAlgo(saved, false), String(saved)).toBe('auto');
            // Unread: a release build's launch.
            expect(launchEnhancerAlgo(saved, undefined), String(saved)).toBe('auto');
        }
    });

    it("keeps a debug build's choice of the list, and gives 'auto' for anything else", () => {
        for (const saved of SAVED) expect(launchEnhancerAlgo(saved, true)).toBe(saved);
        for (const saved of NOT_LISTED)
            expect(launchEnhancerAlgo(saved, true), String(saved)).toBe('auto');
    });
});

describe('isDebugBuild', () => {
    it('is the debug build or debug mode the server says, and nothing else', () => {
        expect(isDebugBuild({ debug_build: true })).toBe(true);
        expect(isDebugBuild({ debug_mode: true })).toBe(true);
        for (const data of [{}, null, undefined, { debug_build: 'true' }, { debug_mode: 1 }])
            expect(isDebugBuild(data), JSON.stringify(data)).toBe(false);
    });
});

describe('a launch', () => {
    const FIELDS = ['_streamLimitsRead', '_viewerOnLan', '_debugBuild'];
    let saved;

    beforeEach(() => {
        saved = Object.fromEntries(FIELDS.map((f) => [f, MoonlightApp[f]]));
        MoonlightApp._streamLimitsRead = false;
        MoonlightApp._debugBuild = undefined;
        built.length = 0;
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(console, 'info').mockImplementation(() => {});
    });

    afterEach(() => {
        Object.assign(MoonlightApp, saved);
        resetStreamLimits();
        vi.restoreAllMocks();
    });

    const launch = (algo) => {
        MoonlightApp._createStreamView(
            { videoCodec: 'h264' },
            {},
            { video_enhancement: 'on', video_enhancement_algo: algo },
        );
        return built.at(-1)[ALGO_ARG];
    };

    it('hands StreamView the algorithm by the streaming settings it read', async () => {
        expect(ALGO_ARG).toBeGreaterThan(0);
        vi.spyOn(BackendClient, 'getStreamingSettings').mockResolvedValue({});
        await MoonlightApp._readStreamLimits();
        expect(MoonlightApp._debugBuild).toBe(false);
        for (const algo of [...SAVED, 'no-such-algo']) expect(launch(algo), algo).toBe('auto');

        for (const flag of [{ debug_build: true }, { debug_mode: true }]) {
            vi.spyOn(BackendClient, 'getStreamingSettings').mockResolvedValue(flag);
            // A first read again: it is awaited (a later one runs in the background).
            MoonlightApp._streamLimitsRead = false;
            await MoonlightApp._readStreamLimits();
            expect(MoonlightApp._debugBuild).toBe(true);
            for (const algo of SAVED) expect(launch(algo), algo).toBe(algo);
            expect(launch('no-such-algo')).toBe('auto');
        }
    });

    it("launches 'auto' when the streaming settings were never read", () => {
        for (const algo of SAVED) expect(launch(algo), algo).toBe('auto');
    });
});
