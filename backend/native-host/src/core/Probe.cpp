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

    // One usable encoder anywhere is enough to be available. Which one a given
    // display gets — and whether reaching it costs a cross-GPU copy — is decided
    // per session by the Selector, not here: a machine with one encoder-less
    // display and one perfectly good one must not be declared unusable.
    //
    // "Usable" means an encoder AND at least one codec it can actually produce.
    // An encoder that can encode nothing we can send is no encoder at all, and
    // counting it here would offer the user a host that fails the moment they
    // click it — worse than offering none.
    if (!caps.anyGpuEncodes()) {
        // No GPU encoder anywhere. Before refusing the machine, ask what the OS
        // itself can encode with — the tier that exists precisely for the
        // machines this branch used to turn away: a Windows-on-ARM laptop whose
        // Adreno we have no SDK for, and a virtual machine whose display adapter
        // exposes no render node at all (bench-vm, hyperv_drm, 07/09/2026).
        //
        // Asked ONLY here, and that is deliberate: enumerating transforms or
        // instantiating a codec costs real time, and probe() runs on every
        // host-list refresh. On a machine with a GPU encoder nothing below ever
        // executes.
        platform::probeFallbackEncoders(caps);

        if (caps.fallbacks.empty()) {
            caps.reason = Unavailability::NoEncoder;
            // Says only what was actually looked for. An earlier wording ("...and
            // software encoding was not fast enough for this display") described
            // a measurement that never happened; the wording after it said there
            // was no fallback at all, which stopped being true on 07/09/2026.
            // What is true now is that everything was asked and nothing answered.
            caps.diagnostic = "no video encoder on any GPU (NVIDIA NVENC, AMD AMF, Intel Quick "
                              "Sync), and no fallback encoder either";
            log::info("[native] unavailable: " + caps.diagnostic);
            return caps;
        }

        if (log::enabled(log::Info)) {
            const FallbackEncoder& best = caps.fallbacks.front();
            log::info(std::string("[native] no GPU encoder — fallback available: ") + best.name +
                      (best.hardware ? " (hardware, via the OS)" : " (on the CPU)"));
        }
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
