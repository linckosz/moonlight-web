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

#pragma once

#include "mw/native/NativeHost.h"

#include <memory>
#include <string>

namespace mw::native {

/// The four callbacks a session delivers on, bundled so the platform seam takes
/// one parameter instead of four and gaining a fifth later touches one line.
struct SessionCallbacks
{
    VideoCallback onVideo;
    AudioCallback onAudio;
    RumbleCallback onRumble;
    SessionEndedCallback onEnded;
};

namespace detail {

/// Build the platform's session object. Implemented once per OS backend
/// (and by platform/Unimplemented.cpp when none is built).
///
/// Receives an already-validated config: the display exists, a codec was agreed
/// with the client, and the GPU association is resolved. Everything a backend
/// still has to do is genuinely platform work.
std::unique_ptr<Session> createPlatformSession(const SessionConfig& config,
                                               const SessionCallbacks& callbacks,
                                               std::string& error);

} // namespace detail
} // namespace mw::native
