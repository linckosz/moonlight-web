/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 MoonlightWeb contributors
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
 * The microphone on the WebSocket transport, read on the audio thread: the
 * first channel of its input, handed to the page in frames of FRAME samples
 * (20 ms at 48 kHz), the unit the page encodes and sends (MicSocketSender.js).
 * Writes nothing to its output: it is connected only so that the graph runs it.
 */
const FRAME = 960;

class MicCaptureProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        this._frame = new Float32Array(FRAME);
        this._filled = 0;
        this._running = true;
        this.port.onmessage = (e) => {
            if (e.data === 'stop') this._running = false;
        };
    }

    process(inputs) {
        if (!this._running) return false;
        const channel = inputs[0] && inputs[0][0];
        if (!channel) return true;
        for (let i = 0; i < channel.length; i++) {
            this._frame[this._filled++] = channel[i];
            if (this._filled === FRAME) {
                this.port.postMessage(this._frame, [this._frame.buffer]);
                this._frame = new Float32Array(FRAME);
                this._filled = 0;
            }
        }
        return true;
    }
}

registerProcessor('mw-mic-capture', MicCaptureProcessor);
