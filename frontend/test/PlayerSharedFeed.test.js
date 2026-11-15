/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * A guest of a native host. Its guests watch one picture: the height is the
 * owner's, so the join page says which instead of offering a choice. And that
 * picture is HEVC unless one guest cannot decode it — a browser without HEVC
 * asks for H.264 before joining, instead of failing its first picture and
 * coming back.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        playerInfo: vi.fn(),
        playerPin: vi.fn(),
    },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, params) => (params ? 'text:' + key + JSON.stringify(params) : 'text:' + key),
}));

const BASE = {
    needs_pin: false,
    machine_name: 'ATELIER',
    state: 'idle',
    access_level: 'viewer',
    permissions: {},
};

describe('PlayerJoinView on a native host', () => {
    let container;

    beforeEach(() => {
        document.body.innerHTML = '<div id="player"></div>';
        container = document.getElementById('player');
        vi.clearAllMocks();
        // The HEVC answer is kept for the page's life: a fresh page per test.
        vi.resetModules();
    });

    afterEach(() => {
        vi.unstubAllGlobals();
    });

    /** The join page for @p info, in a browser that decodes HEVC or not. */
    async function mount(info, decodesHevc) {
        vi.stubGlobal('VideoDecoder', {
            isConfigSupported: async (cfg) => ({
                supported: decodesHevc || !/^(hev1|hvc1)/.test(cfg.codec),
                config: cfg,
            }),
        });
        const { PlayerJoinView } = await import('../js/ui/PlayerJoinView.js');
        const { BackendClient } = await import('../js/api/BackendClient.js');
        BackendClient.playerInfo.mockResolvedValue({ ...BASE, ...info });
        const onJoin = vi.fn(async () => {});
        const view = new PlayerJoinView(container, 'tok', onJoin);
        await view.refresh();
        return onJoin;
    }

    const join = async (onJoin) => {
        container.querySelector('.player-join-btn').click();
        await vi.waitFor(() => expect(onJoin).toHaveBeenCalled());
        return onJoin.mock.calls[0];
    };

    it("says the owner's height instead of offering a choice", async () => {
        await mount({ feed_height: 1440, shared_feed: true }, true);
        expect(container.querySelector('.player-quality-btn')).toBeNull();
        const fixed = container.querySelector('.player-quality-fixed');
        expect(fixed.textContent).toContain('text:player.qualityFixed');
        expect(fixed.textContent).toContain('1440');
    });

    it("joins at the owner's height, leaving the codec to the host", async () => {
        const onJoin = await mount({ feed_height: 720, shared_feed: true }, true);
        const [info, transportIndex, codec] = await join(onJoin);
        expect(info.height).toBe(720);
        expect(transportIndex).toBe(0);
        expect(codec).toBeUndefined();
    });

    it('asks for H.264 at once when this browser decodes no HEVC', async () => {
        const onJoin = await mount({ feed_height: 1080, shared_feed: true }, false);
        const [, , codec] = await join(onJoin);
        expect(codec).toBe('h264');
    });

    it('changes nothing on a host where each guest has a stream of its own', async () => {
        const onJoin = await mount({}, false);
        // The guest's own choice, Auto first (issue #67)…
        expect(container.querySelectorAll('.player-quality-btn').length).toBe(4);
        expect(container.querySelector('.player-quality-fixed')).toBeNull();
        // …and the codec the backend's, the fallback coming after a failure.
        const [info, , codec] = await join(onJoin);
        expect(info.height).toBe('auto');
        expect(codec).toBeUndefined();
    });
});
