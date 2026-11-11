/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, afterEach } from 'vitest';
import { StreamView, IOS_ERROR_BEFORE_HIDDEN_MS } from '../js/ui/StreamView.js';
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

describe('StreamView — the error iOS throws just before the page goes hidden', () => {
    let now = 0;
    function at(ms) {
        now = ms;
    }
    function v2(over = {}) {
        vi.spyOn(performance, 'now').mockImplementation(() => now);
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'error').mockImplementation(() => {});
        return view({ _noteHiddenAfterError: P._noteHiddenAfterError, ...over });
    }

    afterEach(() => {
        setVisibility('visible');
        vi.restoreAllMocks();
    });

    it('gives the attempt back when hidden comes 107 ms after the error', () => {
        setVisibility('visible');
        const v = v2({ _recoveryAttempts: 2 });
        at(1000);
        v._onDecoderError(failure);
        expect(v._recoveryAttempts).toBe(3);
        setVisibility('hidden');
        at(1107);
        expect(v._noteHiddenAfterError(now)).toBe(true);
        expect(v._recoveryAttempts).toBe(2);
        expect(
            console.warn.mock.calls.some((c) => /107 ms before the page went hidden/.test(c[0])),
        ).toBe(true);
    });

    it('takes the pardon of the spell: a second error while hidden counts', () => {
        setVisibility('visible');
        const v = v2();
        at(1000);
        v._onDecoderError(failure);
        setVisibility('hidden');
        at(1044);
        v._noteHiddenAfterError(now);
        expect(v._recoveryAttempts).toBe(0);
        at(2000);
        v._onDecoderError(failure);
        expect(v._recoveryAttempts).toBe(1);
    });

    it('keeps the count when hidden comes too long after the error', () => {
        setVisibility('visible');
        const v = v2();
        at(1000);
        v._onDecoderError(failure);
        at(1000 + IOS_ERROR_BEFORE_HIDDEN_MS + 1);
        expect(v._noteHiddenAfterError(now)).toBe(false);
        expect(v._recoveryAttempts).toBe(1);
    });

    it('gives back an attempt only once, and never below zero', () => {
        setVisibility('visible');
        const v = v2();
        at(1000);
        v._onDecoderError(failure);
        // A frame decoded before the event already reset the count.
        v._recoveryAttempts = 0;
        at(1050);
        expect(v._noteHiddenAfterError(now)).toBe(true);
        expect(v._recoveryAttempts).toBe(0);
        at(1060);
        expect(v._noteHiddenAfterError(now)).toBe(false);
    });

    it('does nothing when no error came before', () => {
        const v = v2({ _recoveryAttempts: 4 });
        at(500);
        expect(v._noteHiddenAfterError(now)).toBe(false);
        expect(v._recoveryAttempts).toBe(4);
    });
});
