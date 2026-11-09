/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The page waiting for the host to come back from an update (09/11/2026: on the
 * UM790Pro and on mw-arm, opened at https://<LAN IP>, the page sat on
 * "Restarting MoonlightWeb…" long after the host was back — the host had a new
 * certificate, so every background request failed without a word). Pinned
 * down: a status request that never settles ends phase 1; a long run of
 * requests with no HTTP answer at all reloads the page, which is the one thing
 * that shows the browser's certificate warning; any HTTP answer, even an error,
 * restarts that count; the new version still reloads the page as before.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        startUpdate: vi.fn(async () => ({})),
        getUpdateStatus: vi.fn(),
        probeHealthOutcome: vi.fn(),
    },
}));

import { HostUpdateRun } from '../js/ui/HostUpdate.js';
import { BackendClient } from '../js/api/BackendClient.js';

/** A run on a clock the test moves: each wait advances it, nothing really waits. */
function makeRun() {
    const run = new HostUpdateRun();
    let clock = 0;
    run._now = () => clock;
    run._sleep = async (ms) => {
        clock += ms;
    };
    run._reload = vi.fn();
    run._startAnimation = () => {};
    run._stopAnimation = () => {};
    return { run, elapsed: () => clock };
}

const SILENT = { health: null, network: true };

describe('the page waiting for the host after an update', () => {
    beforeEach(() => {
        vi.clearAllMocks();
        sessionStorage.clear();
    });

    it('bounds every status request, so one that never settles cannot hold phase 1', async () => {
        const { run } = makeRun();
        BackendClient.getUpdateStatus.mockRejectedValue(new Error('server_timeout'));
        BackendClient.probeHealthOutcome.mockResolvedValue({
            health: { version: '0.4.3' },
            network: false,
        });
        await run._watch({ current: '0.4.2' });
        expect(BackendClient.getUpdateStatus).toHaveBeenCalledWith({
            timeoutMs: HostUpdateRun.STATUS_TIMEOUT_MS,
        });
        expect(run._reload).toHaveBeenCalledTimes(1);
    });

    it('reloads the page after a long run of requests that got no HTTP answer', async () => {
        const { run, elapsed } = makeRun();
        BackendClient.getUpdateStatus.mockRejectedValue(new TypeError('Failed to fetch'));
        BackendClient.probeHealthOutcome.mockResolvedValue(SILENT);
        await run._watch({ current: '0.4.2' });
        expect(run._reload).toHaveBeenCalledTimes(1);
        // Not before the threshold, and not long after it either.
        expect(elapsed()).toBeGreaterThanOrEqual(HostUpdateRun.NETWORK_RELOAD_MS);
        expect(elapsed()).toBeLessThan(HostUpdateRun.NETWORK_RELOAD_MS + 15000);
    });

    it('lets a normal install run: silence shorter than the threshold reloads only on the new version', async () => {
        const { run } = makeRun();
        BackendClient.getUpdateStatus.mockRejectedValue(new TypeError('Failed to fetch'));
        const silentPolls = Math.floor(HostUpdateRun.NETWORK_RELOAD_MS / 3000) - 2;
        for (let i = 0; i < silentPolls; i++)
            BackendClient.probeHealthOutcome.mockResolvedValueOnce(SILENT);
        BackendClient.probeHealthOutcome.mockResolvedValue({
            health: { version: '0.4.3' },
            network: false,
        });
        await run._watch({ current: '0.4.2' });
        expect(run._reload).toHaveBeenCalledTimes(1);
        expect(BackendClient.probeHealthOutcome).toHaveBeenCalledTimes(silentPolls + 1);
    });

    it('counts again from zero after any HTTP answer, an error included', async () => {
        const { run, elapsed } = makeRun();
        BackendClient.getUpdateStatus.mockRejectedValue(new TypeError('Failed to fetch'));
        const half = Math.floor(HostUpdateRun.NETWORK_RELOAD_MS / 3000 / 2);
        for (let i = 0; i < half; i++)
            BackendClient.probeHealthOutcome.mockResolvedValueOnce(SILENT);
        // A proxy's 502: something answered, so the host is not unreachable.
        BackendClient.probeHealthOutcome.mockResolvedValueOnce({ health: null, network: false });
        BackendClient.probeHealthOutcome.mockResolvedValue(SILENT);
        await run._watch({ current: '0.4.2' });
        expect(run._reload).toHaveBeenCalledTimes(1);
        expect(elapsed()).toBeGreaterThanOrEqual(
            (half + 1) * 3000 + HostUpdateRun.NETWORK_RELOAD_MS,
        );
    });

    it('does not reload onto the version it started from', async () => {
        const { run } = makeRun();
        BackendClient.getUpdateStatus.mockRejectedValue(new TypeError('Failed to fetch'));
        BackendClient.probeHealthOutcome.mockResolvedValue({
            health: { version: '0.4.2' },
            network: false,
        });
        await run._watch({ current: '0.4.2' });
        expect(run._reload).not.toHaveBeenCalled();
    });
});
