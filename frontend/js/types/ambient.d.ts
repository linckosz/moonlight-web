/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * Ambient declarations for browser globals TypeScript's bundled `DOM` /
 * `WebWorker` libs do not ship.
 *
 * Types only — this file emits nothing and is never loaded at runtime. It
 * exists so `npm run typecheck` reports genuine structural problems instead of
 * drowning them in "Cannot find name" noise for APIs we deliberately use behind
 * capability checks.
 *
 * Kept intentionally minimal: only the members the codebase actually touches
 * are declared, so a wrong guess about an unused part of these specs can never
 * mask a real error. Widen as usage grows.
 */

// -----------------------------------------------------------------------------
// WebGPU — bitflag namespaces used by WebGpuRenderer.js.
//
// The `GPU*` interfaces themselves fall back to `any` (noImplicitAny is off),
// but these constants are value-space globals and must be declared to resolve.
// -----------------------------------------------------------------------------

declare namespace GPUShaderStage {
    const VERTEX: number;
    const FRAGMENT: number;
    const COMPUTE: number;
}

declare namespace GPUBufferUsage {
    const MAP_READ: number;
    const MAP_WRITE: number;
    const COPY_SRC: number;
    const COPY_DST: number;
    const INDEX: number;
    const VERTEX: number;
    const UNIFORM: number;
    const STORAGE: number;
    const INDIRECT: number;
    const QUERY_RESOLVE: number;
}

declare namespace GPUTextureUsage {
    const COPY_SRC: number;
    const COPY_DST: number;
    const TEXTURE_BINDING: number;
    const STORAGE_BINDING: number;
    const RENDER_ATTACHMENT: number;
}

// -----------------------------------------------------------------------------
// Insertable Streams for MediaStreamTrack — Chromium-only, feature-detected
// before use (VideoElementRenderer.js / the webrtc-media path).
// -----------------------------------------------------------------------------

declare class MediaStreamTrackGenerator extends MediaStreamTrack {
    constructor(init: { kind: 'audio' | 'video' });
    readonly writable: WritableStream;
}

// -----------------------------------------------------------------------------
// AudioWorklet global scope — visible only inside an AudioWorkletProcessor
// module (audio-processor.js), which TS type-checks against the window/worker
// scope and so cannot see these.
// -----------------------------------------------------------------------------

declare abstract class AudioWorkletProcessor {
    readonly port: MessagePort;
    constructor(options?: any);
    process(
        inputs: Float32Array[][],
        outputs: Float32Array[][],
        parameters: Record<string, Float32Array>,
    ): boolean;
}

declare function registerProcessor(
    name: string,
    processorCtor: new (options?: any) => AudioWorkletProcessor,
): void;

// -----------------------------------------------------------------------------
// Vendor-prefixed / not-yet-standard members the codebase feature-detects.
// All are optional, so the `typeof x === 'function'` guards around them stay
// meaningful instead of being typed away.
// -----------------------------------------------------------------------------

interface Navigator {
    /** WebGPU entry point — absent outside Chromium and on insecure origins. */
    readonly gpu?: any;
    /**
     * Keyboard Lock / getLayoutMap — Chromium only. Extends EventTarget: the
     * layout cache re-reads the map on the `layoutchange` event.
     */
    readonly keyboard?: EventTarget & {
        lock(keyCodes?: string[]): Promise<void>;
        unlock(): void;
        getLayoutMap(): Promise<Map<string, string>>;
    };
    /** iOS Safari home-screen web app flag. */
    readonly standalone?: boolean;
}

interface HTMLVideoElement {
    /** iOS Safari native video-player fullscreen. */
    webkitExitFullscreen?: () => void;
    readonly webkitDisplayingFullscreen?: boolean;
}

interface RTCRtpReceiver {
    /** Chromium playout-delay control, driven by `JitterController`. */
    setJitterBufferMinimumDelay?: (delayHint: number | null) => void;
}

interface Window {
    /** Legacy constructor still required to unlock audio on older iOS Safari. */
    webkitAudioContext?: typeof AudioContext;
    /** Click-to-photon probe of the visible stream (debug builds, Windows host). */
    mwLatency?: import('../stream/LatencyProbe.js').LatencyProbe | null;
    /** Every click-to-photon entry measured in this page, in order. */
    mwLatencyResults?: any[];
}

interface TouchEvent {
    /** iOS Safari pinch-gesture scale factor (non-standard, Safari only). */
    readonly scale?: number;
}

// -----------------------------------------------------------------------------
// WebCodecs colour space — TypeScript's bundled lib.dom predates the wide-gamut
// and HDR additions to the spec: its `VideoColorPrimaries`, `VideoMatrix-
// Coefficients` and `VideoTransferCharacteristics` unions have no 'bt2020',
// 'bt2020-ncl' or 'pq', all of which Chromium ships and the HDR decode path
// requires. The lib aliases cannot be widened in place (duplicate type-alias
// declarations are an error, and interface merging cannot loosen a property),
// so the spec-current shape is declared under its own name here.
// -----------------------------------------------------------------------------

interface HdrVideoColorSpaceInit {
    primaries?: 'bt709' | 'bt470bg' | 'smpte170m' | 'bt2020' | 'smpte432' | null;
    transfer?: 'bt709' | 'smpte170m' | 'iec61966-2-1' | 'linear' | 'pq' | 'hlg' | null;
    matrix?: 'rgb' | 'bt709' | 'bt470bg' | 'smpte170m' | 'bt2020-ncl' | null;
    fullRange?: boolean | null;
}

/** `VideoDecoderConfig` with the spec-current colour space. */
type HdrVideoDecoderConfig = Omit<VideoDecoderConfig, 'colorSpace'> & {
    colorSpace?: HdrVideoColorSpaceInit;
};
