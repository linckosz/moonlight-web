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

#include "mw/native/NativeHost.h"

#include "Log.h"
#include "Probe.h"
#include "Selector.h"
#include "Session.h"

#ifndef MW_NATIVE_VERSION
#define MW_NATIVE_VERSION "0.1.0-dev"
#endif

namespace mw::native {

Capabilities NativeHost::probe()
{
    return mw::native::probe();
}

VirtualGamepad NativeHost::probeVirtualGamepad()
{
    const VirtualGamepad result = platform::probeVirtualGamepad();
    log::info(std::string("[native] virtual gamepad: ") +
              (result.present ? "available" : "unavailable") +
              (result.diagnostic.empty() ? "" : " — " + result.diagnostic));
    return result;
}

std::unique_ptr<Session> NativeHost::createSession(const SessionConfig& config,
                                                   VideoCallback onVideo, AudioCallback onAudio,
                                                   RumbleCallback onRumble, CursorCallback onCursor,
                                                   SessionEndedCallback onEnded, std::string& error)
{
    if (!onVideo) {
        error = "a session without a video callback would encode into nothing";
        return nullptr;
    }

    // Probe again rather than caching: displays are hot-pluggable, and a
    // session built on a display list from minutes ago can name a monitor that
    // has since been unplugged. The probe is cheap by design for this reason.
    Capabilities caps = mw::native::probe();
    if (!caps.available) {
        error = caps.diagnostic.empty() ? toString(caps.reason) : caps.diagnostic;
        return nullptr;
    }

    // The bench may ask for the machine to be treated as if no GPU encoded —
    // the only way to exercise the fallback tier on a bench that has NVENC. It
    // is done by putting the capability set in exactly the state the tier
    // exists for, rather than by a special case in the Selector: every GPU is
    // stripped of its encoders, the fallbacks are probed as probe() would have,
    // and select() takes the path a Snapdragon would. A real session never sets
    // this.
    if (config.tuning.fallback != EncoderTuning::Fallback::None) {
        for (GpuInfo& gpu : caps.gpus) {
            gpu.encoders.clear();
            gpu.codecs.clear();
        }
        if (caps.fallbacks.empty()) platform::probeFallbackEncoders(caps);
        if (config.tuning.fallback != EncoderTuning::Fallback::Tier) {
            // "mf" and "mfsw" both name Media Foundation here; which transform
            // the session opens is the encoder's business (MfEncoder).
            const EncoderApi wanted = config.tuning.fallback == EncoderTuning::Fallback::Cpu
                                          ? EncoderApi::Software
                                          : EncoderApi::MediaFoundation;
            std::vector<FallbackEncoder> kept;
            for (FallbackEncoder& fb : caps.fallbacks)
                if (fb.api == wanted) kept.push_back(std::move(fb));
            caps.fallbacks = std::move(kept);
        }
        log::warning("[native] bench: every GPU encoder hidden — this session runs the fallback "
                     "tier");
    }

    Selection selection;
    if (!select(caps, config, selection, error)) return nullptr;

    // Normalize what the platform layer receives, so every backend is handed an
    // already-resolved configuration and none of them re-implements the policy.
    SessionConfig resolved = config;
    resolved.displayId = selection.display->id;
    resolved.width = selection.width;
    resolved.height = selection.height;
    resolved.fps = selection.fps;
    resolved.hdr = selection.hdr;
    resolved.yuv444 = selection.yuv444;
    resolved.clientCodecs = {selection.codec};
    // Carried through unchanged: whether the encoder can honour it is its own
    // answer, reported back in SessionInfo::intraRefresh.
    resolved.intraRefresh = config.intraRefresh;

    // A fallback session may have no GPU at all behind it (a Linux VM whose
    // display adapter exposes no render node); the name then says so rather
    // than dereferencing nothing.
    const std::string gpuName = selection.gpu ? selection.gpu->name : "no GPU";
    log::info(std::string("[native] session: display ") + std::to_string(resolved.displayId) + " " +
              std::to_string(resolved.width) + "x" + std::to_string(resolved.height) + "@" +
              std::to_string(resolved.fps) + " " + toString(selection.codec) + " via " +
              toString(selection.encoder) + " on " + gpuName + (selection.hdr ? " (HDR)" : "") +
              (selection.yuv444 ? " (4:4:4)" : "") +
              (selection.crossGpuCopy ? " [cross-GPU copy]" : "") +
              (selection.fallbackEncoder
                   ? (selection.cpuEncoder ? " [fallback, CPU]" : " [fallback, via the OS]")
                   : ""));

    // Hand the decision down rather than let the backend take it again. The
    // backend's simpler answer — "encode on the GPU that drives the display" —
    // is wrong exactly when the Selector was needed, so leaving it to guess
    // undoes the work above.
    ResolvedTarget target;
    target.displayId = selection.display->id;
    target.encodeGpuName = gpuName;
    target.encoder = selection.encoder;
    target.codec = selection.codec;
    target.crossGpuCopy = selection.crossGpuCopy;
    target.hdr = selection.hdr;
    // Granted, not requested: the Selector already moved the codec to one that
    // has 4:4:4, or gave it up. The backend must never re-ask the encoder.
    target.yuv444 = selection.yuv444;
    target.encodeAdapterHandle = selection.gpu ? selection.gpu->nativeHandle : 0;

    // Capture always happens on the adapter that scans the display out; only
    // the encoder may sit elsewhere.
    if (const GpuInfo* displayGpu = caps.gpuFor(*selection.display)) {
        target.captureAdapterHandle = displayGpu->nativeHandle;
    } else {
        // No association: capture where we encode and accept whatever the
        // platform gives us. Rare enough to be worth saying out loud.
        target.captureAdapterHandle = target.encodeAdapterHandle;
        log::warning("[native] display names no GPU — capturing on the encoder's adapter");
    }

    // DXGI wants the output's index within ITS OWN adapter.
    unsigned outputIndex = 0;
    for (const DisplayInfo& display : caps.displays) {
        if (display.id == selection.display->id) break;
        if (display.gpuId == selection.display->gpuId) ++outputIndex;
    }
    target.outputIndex = outputIndex;

    SessionCallbacks callbacks;
    callbacks.onVideo = std::move(onVideo);
    callbacks.onAudio = std::move(onAudio);
    callbacks.onRumble = std::move(onRumble);
    callbacks.onCursor = std::move(onCursor);
    callbacks.onEnded = std::move(onEnded);

    return detail::createPlatformSession(resolved, target, callbacks, error);
}

void NativeHost::setLogSink(std::function<void(int level, const std::string& message)> sink)
{
    log::setSink(std::move(sink));
}

const char* NativeHost::version()
{
    return MW_NATIVE_VERSION;
}

} // namespace mw::native
