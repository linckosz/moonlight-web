/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * The enhancer and decode-rate governors ride the 500 ms tick, not the stats
 * card's repaint: with the card hidden or closed, _updateOverlay returns
 * before its body, and both ladders used to stop there for the session.
 */
const P = StreamView.prototype;

const DIAG = { arrivalAvgMs: 1000 / 60, decodeQueueAvg: 3, renderServiceMs: 2 };

function view(over = {}) {
    return {
        _transport: 'webrtc',
        _firstFrameRendered: true,
        _useWorker: false,
        _workerDiag: null,
        _diag: { snapshot: () => DIAG },
        _fpsTimestamps: [],
        _applyEnhancerGovernor: vi.fn(),
        _applyDecodeRateGovernor: vi.fn(),
        _tickAutoCeiling: vi.fn(),
        _tickAv1Watch: vi.fn(),
        _decodedFps: P._decodedFps,
        _runPipelineGovernors: P._runPipelineGovernors,
        ...over,
    };
}

describe('StreamView — the governors on the tick', () => {
    it('feeds both governors with the card hidden or closed', () => {
        for (const card of [{ _showPerfStats: false }, { _statsClosed: true }]) {
            const v = view(card);
            v._fpsTimestamps = Array.from({ length: 120 }, (_, i) => 9000 + i * 8);
            v._runPipelineGovernors(10000);
            expect(v._applyEnhancerGovernor).toHaveBeenCalledWith(DIAG, 10000);
            expect(v._applyDecodeRateGovernor).toHaveBeenCalledWith(DIAG, 60, 10000);
        }
    });

    it('leaves the enhancer to the worker, and reads its posted counters', () => {
        const posted = { ...DIAG, decodeQueueAvg: 5 };
        const v = view({ _useWorker: true, _workerDiag: posted });
        v._runPipelineGovernors(10000);
        expect(v._applyEnhancerGovernor).not.toHaveBeenCalled();
        expect(v._applyDecodeRateGovernor).toHaveBeenCalledWith(posted, 0, 10000);
    });

    it('waits for the first frame, and skips webrtc-media', () => {
        for (const over of [{ _firstFrameRendered: false }, { _transport: 'webrtc-media' }]) {
            const v = view(over);
            v._runPipelineGovernors(10000);
            expect(v._applyEnhancerGovernor).not.toHaveBeenCalled();
            expect(v._applyDecodeRateGovernor).not.toHaveBeenCalled();
            expect(v._tickAutoCeiling).not.toHaveBeenCalled();
        }
    });

    it("feeds the meter of Auto's ceiling what the pipeline saw", () => {
        const tick = vi.fn();
        const v = {
            autoCeilingMeter: {
                tick,
                asked: { width: 1920, height: 1080, fps: 120, below: 0 },
            },
            hasActiveShare: () => false,
            stats: { received: 500 },
            _resolution: '1920×1080',
            _decodeRateCap: 0,
            _hostPresentsFps: 118,
            _tickAutoCeiling: P._tickAutoCeiling,
        };
        v._tickAutoCeiling(
            { arrivalAvgMs: 10, dropQueueFull: 2, dropBackpressure: 3, recoverReset: 1 },
            10000,
        );
        expect(tick).toHaveBeenCalledWith(10000, {
            neutral: false,
            stepped: false,
            arrivalFps: 100,
            sizeMatches: true,
            frames: 500,
            drops: 5,
            rebuilds: 1,
            fpsCap: 0,
            presentsFps: 118,
        });
    });

    it('calls a share or a transition neutral for that meter, and says when the cadence steps', () => {
        const sample = (over) => {
            const tick = vi.fn();
            const v = {
                autoCeilingMeter: { tick, asked: null },
                hasActiveShare: () => false,
                stats: { received: 0 },
                _tickAutoCeiling: P._tickAutoCeiling,
                ...over,
            };
            v._tickAutoCeiling(DIAG, 10000);
            return tick.mock.calls[0][1];
        };
        expect(sample({}).neutral).toBe(false);
        expect(sample({ hasActiveShare: () => true }).neutral).toBe(true);
        expect(sample({ autoCeilingIdle: () => true }).neutral).toBe(true);
        expect(sample({ _stepper: { stepFps: 240 } })).toMatchObject({
            neutral: false,
            stepped: true,
        });
        expect(sample({ _stepper: { stepFps: 0 } })).toMatchObject({
            neutral: false,
            stepped: false,
        });
    });

    it('tells that meter of a step down of the enhancer, not of a step up', () => {
        const steps = (from, to) => {
            const noteSick = vi.fn();
            P._noteEnhancerStep.call(
                { autoCeilingMeter: { noteSick }, _videoEnhancementAlgo: from },
                to,
            );
            return noteSick.mock.calls.length;
        };
        expect(steps('mwsr', 'off')).toBe(1);
        expect(steps('gl-sgsr', 'gl-mwsr')).toBe(1);
        expect(steps('off', 'mwsr')).toBe(0);
        expect(steps('auto', 'off')).toBe(0);
    });

    it('prunes the decoded-frame window every tick, card or not', () => {
        const v = view({ _firstFrameRendered: false });
        v._fpsTimestamps = Array.from({ length: 600 }, (_, i) => i * 16);
        v._runPipelineGovernors(20000);
        expect(v._fpsTimestamps).toEqual([]);
    });
});

describe("StreamView — what Auto's ceiling asks of the rest", () => {
    it('keeps the verdict of a rung on trial out of the enhancer memory', () => {
        localStorage.removeItem('mw_enhancer_rung');
        const v = {
            _enhancerCeiling: 'mwsr',
            autoCeilingOnTrial: () => true,
            _rememberEnhancer: P._rememberEnhancer,
        };
        v._rememberEnhancer('webgl', 'off', 1000);
        expect(localStorage.getItem('mw_enhancer_rung')).toBeNull();
        v.autoCeilingOnTrial = () => false;
        v._rememberEnhancer('webgl', 'off', 1000);
        expect(JSON.parse(localStorage.getItem('mw_enhancer_rung')).webgl.rung).toBe('off');
        localStorage.removeItem('mw_enhancer_rung');
    });

    it('turns the cadence bound into a rate at this frame, 1080p240 allowed at the base', () => {
        const at = (autoCeilingMaxPixelRate, _resolution) =>
            P._autoCeilingMaxFps.call({ autoCeilingMaxPixelRate, _resolution });
        expect(at(500e6, '1920×1080')).toBe(241);
        expect(at(1000e6, '3840×2160')).toBe(120);
        expect(at(0, '3840×2160')).toBe(0);
        expect(at(500e6, '')).toBe(0);
    });
});

describe("StreamView — the stats card's line on Auto's ceiling", () => {
    const row = (line) =>
        P._autoCeilingRow.call({ autoCeilingLine: line === undefined ? null : () => line });

    it('says nothing when nothing was learned', () => {
        expect(row()).toBe('');
        expect(row(null)).toBe('');
    });

    it('adds one row of the card when there is something to say', () => {
        const html = row({ size: '1440p120', trial: true });
        expect(html.match(/class="stats-row"/g)).toHaveLength(1);
        expect(html).toContain('class="stats-label"');
        expect(html).toContain('class="stats-value"');
    });
});

describe("the words for Auto's ceiling, as Bruno validated them", () => {
    const words = (code) => {
        const path = fileURLToPath(new URL('../locales/' + code + '.json', import.meta.url));
        const s = JSON.parse(readFileSync(path, 'utf8')).stream;
        const fill = (text) => text.replace('{{size}}', '1440p120');
        return [
            s.statAuto + ' ' + fill(s.statAutoLearned),
            s.statAuto + ' ' + fill(s.statAutoTrial),
        ];
    };

    it('reads the same in the three languages', () => {
        expect(words('fr')).toEqual(['Auto : 1440p120, appris', "Auto : 1440p120, à l'essai"]);
        expect(words('en')).toEqual(['Auto: 1440p120, learned', 'Auto: 1440p120, on trial']);
        expect(words('zh').map((w) => w.replace(' ', ''))).toEqual([
            '自动：1440p120，已学习',
            '自动：1440p120，试用中',
        ]);
    });
});
