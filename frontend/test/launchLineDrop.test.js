/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

/**
 * 15/11/2026: an iPhone back from 35 minutes in the background pressed a card;
 * the launch went out on a line that looked open and carried nothing, failed
 * when the transport gave up on it, and the line was back a second later. The
 * second press worked. launchAppKeepingLine now makes that second press itself,
 * once, and only when the line failed to carry the launch: never over an
 * answer from the host.
 */

const line = { ready: vi.fn(async () => {}) };
vi.mock('../js/net/tunnelBridge.js', async (importOriginal) => ({
    ...(await importOriginal()),
    lineReady: () => line.ready(),
}));

const { BackendClient } = await import('../js/api/BackendClient.js');
const { noteNotCarried, notCarriedSoFar } = await import('../js/net/tunnelBridge.js');

/** What BackendClient throws for an answer that is not OK. */
function answered(status, code) {
    return Object.assign(new Error(code || `Request failed (${status})`), {
        statusCode: status,
        responseBody: code ? { error: code, code } : 'the connection failed',
    });
}

/** The line failed to carry @p path: what the bridge sees, and the 502 the
 *  service worker then answers by itself. */
function notCarried(path) {
    noteNotCarried('POST', path);
    return answered(502);
}

describe('launchAppKeepingLine and a line that fails under it', () => {
    beforeEach(() => {
        vi.restoreAllMocks();
        line.ready.mockClear();
        vi.spyOn(console, 'warn').mockImplementation(() => {});
    });

    it('launches once more on the new line when the line failed to carry the launch', async () => {
        const launch = vi
            .spyOn(BackendClient, 'launchApp')
            .mockImplementationOnce(async () => {
                throw notCarried('/api/hosts/host/start');
            })
            .mockImplementationOnce(async () => ({ status: 'streaming' }));

        const result = await BackendClient.launchAppKeepingLine('host', 1, { video_codec: 'hevc' });

        expect(result).toEqual({ status: 'streaming' });
        expect(launch).toHaveBeenCalledTimes(2);
        // The same launch, made again from its start: whatever it signs or asks
        // first is asked again, nothing of the first one is replayed.
        expect(launch.mock.calls[1]).toEqual(['host', 1, { video_codec: 'hevc' }]);
        expect(line.ready).toHaveBeenCalledTimes(1);
    });

    // SR, E1: the line dropped and came back during the launch, then the host
    // answered. Its answer stands, and only one /start went out.
    it('reports what the host answered, even when the line dropped in between', async () => {
        const refusal = answered(409, 'key_refused');
        const launch = vi.spyOn(BackendClient, 'launchApp').mockImplementation(async () => {
            noteNotCarried('POST', '/api/hosts/host/launch-proof'); // lost, asked again
            throw refusal; // and then the host said no
        });

        await expect(BackendClient.launchAppKeepingLine('host', 1)).rejects.toBe(refusal);
        expect(launch).toHaveBeenCalledTimes(1);
        expect(line.ready).not.toHaveBeenCalled();
    });

    it('never asks again over a key refused or a proof required', async () => {
        for (const code of ['key_refused', 'launch_proof_required']) {
            const refusal = answered(409, code);
            const launch = vi.spyOn(BackendClient, 'launchApp').mockRejectedValue(refusal);
            await expect(BackendClient.launchAppKeepingLine('host', 1)).rejects.toBe(refusal);
            expect(launch).toHaveBeenCalledTimes(1);
            launch.mockRestore();
        }
    });

    it('reports a 502 the host answered itself', async () => {
        const fromHost = answered(502, 'The host did not answer');
        const launch = vi.spyOn(BackendClient, 'launchApp').mockRejectedValue(fromHost);

        await expect(BackendClient.launchAppKeepingLine('host', 1)).rejects.toBe(fromHost);
        expect(launch).toHaveBeenCalledTimes(1);
    });

    it("does not take another host's lost request for this launch's", async () => {
        const launch = vi.spyOn(BackendClient, 'launchApp').mockImplementation(async () => {
            throw notCarried('/api/hosts/other/start');
        });

        await expect(BackendClient.launchAppKeepingLine('host', 1)).rejects.toMatchObject({
            statusCode: 502,
        });
        expect(launch).toHaveBeenCalledTimes(1);
    });

    it('asks only once more: a second failure is reported', async () => {
        const launch = vi.spyOn(BackendClient, 'launchApp').mockImplementation(async () => {
            throw notCarried('/api/hosts/host/start');
        });

        await expect(BackendClient.launchAppKeepingLine('host', 1)).rejects.toMatchObject({
            statusCode: 502,
        });
        expect(launch).toHaveBeenCalledTimes(2);
    });

    it('does not touch a launch that worked', async () => {
        const launch = vi
            .spyOn(BackendClient, 'launchApp')
            .mockResolvedValue({ status: 'streaming' });

        await expect(BackendClient.launchAppKeepingLine('host', 1)).resolves.toEqual({
            status: 'streaming',
        });
        expect(launch).toHaveBeenCalledTimes(1);
    });
});

describe('notCarriedSoFar', () => {
    it('counts by method and path, from what the bridge noted', () => {
        const before = notCarriedSoFar('POST', '/api/hosts/h9/');
        noteNotCarried('POST', 'https://stream.example/api/hosts/h9/start?x=1');
        noteNotCarried('post', '/api/hosts/h9/launch-proof');
        noteNotCarried('GET', '/api/hosts/h9/apps');
        noteNotCarried('POST', '/api/hosts/h10/start');

        expect(notCarriedSoFar('POST', '/api/hosts/h9/') - before).toBe(2);
    });
});
