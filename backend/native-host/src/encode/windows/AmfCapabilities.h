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

/// What AMF can really do on one adapter.
struct AmfCaps
{
    bool usable = false;
    std::vector<Codec> codecs; ///< best first, as the Selector expects
    bool supports10Bit = false;
    bool supports444 = false;
};

/// Open a real AMF context on the adapter with this LUID and ask it what it
/// supports.
///
/// Same reasoning as queryNvencCapabilities: finding `amfrt64.dll` proves an
/// AMD driver is installed somewhere, not what THIS adapter encodes. An RX 7600
/// (RDNA3) does AV1; an older Radeon on the same driver does not. So each codec
/// is established by actually creating its encoder component — the only answer
/// the hardware gives honestly.
///
/// Cached per LUID for the life of the process: a GPU's encoder does not change
/// under a running driver.
AmfCaps queryAmfCapabilities(uint64_t adapterLuid);

} // namespace mw::native::encode
