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

#include "OpusEncoder.h"

#include "../core/Log.h"

#include <opus.h>

namespace mw::native::audio {

namespace {
// The largest packet Opus can produce for one 5 ms stereo frame is far below
// this; the recommended ceiling from the API documentation.
constexpr size_t kMaxPacketBytes = 4000;
} // namespace

OpusEncoder::~OpusEncoder()
{
    if (m_Encoder) opus_encoder_destroy(m_Encoder);
}

bool OpusEncoder::open(std::string& error)
{
    int err = OPUS_OK;
    m_Encoder =
        opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (!m_Encoder || err != OPUS_OK) {
        error = std::string("opus_encoder_create: ") + opus_strerror(err);
        m_Encoder = nullptr;
        return false;
    }
    opus_encoder_ctl(m_Encoder, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(m_Encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(m_Encoder, OPUS_SET_VBR_CONSTRAINT(1));
    // Music, not speech: the stream is whatever the host's mixer carries.
    opus_encoder_ctl(m_Encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    // Mid-scale complexity: the encode of a 5 ms frame is well under a
    // millisecond either way, and the higher settings buy quality per bit
    // that 128 kbps stereo does not need.
    opus_encoder_ctl(m_Encoder, OPUS_SET_COMPLEXITY(5));
    // Every packet stands alone: a lost one never costs the next.
    opus_encoder_ctl(m_Encoder, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(m_Encoder, OPUS_SET_PACKET_LOSS_PERC(0));
    return true;
}

size_t OpusEncoder::encode(const float* interleaved, std::vector<uint8_t>& out)
{
    if (!m_Encoder || !interleaved) return 0;
    if (out.size() < kMaxPacketBytes) out.resize(kMaxPacketBytes);
    const opus_int32 n = opus_encode_float(m_Encoder, interleaved, kFrameSamples, out.data(),
                                           static_cast<opus_int32>(out.size()));
    if (n < 0) {
        if (!m_LoggedError) {
            log::warning(std::string("[native] opus_encode_float: ") + opus_strerror(n));
            m_LoggedError = true;
        }
        return 0;
    }
    return static_cast<size_t>(n);
}

const char* OpusEncoder::libraryVersion()
{
    return opus_get_version_string();
}

} // namespace mw::native::audio
