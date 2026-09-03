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

} // namespace

bool select(const Capabilities& caps, const SessionConfig& config, Selection& out,
            std::string& error)
{
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

    // Same rule as the fallback below: an encoder with no codec cannot encode,
    // so a display whose GPU is in that state must look elsewhere rather than
    // fail codec negotiation a few lines later.
    if (!out.gpu || out.gpu->encoders.empty() || out.gpu->codecs.empty()) {
        const GpuInfo* fallback = firstEncodingGpu(caps);
        if (!fallback) {
            error = "no GPU on this machine has a usable encoder";
            return false;
        }
        // Deliberate, and always worth saying out loud: this is the one case
        // where the zero-copy promise is given up, and a silent regression here
        // would look like "the engine just got slower".
        out.crossGpuCopy = (out.gpu != nullptr);
        log::warning(std::string("[native] display's GPU cannot encode — falling back to '") +
                     fallback->name + "' with a cross-GPU copy per frame");
        out.gpu = fallback;
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
    bool found = false;
    out.yuv444 = false;
    if (config.yuv444) {
        for (Codec candidate : config.clientCodecs) {
            if (!gpuHasCodec(*out.gpu, candidate) || !out.gpu->supports444(candidate)) continue;
            out.codec = candidate;
            out.yuv444 = true;
            found = true;
            break;
        }
    }
    for (Codec candidate : config.clientCodecs) {
        if (found) break;
        if (!gpuHasCodec(*out.gpu, candidate)) continue;
        out.codec = candidate;
        found = true;
    }
    if (!found) {
        error = "this GPU and this browser have no video codec in common";
        return false;
    }
    if (config.yuv444 && !out.yuv444) {
        log::info(std::string("[native] 4:4:4 requested but no codec this browser and '") +
                  out.gpu->name + "' share can carry it — streaming 4:2:0 " + toString(out.codec));
    } else if (out.yuv444 && out.codec != config.clientCodecs.front() &&
               gpuHasCodec(*out.gpu, config.clientCodecs.front())) {
        log::info(std::string("[native] 4:4:4 steers the codec to ") + toString(out.codec) + " — " +
                  toString(config.clientCodecs.front()) + " has no 4:4:4 on this encoder");
    }

    out.encoder = out.gpu->encoders.front();

    // ── HDR: only when it is real all the way through ────────────────────────
    //
    // Asked for is not the same as achievable. Rather than fail — the user
    // asked to stream, not to negotiate — the session runs SDR and reports it,
    // and the stats overlay is where the difference shows.
    out.hdr = config.hdr && out.display->hdrActive && out.gpu->supports10Bit &&
              (out.codec == Codec::Hevc || out.codec == Codec::Av1);
    if (config.hdr && !out.hdr) {
        log::info("[native] HDR requested but not achievable here — streaming SDR");
    }

    // ── Geometry: zero means "native", which is the default ─────────────────
    out.width = config.width > 0 ? config.width : out.display->width;
    out.height = config.height > 0 ? config.height : out.display->height;

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
