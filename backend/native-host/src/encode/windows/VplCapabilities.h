/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#pragma once

#include "mw/native/Capabilities.h"

#include <cstdint>
#include <vector>

namespace mw::native::encode {

/// What oneVPL can really do on one adapter.
struct VplCaps
{
    bool usable = false;
    std::vector<Codec> codecs; ///< best first, as the Selector expects
    bool supports10Bit = false;
    bool supports444 = false;
};

/// Open a real oneVPL session on the adapter with this LUID and ask it what it
/// supports.
///
/// Same reasoning as the NVENC and AMF queries: finding `libvpl.dll` proves an
/// Intel driver is installed, not what THIS adapter encodes. An Arc card and a
/// UHD iGPU answer to the same dispatcher and differ on AV1, so each codec is
/// established by asking the encoder itself — `MFXVideoENCODE_Query`, which
/// answers without allocating anything.
///
/// Cached per LUID for the life of the process.
VplCaps queryVplCapabilities(uint64_t adapterLuid);

} // namespace mw::native::encode
