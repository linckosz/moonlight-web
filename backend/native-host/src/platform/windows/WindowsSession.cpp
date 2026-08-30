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

#include "../../core/Session.h"

// The Windows capture → encode pipeline.
//
// Not built yet: this increment landed the probe (displays, GPUs and the
// display→GPU association), which is what the rest of the pipeline is chosen
// from. Duplication, colour conversion and the encoder come next.
//
// Reaching this function is currently impossible through the product: probe()
// reports the engine unavailable while no adapter lists a codec, and
// NativeHostBackend only produces a native descriptor for an available engine.
// It is written to refuse rather than to assert, because "unreachable" is a
// claim about today's callers and those change.

namespace mw::native::detail {

std::unique_ptr<Session> createPlatformSession(const SessionConfig&, const SessionCallbacks&,
                                               std::string& error)
{
    error = "the Windows capture pipeline is not implemented in this build yet";
    return nullptr;
}

} // namespace mw::native::detail
