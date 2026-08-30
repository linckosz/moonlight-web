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

#include "Probe.h"
#include "Log.h"

#include <string>

namespace mw::native {

Capabilities probe()
{
    Capabilities caps;

    // The order below is the order of certainty: each check answers a question
    // whose "no" is final, so an unsupported machine gets ONE accurate reason
    // instead of whatever the first API happened to fail with.
    //
    // Every one of these reasons maps to the same sentence for the user (§23) —
    // the distinction exists for the log line and for telemetry.

    if (!platform::isOsSupported()) {
        caps.reason = Unavailability::OsTooOld;
        caps.diagnostic = "operating system predates the capture APIs this engine needs";
        log::info("[native] unavailable: " + caps.diagnostic);
        return caps;
    }

    if (!platform::hasInteractiveSession()) {
        caps.reason = Unavailability::NoInteractiveSession;
        caps.diagnostic = "no interactive desktop session (service in session 0, or nobody "
                          "logged in)";
        log::info("[native] unavailable: " + caps.diagnostic);
        return caps;
    }

    const Unavailability enumerated = platform::enumerate(caps);
    if (enumerated != Unavailability::None) {
        caps.reason = enumerated;
        if (caps.diagnostic.empty()) caps.diagnostic = toString(enumerated);
        log::info("[native] unavailable: " + caps.diagnostic);
        return caps;
    }

    if (caps.displays.empty()) {
        caps.reason = Unavailability::NoDisplay;
        caps.diagnostic = "no display attached";
        log::info("[native] unavailable: " + caps.diagnostic);
        return caps;
    }

    // An encoder anywhere is enough to be available. Which one a given display
    // gets — and whether reaching it costs a cross-GPU copy — is decided per
    // session by the Selector, not here: a machine with one encoder-less
    // display and one perfectly good one must not be declared unusable.
    bool anyEncoder = false;
    for (const GpuInfo& gpu : caps.gpus) {
        if (!gpu.encoders.empty()) {
            anyEncoder = true;
            break;
        }
    }
    if (!anyEncoder) {
        caps.reason = Unavailability::NoEncoder;
        caps.diagnostic = "no hardware encoder on any GPU, and software encoding was not "
                          "fast enough for this display";
        log::info("[native] unavailable: " + caps.diagnostic);
        return caps;
    }

    caps.available = true;
    caps.reason = Unavailability::None;

    if (log::enabled(log::Info)) {
        std::string summary = "[native] available: " + std::to_string(caps.displays.size()) +
                              " display(s), " + std::to_string(caps.gpus.size()) + " GPU(s), " +
                              toString(caps.capture);
        log::info(summary);
    }
    return caps;
}

} // namespace mw::native
