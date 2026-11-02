/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi } from 'vitest';
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

    it('feeds the meter of Auto\'s ceiling what the pipeline saw', () => {
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
            arrivalFps: 100,
            sizeMatches: true,
            frames: 500,
            drops: 5,
            rebuilds: 1,
            fpsCap: 0,
            presentsFps: 118,
        });
    });

    it('calls a share, a transition or a cadence step neutral for that meter', () => {
        const neutral = (over) => {
            const tick = vi.fn();
            const v = {
                autoCeilingMeter: { tick, asked: null },
                hasActiveShare: () => false,
                stats: { received: 0 },
                _tickAutoCeiling: P._tickAutoCeiling,
                ...over,
            };
            v._tickAutoCeiling(DIAG, 10000);
            return tick.mock.calls[0][1].neutral;
        };
        expect(neutral({})).toBe(false);
        expect(neutral({ hasActiveShare: () => true })).toBe(true);
        expect(neutral({ autoCeilingIdle: () => true })).toBe(true);
        expect(neutral({ _stepper: { stepFps: 240 } })).toBe(true);
        expect(neutral({ _stepper: { stepFps: 0 } })).toBe(false);
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
