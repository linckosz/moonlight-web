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
 * Whether this GPU keeps mwsr's vote one slope per 2×2 quad (MwsrShader's
 * mwsrQuadVerdict()): Direct3D's ddx_coarse and WARP do, Chrome on a Mac takes
 * dpdxCoarse per pixel (22/11/2026, an M3 and an M1 Pro), and WebGL's dFdx is
 * whatever the driver gives. One small draw when the renderer starts, decided
 * once: 'emulate' only where the native slopes are fine and the emulation's
 * coarse; anything else (an error, a lost device or context, a picture that
 * says neither, the time bound) leaves 'native', the texts as they were.
 *
 * What it finds stays on the page: one console line, the verdict and the time,
 * no GPU name and no pixel.
 */

import {
    MWSR_QUAD_PROBE_GLSL,
    MWSR_QUAD_PROBE_SIZE,
    MWSR_QUAD_PROBE_WGSL,
    mwsrQuadVerdict,
} from './MwsrShader.js';

/** How long a WebGPU probe may take before the renderer goes on without it. */
export const MWSR_QUAD_PROBE_TIMEOUT_MS = 250;

const N = MWSR_QUAD_PROBE_SIZE;

function note(api, verdict, ms) {
    console.info(
        `[mwsr] quad probe (${api}): ${verdict.native} slopes, ` +
            `${verdict.quad === 'emulate' ? 'emulated per quad' : 'kept'} (${ms.toFixed(1)} ms)`,
    );
}

/**
 * The probe on a WebGPU device. Its errors stay in its own error scopes, so
 * that none reaches the device's uncapturederror; its texture and buffer are
 * destroyed on every path. Resolves 'native' or 'emulate', never rejects.
 * @param {GPUDevice} device
 * @param {{timeoutMs?: number}} [opts]
 * @returns {Promise<'native'|'emulate'>}
 */
export async function mwsrQuadProbeWebGpu(device, { timeoutMs = MWSR_QUAD_PROBE_TIMEOUT_MS } = {}) {
    const t0 = performance.now();
    let tex = null;
    let buf = null;
    let timer = 0;
    const run = async () => {
        device.pushErrorScope('out-of-memory');
        device.pushErrorScope('validation');
        let scoped = 2;
        // A throw while drawing is held until the scopes are popped: a scope's
        // error, when there is one, is the one the probe fails with.
        let drawFailed = false;
        let drawError;
        try {
            tex = device.createTexture({
                size: [N, N],
                format: 'rgba8unorm',
                usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC,
            });
            buf = device.createBuffer({
                size: 256 * N,
                usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
            });
            const module = device.createShaderModule({ code: MWSR_QUAD_PROBE_WGSL });
            const pipeline = device.createRenderPipeline({
                layout: 'auto',
                vertex: { module, entryPoint: 'vs' },
                fragment: { module, entryPoint: 'fs', targets: [{ format: 'rgba8unorm' }] },
                primitive: { topology: 'triangle-list' },
            });
            const enc = device.createCommandEncoder();
            const pass = enc.beginRenderPass({
                colorAttachments: [
                    {
                        view: tex.createView(),
                        loadOp: 'clear',
                        storeOp: 'store',
                        clearValue: { r: 0, g: 0, b: 0, a: 0 },
                    },
                ],
            });
            pass.setPipeline(pipeline);
            pass.draw(3);
            pass.end();
            // rows of 256 bytes, the copy's alignment: 16 pixels of 4 bytes, padded
            enc.copyTextureToBuffer({ texture: tex }, { buffer: buf, bytesPerRow: 256 }, [N, N]);
            device.queue.submit([enc.finish()]);
        } catch (e) {
            drawFailed = true;
            drawError = e;
        }
        const errors = [];
        while (scoped-- > 0) errors.push(device.popErrorScope());
        const found = (await Promise.all(errors)).filter(Boolean);
        if (found.length) throw new Error('the probe drew with an error: ' + found[0].message);
        if (drawFailed) throw drawError;
        await buf.mapAsync(GPUMapMode.READ);
        const rows = new Uint8Array(buf.getMappedRange());
        const px = new Uint8Array(N * N * 4);
        for (let y = 0; y < N; y++) px.set(rows.subarray(y * 256, y * 256 + N * 4), y * N * 4);
        buf.unmap();
        return mwsrQuadVerdict(px);
    };
    const bound = new Promise((resolve) => {
        timer = setTimeout(() => resolve(null), timeoutMs);
    });
    // A probe that answers after the bound is let go: its rejection (the
    // buffer destroyed under its map) is caught here, its verdict dropped.
    const probe = run();
    probe.catch(() => {});
    let verdict = null;
    try {
        verdict = await Promise.race([probe, bound]);
    } catch (e) {
        console.info('[mwsr] quad probe (webgpu): kept, the probe failed: ' + (e && e.message));
        verdict = undefined;
    } finally {
        clearTimeout(timer);
        try {
            tex?.destroy();
            buf?.destroy();
        } catch {
            // a lost device has destroyed them already
        }
    }
    if (verdict === undefined) return 'native';
    if (verdict === null) {
        console.info(`[mwsr] quad probe (webgpu): kept, no answer within ${timeoutMs} ms`);
        return 'native';
    }
    note('webgpu', verdict, performance.now() - t0);
    return verdict.quad;
}

/**
 * The probe on a WebGL2 context, synchronous: its own framebuffer and program,
 * and every binding it touches given back as it was. A context lost before or
 * during it gives 'native'.
 * @param {WebGL2RenderingContext} gl
 * @param {string} vsSrc the renderer's full-screen triangle
 * @returns {'native'|'emulate'}
 */
export function mwsrQuadProbeWebGl(gl, vsSrc) {
    const t0 = performance.now();
    let saved;
    try {
        if (!gl || gl.isContextLost()) return 'native';
        saved = {
            fb: gl.getParameter(gl.FRAMEBUFFER_BINDING),
            readFb: gl.getParameter(gl.READ_FRAMEBUFFER_BINDING),
            viewport: gl.getParameter(gl.VIEWPORT),
            program: gl.getParameter(gl.CURRENT_PROGRAM),
            vao: gl.getParameter(gl.VERTEX_ARRAY_BINDING),
            active: gl.getParameter(gl.ACTIVE_TEXTURE),
            packAlign: gl.getParameter(gl.PACK_ALIGNMENT),
            packBuf: gl.getParameter(gl.PIXEL_PACK_BUFFER_BINDING),
        };
        gl.activeTexture(gl.TEXTURE0);
        saved.tex2d = gl.getParameter(gl.TEXTURE_BINDING_2D);
        gl.activeTexture(saved.active);
    } catch {
        // a context that cannot even say its state: nothing drawn, nothing to give back
        return 'native';
    }
    let tex = null;
    let fb = null;
    let vs = null;
    let fs = null;
    let prog = null;
    let vao = null;
    let verdict = null;
    try {
        vs = gl.createShader(gl.VERTEX_SHADER);
        fs = gl.createShader(gl.FRAGMENT_SHADER);
        for (const [sh, src] of [
            [vs, vsSrc],
            [fs, MWSR_QUAD_PROBE_GLSL],
        ]) {
            gl.shaderSource(sh, src);
            gl.compileShader(sh);
            if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS))
                throw new Error('the probe did not compile');
        }
        prog = gl.createProgram();
        gl.attachShader(prog, vs);
        gl.attachShader(prog, fs);
        gl.linkProgram(prog);
        if (!gl.getProgramParameter(prog, gl.LINK_STATUS))
            throw new Error('the probe did not link');
        gl.activeTexture(gl.TEXTURE0);
        tex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, N, N, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        fb = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
        if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE)
            throw new Error('the probe has no framebuffer');
        vao = gl.createVertexArray();
        gl.bindVertexArray(vao);
        gl.viewport(0, 0, N, N);
        gl.useProgram(prog);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
        gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
        gl.pixelStorei(gl.PACK_ALIGNMENT, 4);
        const px = new Uint8Array(N * N * 4);
        gl.readPixels(0, 0, N, N, gl.RGBA, gl.UNSIGNED_BYTE, px);
        if (gl.isContextLost()) throw new Error('the context was lost');
        verdict = mwsrQuadVerdict(px);
    } catch (e) {
        console.info('[mwsr] quad probe (webgl): kept, the probe failed: ' + (e && e.message));
    } finally {
        // Every binding given back, then what the probe made deleted; a lost
        // context has none of either, and a renderer's next draw sets its own.
        try {
            if (!gl.isContextLost()) {
                gl.bindFramebuffer(gl.FRAMEBUFFER, saved.fb);
                gl.bindFramebuffer(gl.READ_FRAMEBUFFER, saved.readFb);
                gl.viewport(
                    saved.viewport[0],
                    saved.viewport[1],
                    saved.viewport[2],
                    saved.viewport[3],
                );
                gl.useProgram(saved.program);
                gl.bindVertexArray(saved.vao);
                gl.activeTexture(gl.TEXTURE0);
                gl.bindTexture(gl.TEXTURE_2D, saved.tex2d);
                gl.activeTexture(saved.active);
                gl.pixelStorei(gl.PACK_ALIGNMENT, saved.packAlign);
                gl.bindBuffer(gl.PIXEL_PACK_BUFFER, saved.packBuf);
                if (vao) gl.deleteVertexArray(vao);
                if (fb) gl.deleteFramebuffer(fb);
                if (tex) gl.deleteTexture(tex);
                if (prog) gl.deleteProgram(prog);
                if (vs) gl.deleteShader(vs);
                if (fs) gl.deleteShader(fs);
            }
        } catch {
            // nothing more to give back
        }
    }
    if (!verdict) return 'native';
    note('webgl', verdict, performance.now() - t0);
    return verdict.quad;
}
