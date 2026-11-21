/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

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
const pairing = await import('../js/util/pairingCrypto.js');

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

// SR, E2: the same, over the real launchApp and the host's launch challenges
// (LaunchGate). A launch made again asks for a new challenge and signs it; a
// key the host refused is never asked again, whatever the line did before.
describe('launchAppKeepingLine and the launch challenges', () => {
    let posts;

    /** Each route answers from its own queue, in order: an answer, or a
     *  function that throws what the line or the host did. */
    const host = (queues) => {
        posts = [];
        vi.spyOn(BackendClient, 'post').mockImplementation(async (path, body) => {
            posts.push({ path, body: structuredClone(body) });
            const next = queues[path.split('/').pop()]?.shift();
            if (next === undefined) throw new Error(`Nothing should ask ${path}`);
            return typeof next === 'function' ? next() : next;
        });
    };
    const sent = (route) => posts.filter((p) => p.path.endsWith(`/${route}`));
    const routes = () => posts.map((p) => p.path.split('/').pop());
    const challenge = (nonce) => ({ required: true, nonce, host_id: 'H' });
    const withoutProof = (body) => {
        const rest = { ...body };
        delete rest.launch_proof;
        return rest;
    };

    beforeEach(() => {
        vi.restoreAllMocks();
        line.ready.mockClear();
        BackendClient._quitTickets.clear();
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(pairing, 'signLaunchProof').mockImplementation(
            async (_label, _hostId, nonce) => `sig:${nonce}`,
        );
    });
    afterEach(() => {
        vi.restoreAllMocks();
        BackendClient._quitTickets.clear();
    });

    it('signs the launch made again over a new challenge: two challenges, two signatures', async () => {
        host({
            'launch-proof': [challenge('N1'), challenge('N2')],
            start: [
                () => {
                    throw notCarried('/api/hosts/host/start');
                },
                { status: 'streaming' },
            ],
        });
        let askedBeforeTheLine = -1;
        line.ready.mockImplementationOnce(async () => {
            askedBeforeTheLine = posts.length;
        });

        const result = await BackendClient.launchAppKeepingLine('host', 1, { video_codec: 'hevc' });

        expect(result).toEqual({ status: 'streaming' });
        expect(routes()).toEqual(['launch-proof', 'start', 'launch-proof', 'start']);
        // The new line is waited for once, between the two launches.
        expect(line.ready).toHaveBeenCalledTimes(1);
        expect(askedBeforeTheLine).toBe(2);

        const signs = pairing.signLaunchProof.mock.calls;
        expect(signs).toHaveLength(2);
        expect(signs.map((c) => c[2])).toEqual(['N1', 'N2']);
        // The same launch, for the same host, signed twice over its own challenge.
        expect(signs[1]).toEqual([pairing.LAUNCH_LABEL, 'H', 'N2', signs[0][3]]);

        const [first, second] = sent('start');
        expect(first.body.launch_proof).toEqual({ nonce: 'N1', sig: 'sig:N1' });
        expect(second.body.launch_proof).toEqual({ nonce: 'N2', sig: 'sig:N2' });
        expect(second.body.launch_proof.nonce).not.toBe(first.body.launch_proof.nonce);
        expect(second.body.launch_proof.sig).not.toBe(first.body.launch_proof.sig);
        // Nothing of the first launch is replayed: its challenge and signature
        // appear in nothing sent after it, and the launch itself is the same.
        expect(JSON.stringify(posts.slice(2))).not.toMatch(/N1/);
        expect(withoutProof(second.body)).toEqual(withoutProof(first.body));
        expect(second.body).toMatchObject({ appId: 1, video_codec: 'hevc' });
    });

    it('gives up after the second launch the line did not carry: two tries, two signatures', async () => {
        let lostAgain = null;
        host({
            'launch-proof': [challenge('N1'), challenge('N2')],
            start: [
                () => {
                    throw notCarried('/api/hosts/host/start');
                },
                () => {
                    lostAgain = notCarried('/api/hosts/host/start');
                    throw lostAgain;
                },
            ],
        });

        const failed = await BackendClient.launchAppKeepingLine('host', 1).catch((e) => e);
        expect(failed).toBe(lostAgain);
        expect(failed).toMatchObject({ statusCode: 502 });
        expect(routes()).toEqual(['launch-proof', 'start', 'launch-proof', 'start']);
        expect(pairing.signLaunchProof.mock.calls.map((c) => c[2])).toEqual(['N1', 'N2']);
        expect(line.ready).toHaveBeenCalledTimes(1);
    });

    it.each(['key_refused', 'launch_proof_required'])(
        'never asks again over a %s the host answered',
        async (code) => {
            const refusal = answered(409, code);
            host({
                'launch-proof': [challenge('N1')],
                start: [
                    () => {
                        throw refusal;
                    },
                ],
            });

            await expect(BackendClient.launchAppKeepingLine('host', 1)).rejects.toBe(refusal);
            expect(routes()).toEqual(['launch-proof', 'start']);
            expect(pairing.signLaunchProof).toHaveBeenCalledTimes(1);
            expect(line.ready).not.toHaveBeenCalled();
        },
    );

    it.each(['key_refused', 'launch_proof_required'])(
        'asks once more over a lost challenge, and not again over the %s that follows',
        async (code) => {
            const refusal = answered(409, code);
            host({
                'launch-proof': [
                    () => {
                        throw notCarried('/api/hosts/host/launch-proof');
                    },
                    challenge('N2'),
                ],
                start: [
                    () => {
                        throw refusal;
                    },
                ],
            });

            await expect(BackendClient.launchAppKeepingLine('host', 1)).rejects.toBe(refusal);
            expect(routes()).toEqual(['launch-proof', 'launch-proof', 'start']);
            expect(pairing.signLaunchProof).toHaveBeenCalledTimes(1);
            expect(pairing.signLaunchProof.mock.calls[0][2]).toBe('N2');
            expect(line.ready).toHaveBeenCalledTimes(1);
        },
    );
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
