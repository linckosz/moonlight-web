/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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
