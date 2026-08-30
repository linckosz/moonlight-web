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
#include "Selector.h"
#include "Session.h"

#ifndef MW_NATIVE_VERSION
#define MW_NATIVE_VERSION "0.1.0-dev"
#endif

namespace mw::native {

// Declared in Probe.cpp — the free function the facade forwards to.
Capabilities probe();

Capabilities NativeHost::probe()
{
    return mw::native::probe();
}

std::unique_ptr<Session> NativeHost::createSession(const SessionConfig& config,
                                                   VideoCallback onVideo, AudioCallback onAudio,
                                                   RumbleCallback onRumble,
                                                   SessionEndedCallback onEnded, std::string& error)
{
    if (!onVideo) {
        error = "a session without a video callback would encode into nothing";
        return nullptr;
    }

    // Probe again rather than caching: displays are hot-pluggable, and a
    // session built on a display list from minutes ago can name a monitor that
    // has since been unplugged. The probe is cheap by design for this reason.
    const Capabilities caps = mw::native::probe();
    if (!caps.available) {
        error = caps.diagnostic.empty() ? toString(caps.reason) : caps.diagnostic;
        return nullptr;
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
    resolved.clientCodecs = {selection.codec};

    log::info(std::string("[native] session: display ") + std::to_string(resolved.displayId) + " " +
              std::to_string(resolved.width) + "x" + std::to_string(resolved.height) + "@" +
              std::to_string(resolved.fps) + " " + toString(selection.codec) + " via " +
              toString(selection.encoder) + " on " + selection.gpu->name +
              (selection.hdr ? " (HDR)" : "") +
              (selection.crossGpuCopy ? " [cross-GPU copy]" : ""));

    SessionCallbacks callbacks;
    callbacks.onVideo = std::move(onVideo);
    callbacks.onAudio = std::move(onAudio);
    callbacks.onRumble = std::move(onRumble);
    callbacks.onEnded = std::move(onEnded);

    return detail::createPlatformSession(resolved, callbacks, error);
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
