/**
 * ESM wrapper around the vendored `opus-decoder` UMD bundle (eshaz /
 * wasm-audio-decoders). The bundle embeds its WASM as base64, so no extra
 * fetch is needed — it works offline / on LAN.
 *
 * Used as a fallback Opus decoder when WebCodecs AudioDecoder is unavailable,
 * notably on iOS (where every browser, including Chrome, runs WebKit).
 *
 * The UMD has no ESM exports; importing it for its side effect installs the
 * decoder on `globalThis['opus-decoder']`. We re-export the synchronous
 * main-thread `OpusDecoder` (the WebWorker variant needs an extra dependency
 * we don't ship).
 */
import '../vendor/opus-decoder.min.js';

const lib = globalThis['opus-decoder'];

/** @type {undefined | (new (opts?: object) => object)} */
export const OpusDecoder = lib ? lib.OpusDecoder : undefined;
