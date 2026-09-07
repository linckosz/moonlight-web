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

#include "mw/native/Capabilities.h"

#include <string>
#include <vector>

namespace mw::native::encode {

/// What Media Foundation can encode on this machine.
///
/// Asked only when no GPU answered a vendor SDK, and that is what makes it worth
/// asking at all: Media Foundation is the one Windows encoder path that does not
/// need us to know the vendor. On a Snapdragon laptop it is how Qualcomm's
/// fixed-function encoder is reached — there is no SDK for it — and on any
/// machine at all it carries Microsoft's own software H.264 transform as a
/// floor.
struct MfCaps
{
    bool usable = false;

    /// Codecs a transform accepted NV12 for, best first.
    std::vector<Codec> codecs;

    /// True when the transform that answered is hardware. Decided by which
    /// enumeration found it, not by its name: MFT_ENUM_FLAG_HARDWARE is the
    /// only trustworthy form of the question, since friendly names are vendor
    /// prose ("Intel® Quick Sync Video H.264 Encoder MFT" is hardware, "H264
    /// Encoder MFT" is Microsoft's software one, and nothing in the strings
    /// says so).
    bool hardware = false;

    /// The transform's friendly name, for the log and the stats overlay.
    std::string name;

    /// English, for the log line when nothing usable was found.
    std::string diagnostic;
};

/// Ask Media Foundation what it can encode. Never throws; a machine without
/// Media Foundation answers `usable = false` with a reason.
MfCaps queryMfCapabilities();

} // namespace mw::native::encode
