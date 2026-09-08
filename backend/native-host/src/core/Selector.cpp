/*
 * MoonlightWeb — native capture & encoding engine.
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

#include "Selector.h"
#include "Log.h"

#include <algorithm>

namespace mw::native {
namespace {

const DisplayInfo* findDisplay(const Capabilities& caps, int displayId)
{
    // A displayId of -1 means "whatever the user is most likely to want", which
    // is the primary display. This is what makes the single-display case a
    // one-click experience: nothing to choose, nothing to pass.
    if (displayId < 0) {
        for (const DisplayInfo& d : caps.displays) {
            if (d.primary) return &d;
        }
        return caps.displays.empty() ? nullptr : &caps.displays.front();
    }

    for (const DisplayInfo& d : caps.displays) {
        if (d.id == displayId) return &d;
    }
    return nullptr;
}

bool gpuHasCodec(const GpuInfo& gpu, Codec codec)
{
    return std::find(gpu.codecs.begin(), gpu.codecs.end(), codec) != gpu.codecs.end();
}

/// The first GPU that can genuinely encode. Only reached when the display's own
/// GPU cannot — see Selection::crossGpuCopy.
///
/// "Can encode" requires a codec, not merely an encoder API. The distinction is
/// not academic: this bench has an AMD iGPU that reports the AMF runtime with an
/// empty codec list (AMF's own capability query is not written yet). Falling
/// back to it would pay for a cross-GPU copy and then fail codec negotiation
/// anyway — abandoning a GPU that could actually have done the job.
const GpuInfo* firstEncodingGpu(const Capabilities& caps)
{
    for (const GpuInfo& gpu : caps.gpus) {
        if (!gpu.encoders.empty() && !gpu.codecs.empty()) return &gpu;
    }
    return nullptr;
}

bool hasCodec(const std::vector<Codec>& codecs, Codec codec)
{
    return std::find(codecs.begin(), codecs.end(), codec) != codecs.end();
}

} // namespace

/// Hardware before software, then the platform probe's own order.
///
/// "Hardware first" is not a preference, it is the difference between a stream
/// and a slideshow on the machines this tier exists for. A Media Foundation
/// transform backed by fixed-function silicon costs the CPU nothing; OpenH264
/// costs it every macroblock, on a machine that by definition had no encoder to
/// spare.
const FallbackEncoder* bestFallback(const Capabilities& caps)
{
    const FallbackEncoder* best = nullptr;
    for (const FallbackEncoder& fb : caps.fallbacks) {
        if (fb.api == EncoderApi::None || fb.codecs.empty()) continue;
        if (!best) {
            best = &fb;
            continue;
        }
        if (fb.hardware && !best->hardware) best = &fb;
    }
    return best;
}

bool select(const Capabilities& caps, const SessionConfig& config, Selection& out,
            std::string& error)
{
    // Every field is decided below or defaulted here — a Selection handed in
    // twice must not carry the first answer's fallback flags into the second
    // (caught by the test suite, 07/09/2026).
    out = Selection{};

    if (config.clientCodecs.empty()) {
        // Not a default we can invent: an empty list would have the engine
        // guessing what the browser can decode, and a wrong guess is a black
        // screen rather than a clean refusal.
        error = "the client listed no codecs it can decode";
        return false;
    }

    out.display = findDisplay(caps, config.displayId);
    if (!out.display) {
        error = "display " + std::to_string(config.displayId) + " does not exist";
        return false;
    }

    // ── GPU: the display's own, unless it cannot encode ──────────────────────
    //
    // Capturing on GPU A to encode on GPU B costs a VRAM→RAM→VRAM round trip
    // that dwarfs every other cost in the pipeline, so the display's own GPU
    // wins whenever it can encode at all — even if another GPU in the machine
    // would encode "better".
    out.gpu = caps.gpuFor(*out.display);
    out.crossGpuCopy = false;

    /// Set only on the last-resort path below; null means the codec walk reads
    /// the GPU's list, as it always has.
    const FallbackEncoder* fallback = nullptr;

    // The bench may name the encoder's GPU outright — that is how an encoder
    // that drives no display (an iGPU beside a discrete card) gets measured at
    // all. A real session never sets this. The copy it costs is declared, and
    // the GPU still has to be able to encode: forcing a GPU without an encoder
    // would fail at init with a vendor error that says nothing.
    if (config.encodeGpuId >= 0) {
        const GpuInfo* forced = nullptr;
        for (const GpuInfo& gpu : caps.gpus)
            if (gpu.id == config.encodeGpuId) forced = &gpu;
        if (!forced) {
            error = "GPU " + std::to_string(config.encodeGpuId) + " does not exist";
            return false;
        }
        if (forced->encoders.empty() || forced->codecs.empty()) {
            error = "GPU " + std::to_string(config.encodeGpuId) + " ('" + forced->name +
                    "') has no usable encoder";
            return false;
        }
        out.crossGpuCopy = out.gpu != forced;
        if (out.crossGpuCopy)
            log::warning("[native] encoder GPU forced to '" + forced->name +
                         "' (bench) — a cross-GPU copy per frame");
        out.gpu = forced;
    }

    // Same rule as the fallback below: an encoder with no codec cannot encode,
    // so a display whose GPU is in that state must look elsewhere rather than
    // fail codec negotiation a few lines later.
    if (!out.gpu || out.gpu->encoders.empty() || out.gpu->codecs.empty()) {
        const GpuInfo* other = firstEncodingGpu(caps);
        if (other) {
            // Deliberate, and always worth saying out loud: this is the one case
            // where the zero-copy promise is given up, and a silent regression
            // here would look like "the engine just got slower".
            out.crossGpuCopy = (out.gpu != nullptr);
            log::warning(std::string("[native] display's GPU cannot encode — falling back to '") +
                         other->name + "' with a cross-GPU copy per frame");
            out.gpu = other;
        } else {
            // ── Nothing in this machine encodes. The fallback tier ───────────
            //
            // Reached only here, which is the whole design: while any GPU can
            // encode, this branch does not exist and every selection is what it
            // always was.
            //
            // `out.gpu` deliberately stays the display's own adapter, null or
            // not. Capture and colour conversion still run there — it is only
            // the encoder that moved off it — and pretending otherwise would
            // send capture to the wrong adapter on a multi-GPU machine whose
            // cards happen to be encoder-less.
            fallback = bestFallback(caps);
            if (!fallback) {
                error = "no GPU on this machine has a usable encoder, and no fallback "
                        "encoder is available either";
                return false;
            }
            out.fallbackEncoder = true;
            out.cpuEncoder = !fallback->hardware;
            out.crossGpuCopy = false;
        }
    }

    // ── Codec: the client's preference order, filtered by the GPU ────────────
    //
    // The client's order is authoritative, not ours: it already reflects what
    // that browser decodes in hardware. Walking it in order and taking the
    // first the GPU can also produce is the whole of §27 — and is why no codec
    // question is ever put to the user.
    //
    // 4:4:4 narrows that walk first. The capability is per codec (NVENC has it
    // on H.264 and HEVC, not AV1), so a client preferring AV1 with 4:4:4 on
    // must land on the first codec that can actually carry it, not on AV1
    // with the chroma silently dropped — the setting exists for text, and a
    // stream that ignores it looks broken. Only when no shared codec has 4:4:4
    // does the walk fall back to the plain one, and the session says 4:2:0.
    //
    // Whose list is walked is the only thing the fallback tier changes here. It
    // is named once, so every rule below — the preference order, the 4:4:4
    // narrowing, the "no codec in common" refusal — is the same code on both
    // paths and cannot drift.
    const std::vector<Codec>& available = fallback ? fallback->codecs : out.gpu->codecs;
    const std::string encoderName = fallback ? fallback->name : (out.gpu ? out.gpu->name : "");

    bool found = false;
    out.yuv444 = false;
    if (config.yuv444) {
        for (Codec candidate : config.clientCodecs) {
            // A fallback encoder carries no 4:4:4: Media Foundation's H.264
            // transforms take NV12 and OpenH264 encodes 4:2:0 only. So this
            // narrowing pass simply finds nothing and the plain walk below
            // decides — which is the honest outcome, not a special case.
            if (fallback) break;
            if (!gpuHasCodec(*out.gpu, candidate) || !out.gpu->supports444(candidate)) continue;
            out.codec = candidate;
            out.yuv444 = true;
            found = true;
            break;
        }
    }
    for (Codec candidate : config.clientCodecs) {
        if (found) break;
        if (!hasCodec(available, candidate)) continue;
        out.codec = candidate;
        found = true;
    }
    if (!found) {
        error = fallback ? "this browser decodes no codec the fallback encoder can produce"
                         : "this GPU and this browser have no video codec in common";
        return false;
    }
    if (config.yuv444 && !out.yuv444) {
        log::info(std::string("[native] 4:4:4 requested but no codec this browser and '") +
                  encoderName + "' share can carry it — streaming 4:2:0 " + toString(out.codec));
    } else if (out.yuv444 && out.codec != config.clientCodecs.front() &&
               gpuHasCodec(*out.gpu, config.clientCodecs.front())) {
        log::info(std::string("[native] 4:4:4 steers the codec to ") + toString(out.codec) + " — " +
                  toString(config.clientCodecs.front()) + " has no 4:4:4 on this encoder");
    }

    out.encoder = fallback ? fallback->api : out.gpu->encoders.front();

    if (fallback) {
        // Said out loud, once, at the only moment it can be said accurately.
        // This is a machine that would have been refused a native stream
        // altogether until now, so the line has to name what saved it and at
        // what cost — a reader who sees "software" and no explanation will go
        // looking for the setting that turned the GPU off.
        log::warning(std::string("[native] no GPU on this machine can encode — falling back to ") +
                     fallback->name +
                     (fallback->hardware ? " (hardware, via the OS)" : " (on the CPU)"));
    }

    // ── HDR: only when it is real all the way through ────────────────────────
    //
    // Asked for is not the same as achievable. Rather than fail — the user
    // asked to stream, not to negotiate — the session runs SDR and reports it,
    // and the stats overlay is where the difference shows.
    //
    // The fallback tier never carries it: 10-bit is a second reason for a weak
    // machine to fall behind, and a browser that is handed PQ it cannot place
    // shows a washed-out picture rather than an error (§21.10).
    out.hdr = config.hdr && !fallback && out.display->hdrActive && out.gpu->supports10Bit &&
              (out.codec == Codec::Hevc || out.codec == Codec::Av1);
    if (config.hdr && !out.hdr) {
        log::info("[native] HDR requested but not achievable here — streaming SDR");
    }

    // ── HDR and 4:4:4 are exclusive, and HDR wins ───────────────────────────
    //
    // 10-bit 4:4:4 exists in HEVC (Y410, profile 4) and no browser displays it:
    // Chrome 152 accepts `hvc1.4.156`, decodes it in hardware and renders a
    // green rectangle (measured 04/09/2026). So the pair has no encoding that
    // could be watched, and the conversion pass refuses it outright rather
    // than produce one.
    //
    // Granting both here would therefore kill the session at init() — the same
    // shape as bug B7, where a capability nothing downstream honoured turned a
    // ticked box into a dead stream. HDR is the one kept: it changes every
    // pixel of a picture, where 4:4:4 changes the edges of text, and it is the
    // one the user can see is missing.
    if (out.hdr && out.yuv444) {
        out.yuv444 = false;
        log::info("[native] 4:4:4 and HDR cannot ride together (no browser decodes 10-bit "
                  "4:4:4) — keeping HDR, streaming 4:2:0");
    }

    // ── Geometry: zero means "native", which is the default ─────────────────
    out.width = config.width > 0 ? config.width : out.display->width;
    out.height = config.height > 0 ? config.height : out.display->height;

    // The fallback tier never upscales. A client whose setting is 1440p asking
    // a 1080p display would have a machine with no encoder to spare convert and
    // encode 1.8× the pixels for no information at all — measured: the
    // Snapdragon's transform took 21 ms a picture at 1440p, the Debian VM's CPU
    // encoder likewise (07/09/2026). The browser scales the picture up itself,
    // and does it better than a CPU under load would.
    if (out.fallbackEncoder && out.display->width > 0 && out.display->height > 0 &&
        (out.width > out.display->width || out.height > out.display->height)) {
        log::info("[native] " + std::to_string(out.width) + "x" + std::to_string(out.height) +
                  " asked of a " + std::to_string(out.display->width) + "x" +
                  std::to_string(out.display->height) +
                  " display on the fallback encoder — streaming the display's own size, no "
                  "upscaling");
        out.width = out.display->width;
        out.height = out.display->height;
    }

    if (config.fps > 0) {
        out.fps = config.fps;
    } else {
        // Round to nearest: a 143.98 Hz panel should stream at 144, not 143.
        out.fps = (out.display->refreshMilliHz + 500) / 1000;
    }
    if (out.fps <= 0) out.fps = 60;

    return true;
}

} // namespace mw::native
