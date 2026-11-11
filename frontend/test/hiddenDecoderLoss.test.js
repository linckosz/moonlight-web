/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, afterEach } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';
import { DecoderErrorTrail } from '../js/stream/DecoderErrorTrail.js';

/**
 * iOS takes the hardware decoder back as the app goes to the background:
 * "Decoder failure" a few hundred ms after "visibility hidden" (iPhone, prod
 * log 07-11/11, 13 of 13). The decoder is rebuilt without spending a recovery
 * attempt, the error is not logged as one, and the page asks for a keyframe
 * the moment it is back.
 */
const P = StreamView.prototype;

function setVisibility(state) {
    Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => state });
}

function view(over = {}) {
    return {
        stats: { received: 100, decoded: 100, dropped: 0, recoveries: 0 },
        videoCodec: 'hevc',
        _quitting: false,
        _manualQuitting: false,
        _decoderRecovering: false,
        _recoveryAttempts: 0,
        MAX_RECOVERY_ATTEMPTS: 10,
        _decoderTrail: new DecoderErrorTrail(),
        _diag: { noteRecovery: vi.fn() },
        decoder: { close: vi.fn() },
        frameQueue: [],
        pendingFrames: [],
        _framePacer: null,
        _stallDetector: { reset: vi.fn() },
        nalParser: { reset: vi.fn() },
        _clearPacingTimer: vi.fn(),
        setupDecoder: vi.fn(),
        setStatus: vi.fn(),
        _requestIdr: vi.fn(),
        _onDecoderError: P._onDecoderError,
        _handleDecoderError: P._handleDecoderError,
        ...over,
    };
}

const failure = { name: 'EncodingError', code: 0, message: 'Decoder failure' };

describe('StreamView — the decoder lost on a hidden page', () => {
    afterEach(() => {
        setVisibility('visible');
        vi.restoreAllMocks();
    });

    it('rebuilds the decoder without counting an attempt, and logs no error', () => {
        setVisibility('hidden');
        const err = vi.spyOn(console, 'error').mockImplementation(() => {});
        const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
        const v = view({ _recoveryAttempts: 3 });
        v._onDecoderError(failure);
        expect(v._recoveryAttempts).toBe(3);
        expect(v.setupDecoder).toHaveBeenCalledOnce();
        expect(v._referenceValid).toBe(false);
        expect(err).not.toHaveBeenCalled();
        expect(warn.mock.calls.some((c) => /lost while the page is hidden/.test(c[0]))).toBe(true);
        // The context line is still there, for the study.
        expect(warn.mock.calls.some((c) => /Decoder error context/.test(c[0]))).toBe(true);
    });

    it('forgives one lost decoder per spell in the background, not a decoder that keeps failing', () => {
        setVisibility('hidden');
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        const v = view();
        // A desktop tab decodes on while hidden: the second error counts.
        v._onDecoderError(failure);
        v._onDecoderError(failure);
        v._onDecoderError(failure);
        expect(v._recoveryAttempts).toBe(2);
        // Back, then hidden again: the next spell forgives its own.
        v._resyncAfterHidden = P._resyncAfterHidden;
        Object.assign(v, {
            connected: true,
            _videoWorker: null,
            _link: null,
            webrtc: null,
            _gamepadManager: null,
            _stepper: null,
        });
        vi.spyOn(console, 'log').mockImplementation(() => {});
        v._resyncAfterHidden();
        v._onDecoderError(failure);
        expect(v._recoveryAttempts).toBe(2);
    });

    it('still counts and logs an error on a visible page', () => {
        setVisibility('visible');
        const err = vi.spyOn(console, 'error').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        const v = view();
        v._onDecoderError(failure);
        expect(v._recoveryAttempts).toBe(1);
        expect(err).toHaveBeenCalled();
    });
});

describe('StreamView — back from the background', () => {
    function back(over) {
        vi.spyOn(console, 'log').mockImplementation(() => {});
        const v = {
            connected: true,
            _stallDetector: { reset: vi.fn() },
            _clearPacingTimer: vi.fn(),
            _framePacer: null,
            _videoWorker: null,
            _link: null,
            webrtc: null,
            _gamepadManager: null,
            _stepper: null,
            _referenceValid: true,
            _requestIdr: vi.fn(),
            _resyncAfterHidden: P._resyncAfterHidden,
            ...over,
        };
        v._resyncAfterHidden();
        return v;
    }

    afterEach(() => vi.restoreAllMocks());

    it('asks for a keyframe when the decoder lost its reference meanwhile', () => {
        const v = back({ _referenceValid: false });
        expect(v._requestIdr).toHaveBeenCalledWith('back from the background');
    });

    it('asks for nothing when the reference held: gaps heal on their own path', () => {
        const v = back({ _referenceValid: true });
        expect(v._requestIdr).not.toHaveBeenCalled();
    });

    it('leaves a worker-decoded stream to the worker', () => {
        const v = back({ _referenceValid: false, _videoWorker: { postMessage: vi.fn() } });
        expect(v._requestIdr).not.toHaveBeenCalled();
    });
});
