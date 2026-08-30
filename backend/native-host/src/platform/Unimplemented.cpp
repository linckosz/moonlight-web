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

#include "../core/Probe.h"
#include "../core/Session.h"

// The platform backend for an OS whose real one has not landed yet.
//
// This file is compiled ONLY when CMake found no platform backend for the
// target (see the MW_NATIVE_PLATFORM logic in CMakeLists.txt). It answers "not
// available here" truthfully, which makes the whole chain — probe, backend,
// REST status, the Hosts page fallback message — exercisable end to end before
// a single line of DXGI exists.
//
// It must never be compiled alongside a real backend: two definitions of the
// same symbols would not link, which is the intended guardrail.

namespace mw::native::platform {

bool hasInteractiveSession()
{
    // Unknowable without platform code, and claiming "yes" would push the
    // verdict onto enumerate() where it would read as a capture failure. Say
    // no: the reason reported is then the accurate one.
    return false;
}

bool isOsSupported()
{
    return false;
}

Unavailability enumerate(Capabilities& caps)
{
    caps.diagnostic = "the native engine has no backend for this platform in this build";
    return Unavailability::ArchNotSupported;
}

} // namespace mw::native::platform

namespace mw::native::detail {

std::unique_ptr<Session> createPlatformSession(const SessionConfig&, const SessionCallbacks&,
                                               std::string& error)
{
    error = "the native engine has no backend for this platform in this build";
    return nullptr;
}

} // namespace mw::native::detail
