/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, afterEach } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';
import { PipelineDiag } from '../js/stream/PipelineDiag.js';

/**
 * The [perf] line with verbose logs on, whether the stats card is shown or
 * not. It used to be written by the card's own tick only: hidden, the tick
 * returned first, and a car's archive with verbose on (issue #48) held no
 * pipeline line to tell its link from its decoder.
 */
function view(over = {}) {
    return {
        _updateGamingOverlay() {},
        _byteRate: { sample() {} },
        _overlayEl: document.createElement('div'),
        _statsClosed: false,
        _showPerfStats: false,
        _perfDiag: true,
        _firstFrameRendered: true,
        _transport: 'webrtc-dc-tcp',
        _lastDiagLogMs: 0,
        _decodedFps: () => 58,
        _sampleDiagRates() {},
        _workerDiag: null,
        _diag: new PipelineDiag(2000),
        _presentedFps: 57,
        _dropsPerSec: 1,
        _formatStageTails: () => '',
        _gpuBehind: null,
        _mainThreadProbe: null,
        _e2eMeasuredStats: { count: 0 },
        _perfDiagLine: StreamView.prototype._perfDiagLine,
        _tracePerfWhileHidden: StreamView.prototype._tracePerfWhileHidden,
        ...over,
    };
}

function perfLines(log) {
    return log.mock.calls.map((c) => String(c[0])).filter((l) => l.startsWith('[perf] '));
}

describe('the [perf] trace with the stats card hidden', () => {
    afterEach(() => {
        vi.restoreAllMocks();
    });

    it('is written once a second while the card is off in settings', () => {
        const log = vi.spyOn(console, 'log').mockImplementation(() => {});
        const now = vi.spyOn(performance, 'now');
        const v = view();
        now.mockReturnValue(5000);
        StreamView.prototype._updateOverlay.call(v);
        now.mockReturnValue(5500); // the next 500 ms tick: too soon
        StreamView.prototype._updateOverlay.call(v);
        now.mockReturnValue(6100);
        StreamView.prototype._updateOverlay.call(v);
        const lines = perfLines(log);
        expect(lines.length).toBe(2);
        expect(lines[0]).toMatch(/^\[perf\] fps in\/out 58\/57 · arrival /);
        expect(v._overlayEl.style.display).toBe('none');
    });

    it('is written too when the card was closed with its ×, or has no element', () => {
        const log = vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(performance, 'now').mockReturnValue(9000);
        StreamView.prototype._updateOverlay.call(
            view({ _showPerfStats: true, _statsClosed: true }),
        );
        StreamView.prototype._updateOverlay.call(view({ _overlayEl: null }));
        expect(perfLines(log).length).toBe(2);
    });

    it('stays silent without verbose logs, before the first frame, and on webrtc-media', () => {
        const log = vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(performance, 'now').mockReturnValue(9000);
        StreamView.prototype._updateOverlay.call(view({ _perfDiag: false }));
        StreamView.prototype._updateOverlay.call(view({ _firstFrameRendered: false }));
        StreamView.prototype._updateOverlay.call(view({ _transport: 'webrtc-media' }));
        expect(perfLines(log)).toEqual([]);
    });
});
