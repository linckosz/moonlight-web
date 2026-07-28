import { WebRtcDataChannel } from '../api/WebRtcDataChannel.js';
import { WebRtcMedia } from '../api/WebRtcMedia.js';

/**
 * The transport surface `StreamView` programs against.
 *
 * `this.webrtc` is one of two classes with deliberately different capabilities:
 *
 * - `WebRtcDataChannel` — frames arrive as DataChannel messages and are decoded
 *   by WebCodecs, so it owns the frame/loss/IDR callbacks.
 * - `WebRtcMedia` — frames arrive on a native RTP media track rendered by
 *   `<video>`, so it owns the video-element and jitter-buffer knobs instead.
 *
 * Members that exist on only one side are declared **optional** here. That is
 * the accurate contract: every call site guards them with a `typeof … ===
 * 'function'` check or a `_transport` branch, and the optionality keeps those
 * guards meaningful instead of typing them away.
 */
export type StreamTransport = (WebRtcDataChannel | WebRtcMedia) & {
    // ── webrtc-media only ────────────────────────────────────────────────────
    /** Bind the RTP video track to the `<video>` element. */
    setVideoElement?: (videoEl: HTMLVideoElement) => void;
    /** Adaptive playout delay, driven by `JitterController`. */
    setVideoJitterBufferTarget?: (ms: number) => void;

    // ── DataChannel / WSS only ───────────────────────────────────────────────
    /** Reassembled Annex B frame ready for `VideoDecoder`. */
    onVideo?:
        | ((frame: Uint8Array, isKeyframe: boolean, backendTs: number, frameId: number) => void)
        | null;
    /** A frame was dropped during reassembly — reference is no longer valid. */
    onFrameLoss?: ((frameId: number, wasKeyframe: boolean) => void) | null;
    /** The transport sent an IDR request (throttled) — feeds congestion stats. */
    onIdrRequested?: ((reason: string) => void) | null;

    // ── Both transports ──────────────────────────────────────────────────────
    // Delivered as an exit notice on the input DataChannel, sent by the relay
    // (DataChannelRelay / MediaTrackRelay) just before it closes the channel.
    /** Session taken over by another device. */
    onTakeover?: (() => void) | null;
    /** This device's access was revoked by an admin. */
    onRevoked?: (() => void) | null;

    /**
     * Surface ICE failure as an error so MoonlightApp can relaunch on the next
     * transport. `WebRtcMedia` deliberately ignores it (no in-session fallback).
     */
    _chainFallback?: boolean;
};
