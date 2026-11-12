/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The automatic bitrate on the host's own network: the server says, in the
 * streaming settings a launch reads anyway, whether it sees this browser on
 * its network (`viewer_lan`), and the estimate takes the LAN rule — for a
 * GameStream host too, which takes its bitrate at launch and nowhere else.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { MoonlightApp } from '../js/app.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { resetStreamLimits } from '../js/util/StreamLimits.js';

const FIELDS = ['_streamLimitsRead', '_viewerOnLan'];
let saved;

beforeEach(() => {
    saved = Object.fromEntries(FIELDS.map((f) => [f, MoonlightApp[f]]));
    MoonlightApp._streamLimitsRead = false;
    MoonlightApp._viewerOnLan = undefined;
    vi.spyOn(console, 'log').mockImplementation(() => {});
    vi.spyOn(console, 'info').mockImplementation(() => {});
});

afterEach(() => {
    Object.assign(MoonlightApp, saved);
    resetStreamLimits();
    vi.restoreAllMocks();
});

const answer = (data) => vi.spyOn(BackendClient, 'getStreamingSettings').mockResolvedValue(data);
const sdr = {};
const hdr = { hdr_enabled: true };

describe('Auto bitrate, LAN or Internet', () => {
    it('takes the LAN rule when the host sees the browser on its network', async () => {
        answer({ viewer_lan: true });
        await MoonlightApp._readStreamLimits();
        expect(MoonlightApp._autoBitrateFor(sdr, 1080, '16:9')).toBe(20000);
        expect(MoonlightApp._autoBitrateFor(sdr, 1440, '16:9')).toBe(35000);
        expect(MoonlightApp._autoBitrateFor(hdr, 1080, '16:9')).toBe(24000);
    });

    it('keeps the Internet rule otherwise, and when the server does not say', async () => {
        answer({ viewer_lan: false });
        await MoonlightApp._readStreamLimits();
        expect(MoonlightApp._autoBitrateFor(sdr, 1080, '16:9')).toBe(10000);
        // A server from before v0.4.4 has no such key.
        MoonlightApp._streamLimitsRead = false;
        answer({ stream_fps: 0 });
        await MoonlightApp._readStreamLimits();
        expect(MoonlightApp._autoBitrateFor(sdr, 1080, '16:9')).toBe(10000);
        // Nothing read at all.
        MoonlightApp._viewerOnLan = undefined;
        expect(MoonlightApp._autoBitrateFor(hdr, 1080, '16:9')).toBe(12000);
    });

    it('follows the browser from one read to the next', async () => {
        answer({ viewer_lan: true });
        await MoonlightApp._readStreamLimits();
        expect(MoonlightApp._autoBitrateFor(sdr, 720, '16:9')).toBe(11000);
        answer({ viewer_lan: false });
        // Read again behind a launch: settled before the next one.
        await MoonlightApp._readStreamLimits();
        await new Promise((resolve) => setTimeout(resolve, 0));
        expect(MoonlightApp._autoBitrateFor(sdr, 720, '16:9')).toBe(6000);
    });

    it("stays under the administrator's ceiling", async () => {
        answer({ viewer_lan: true, max_allowed_bitrate: 15 });
        await MoonlightApp._readStreamLimits();
        expect(MoonlightApp._autoBitrateFor(sdr, 1080, '16:9')).toBe(15000);
    });
});
