/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * MWSR's zones from the native host (plan B, docs/design/upscaler-mwsr-b.md
 * §12 c) travel with their frame from the transport's onVideo to the renderer:
 * through handleVideoFrame, to the decode worker, or to decodeFrame, which
 * gives them to the renderer under the timestamp of the frame's chunk, as it
 * gives mwsr 3 its QP.
 */
const P = StreamView.prototype;

const ZONES = { cols: 32, rows: 18, scale: 2, codes: new Uint8Array(576).fill(3) };
// An H.264 delta frame, Annex B: one slice NAL.
const DELTA = new Uint8Array([0, 0, 0, 1, 0x41, 0x9a, 0x00, 0x10]);

function decodingView(over = {}) {
    return {
        decoderConfigured: true,
        pendingFrames: [],
        decoder: { state: 'configured', decodeQueueSize: 0, decode: vi.fn() },
        _referenceValid: true,
        stats: { dropped: 0, received: 0 },
        _diag: { noteDecodeQueue() {}, noteDrop() {}, arrivalAvgMs: 16 },
        DECODE_QUEUE_MAX: 8,
        _queueStallStart: 0,
        _lastChunkTs: 0,
        frameCount: 0,
        _noDescription: false,
        nalParser: { codec: 'h264' },
        _lastQueueFlushMs: 0,
        _renderer: {
            noteEncodedFrame: vi.fn(),
            wantsFrameQp: false,
            wantsFrameZones: true,
            noteFrameZones: vi.fn(),
        },
        _trackChunkSubmit: vi.fn(),
        _clientArriveStats: { addSample() {} },
        _tellDecodeQueue: vi.fn(),
        _flushDecodeQueue: vi.fn(),
        _handleDecoderError: vi.fn(),
        decodeFrame: P.decodeFrame,
        ...over,
    };
}

describe("StreamView — mwsr's zones ride with their frame", () => {
    beforeEach(() => {
        vi.stubGlobal(
            'EncodedVideoChunk',
            class {
                constructor(init) {
                    Object.assign(this, init);
                }
            },
        );
    });
    afterEach(() => {
        vi.unstubAllGlobals();
        vi.restoreAllMocks();
    });

    it("hands the transport's zones on with the frame", () => {
        const v = {
            _quitting: false,
            _fatalDecodeError: false,
            _stepper: null,
            _ultraMode: () => false,
            _transport: 'webrtc',
            _lastFrameId: -1,
            _firstFrameId: -1,
            _dropTestEvery: 0,
            _totalBytes: 0,
            _noteLinkArrival: vi.fn(),
            _stallDetector: { note: () => null },
            _processVideoFrame: vi.fn(),
        };
        P.handleVideoFrame.call(v, DELTA, false, 1000, 1, ZONES);
        expect(v._processVideoFrame.mock.calls[0][4]).toBe(ZONES);
        // a frame without them, from a transport that gives four arguments
        P.handleVideoFrame.call(v, DELTA, false, 1016, 2);
        expect(v._processVideoFrame.mock.calls[1][4]).toBe(null);
    });

    it('posts them to the decode worker beside the bytes', () => {
        const v = {
            _firstFrameProcessed: true,
            _useWorker: true,
            _videoWorker: { postMessage: vi.fn() },
        };
        P._processVideoFrame.call(v, DELTA.slice(), false, 1000, 0, ZONES);
        expect(v._videoWorker.postMessage.mock.calls[0][0].zones).toBe(ZONES);
    });

    it("gives them to the renderer under the chunk's timestamp", () => {
        const v = decodingView();
        P.decodeFrame.call(v, DELTA, false, 1000, 0, ZONES);
        expect(v._renderer.noteFrameZones).toHaveBeenCalledWith(1000000, ZONES);
        expect(v.decoder.decode.mock.calls[0][0].timestamp).toBe(1000000);
        // none for a frame without them, nor for a renderer that reads none
        P.decodeFrame.call(v, DELTA, false, 1016, 0, null);
        const w = decodingView({ _renderer: { ...v._renderer, wantsFrameZones: false } });
        w._renderer.noteFrameZones = vi.fn();
        P.decodeFrame.call(w, DELTA, false, 1000, 0, ZONES);
        expect(v._renderer.noteFrameZones).toHaveBeenCalledTimes(1);
        expect(w._renderer.noteFrameZones).not.toHaveBeenCalled();
    });

    it('keeps them with a frame that waits for the decoder', () => {
        const v = decodingView({ decoderConfigured: false });
        P.decodeFrame.call(v, DELTA, true, 1000, 0, ZONES);
        expect(v._renderer.noteFrameZones).not.toHaveBeenCalled();
        v.decoderConfigured = true;
        P.flushPendingFrames.call(v);
        expect(v._renderer.noteFrameZones).toHaveBeenCalledWith(1000000, ZONES);
    });
});
