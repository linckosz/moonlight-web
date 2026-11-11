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

#include "../input/IInputSink.h"
#include "Log.h"
#include "Probe.h"
#include "Selector.h"
#include "Session.h"

#include <algorithm>

#ifdef _WIN32
#include "../platform/windows/InputDesktop.h"
#endif

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

namespace {

/// A session whose pictures come from the guests' shared feed: its display,
/// found again, and nothing chosen for video — there is no encoder to pick,
/// and the feed already picked the codec every guest decodes.
std::unique_ptr<Session> createExternalSession(const SessionConfig& config,
                                               SessionCallbacks callbacks, std::string& error)
{
#ifndef _WIN32
    (void)config;
    (void)callbacks;
    error = "the guests' shared feed is carried on Windows only for now";
    return nullptr;
#else
    const Capabilities caps = mw::native::probe();
    if (!caps.available) {
        error = caps.diagnostic.empty() ? toString(caps.reason) : caps.diagnostic;
        return nullptr;
    }
    const DisplayInfo* display = nullptr;
    for (const DisplayInfo& d : caps.displays)
        if (d.id == config.displayId) display = &d;
    if (!display) {
        error = "that display is no longer connected";
        return nullptr;
    }
    ResolvedTarget target;
    target.displayId = display->id;
    target.capture = display->capture != CaptureApi::None ? display->capture : caps.capture;
    SessionConfig resolved = config;
    // Nothing downstream encodes: the fields that would pick an encoder are
    // left as the caller gave them, and read by no one.
    resolved.intraRefresh = false;
    resolved.intraRefreshRequired = false;
    log::info("[native] session: display " + std::to_string(display->id) +
              ", pictures from the guests' shared feed — this session injects its viewer's "
              "input and captures the host's audio");
    return detail::createPlatformSession(resolved, target, callbacks, error);
#endif
}

} // namespace

std::unique_ptr<Session> NativeHost::createSession(const SessionConfig& config,
                                                   VideoCallback onVideo, AudioCallback onAudio,
                                                   RumbleCallback onRumble, CursorCallback onCursor,
                                                   SessionEndedCallback onEnded, std::string& error)
{
    if (config.videoSource == VideoSource::External) {
        SessionCallbacks callbacks;
        callbacks.onAudio = std::move(onAudio);
        callbacks.onRumble = std::move(onRumble);
        callbacks.onEnded = std::move(onEnded);
        return createExternalSession(config, std::move(callbacks), error);
    }

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

    // "Match my screen" is a Windows or macOS session's to attempt (a display
    // mode change, applyClientMode in both). On Linux it is Auto from the start:
    // the fallback box, never upscaled — no mode change without root under
    // Wayland, and none attempted (decided 24/09/2026). Except on the portal's
    // virtual monitor, which changes no mode either: the stream makes it, and
    // makes it at the client's size (plan Idées Punktfunk, C0, 01/10/2026).
    SessionConfig asked = config;
#if !defined(_WIN32) && !defined(__APPLE__)
    if (sizeVirtualMonitorToClient(caps, asked))
        log::info("[native] \"Match my screen\" on the virtual display — made at the client's "
                  "size (" +
                  std::to_string(asked.width) + "x" + std::to_string(asked.height) + " asked)");
    else if (fallBackFromMatch(asked))
        log::info("[native] this platform changes no display mode — \"Match my screen\" "
                  "streams as Auto");
#endif

    // What the session's own settings unlock: on Linux, AV1 where it asks for
    // the Vulkan Video chain (C13.12). Nothing elsewhere.
    const std::vector<Codec> offered = platform::offerSessionCodecs(caps, asked);

    Selection selection;
    if (!select(caps, asked, selection, error)) return nullptr;

    // Normalize what the platform layer receives, so every backend is handed an
    // already-resolved configuration and none of them re-implements the policy.
    SessionConfig resolved = asked;
    resolved.displayId = selection.display->id;
    resolved.width = selection.width;
    resolved.height = selection.height;
    // The request itself, kept beside the shaped frame: a session that puts
    // its display in the client's mode ("Match my screen") looks that mode
    // up by the size the client asked, not by the frame shaped to the mode
    // the display had before.
    resolved.requestedWidth = asked.width;
    resolved.requestedHeight = asked.height;
    resolved.fps = selection.fps;
    resolved.hdr = selection.hdr;
    resolved.yuv444 = selection.yuv444;
    // ⚠️ NOT narrowed to {selection.codec}, which is what this line used to do.
    // The choice already travels — ResolvedTarget::codec is where a backend
    // reads it — so overwriting the list here bought nothing and cost the truth:
    // it made the config claim the client decodes exactly one codec, when what
    // the field means is what the client CAN decode.
    //
    // That difference is not academic. On Linux the portal may hand over shared
    // memory, which forces the CPU pair, which encodes H.264 and nothing else —
    // a fact known only after the stream has negotiated, long after this. The
    // session then has to ask "will this client take H.264 instead?", and with
    // the list narrowed the only answer available was "it asked for HEVC", so
    // the session died on a machine whose browser decodes H.264 perfectly well.
    resolved.clientCodecs = config.clientCodecs;
    // Carried through unchanged: whether the encoder can honour it is its own
    // answer, reported back in SessionInfo::intraRefresh. Required is asked.
    resolved.intraRefresh = config.intraRefresh || config.intraRefreshRequired;

    // A fallback session may have no GPU at all behind it (a Linux VM whose
    // display adapter exposes no render node); the name then says so rather
    // than dereferencing nothing.
    const std::string gpuName = selection.gpu ? selection.gpu->name : "no GPU";
    // The driver, which a regression report needs before anything else.
    const std::string driver =
        selection.gpu ? driverVersionText(selection.gpu->driverVersion) : std::string();
    log::info(std::string("[native] session: display ") + std::to_string(resolved.displayId) + " " +
              std::to_string(resolved.width) + "x" + std::to_string(resolved.height) + "@" +
              std::to_string(resolved.fps) + " " + toString(selection.codec) + " via " +
              toString(selection.encoder) + " on " + gpuName +
              (driver.empty() ? std::string() : " (driver " + driver + ")") +
              (selection.hdr ? " (HDR)" : "") + (selection.yuv444 ? " (4:4:4)" : "") +
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
    if (!offered.empty() && selection.gpu && !selection.fallbackEncoder)
        for (Codec c : selection.gpu->codecs)
            if (std::find(offered.begin(), offered.end(), c) == offered.end())
                target.codecsWithoutOffer.push_back(c);
    target.crossGpuCopy = selection.crossGpuCopy;
    target.hdr = selection.hdr;
    target.hdrCapable = selection.hdrCapable;
    target.fallbackEncoder = selection.fallbackEncoder;
    // Granted, not requested: the Selector already moved the codec to one that
    // has 4:4:4, or gave it up. The backend must never re-ask the encoder.
    target.yuv444 = selection.yuv444;
    target.encodeAdapterHandle = selection.gpu ? selection.gpu->nativeHandle : 0;
    target.encodeVendorId = selection.gpu ? selection.gpu->vendorId : 0;
    target.encodeDriverVersion = selection.gpu ? selection.gpu->driverVersion : 0;
    // The probe already decided which route can give a picture on this machine;
    // the session obeys rather than asking again. One rule, one place.
    // A display with its own route (Linux's portal virtual display) takes it.
    target.capture =
        selection.display->capture != CaptureApi::None ? selection.display->capture : caps.capture;
    target.portalVirtual = selection.display->key == kPortalVirtualDisplayKey;
    target.gamescopeSteam = selection.display->key == kGamescopeSteamDisplayKey;
    target.gamescopeApp = selection.display->key == kGamescopeAppDisplayKey;

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
    target.displayKey = selection.display->key;
    target.displayOsName = selection.display->osName;

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

void NativeHost::setKeyboardDiagnostics(bool on)
{
    input::keyboardDiagnosticsFlag().store(on, std::memory_order_relaxed);
}

bool NativeHost::keyboardDiagnostics()
{
    return input::keyboardDiagnostics();
}

void NativeHost::setSecureAttentionSender(std::function<bool(std::string& error)> sender)
{
#ifdef _WIN32
    platform::setSecureAttentionSender(std::move(sender));
#else
    (void)sender;
#endif
}

bool NativeHost::secureDesktopHasInput()
{
#ifdef _WIN32
    return platform::inputDesktopName() != "Default";
#else
    return false;
#endif
}

bool NativeHost::enterSignInScreenOnly()
{
#ifdef _WIN32
    platform::enterSignInScreenOnly();
    return true;
#else
    return false;
#endif
}

bool NativeHost::signInScreenOnly()
{
#ifdef _WIN32
    return platform::signInScreenOnly();
#else
    return false;
#endif
}

const char* NativeHost::version()
{
    return MW_NATIVE_VERSION;
}

} // namespace mw::native
