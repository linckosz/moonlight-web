/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#include <QString>

/**
 * Extraction of the DTLS certificate fingerprint from an SDP blob, for
 * MW-BIND-v1 (docs/design/pairing-signature.md).
 *
 * What is signed is this extracted value, never the SDP text: both ends rewrite
 * SDP in flight (the frontend forces stereo Opus, the backend rewrites
 * candidates), so a signature over the raw document would break on the first
 * such adjustment and teach us to make verification optional.
 */
namespace SdpFingerprint {

/**
 * The single SHA-256 fingerprint an SDP commits to, normalised as uppercase hex
 * with colon separators — or an empty string when the SDP does not commit to
 * exactly one.
 *
 * Empty is returned when:
 *  - there is no `a=fingerprint:` line at all;
 *  - any of them uses a hash other than sha-256 (an attacker who can downgrade
 *    us to sha-1 can collide the value we are signing);
 *  - any of them is malformed, or not 32 colon-separated hex bytes;
 *  - they are not all identical. This is the case that matters most: an SDP may
 *    legitimately repeat the same fingerprint per m-line, but two *different*
 *    values means the peer has left the choice of which one to trust up to
 *    whoever reads it — including an attacker who added the second one.
 *
 * The caller must treat empty as a hard failure, never as "no fingerprint to
 * check, carry on".
 */
QString extract(const QString& sdp);

} // namespace SdpFingerprint
