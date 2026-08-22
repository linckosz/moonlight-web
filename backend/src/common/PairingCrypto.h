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

#include <QByteArray>
#include <QString>
#include <QVector>

/**
 * MW-BIND-v1 — binding the DTLS fingerprint to the pairing key.
 *
 * The signaling channel will one day pass through an introduction server that
 * we do not want to have to trust. That server could otherwise swap the DTLS
 * fingerprints in flight, terminate DTLS on both sides, and inject keyboard and
 * mouse events into a real desktop. These helpers let each side sign the
 * fingerprint it is committing to with a key the server has never seen.
 *
 * See docs/design/pairing-signature.md for the protocol and its rationale.
 *
 * Curve is ECDSA P-256, chosen because WebCrypto supports it everywhere and
 * supports non-extractable private keys everywhere. Signatures cross the wire
 * in the **raw r‖s form WebCrypto produces and expects** (64 bytes), never in
 * OpenSSL's native DER — conversion happens here, at the boundary.
 */
namespace PairingCrypto {

/// The protocol label. Anything that does not carry it is not our protocol.
inline constexpr char PROTOCOL[] = "MW-BIND-v1";

/// Raw r‖s signature length for P-256: two 32-byte scalars.
inline constexpr int SIGNATURE_BYTES = 64;

/// Minimum nonce length accepted from a peer (128 bits, per the design).
inline constexpr int MIN_NONCE_BYTES = 16;

// ── Key material ───────────────────────────────────────────────────────────

/// Generate a fresh P-256 private key, PEM-encoded. Empty on failure.
QByteArray generatePrivateKeyPem();

/// The SPKI (DER) public key of a PEM private key. Empty on failure.
QByteArray publicKeySpkiFromPrivatePem(const QByteArray& privatePem);

/// Key identifier: base64url(SHA-256(spki)), unpadded. This is what travels as
/// `keyId` and what the admin UI can show to name a device's key.
QString keyId(const QByteArray& spkiDer);

/// True when @p spkiDer parses as a P-256 public key. Everything arriving from
/// a browser goes through this before being stored — a key we cannot parse is
/// a key we could never verify against, and storing it would silently create a
/// pairing that can never succeed.
bool isValidP256Spki(const QByteArray& spkiDer);

// ── Message construction ───────────────────────────────────────────────────

/**
 * Concatenate @p fields with an unambiguous framing: each field is preceded by
 * its length as a 4-byte big-endian integer.
 *
 * Plain concatenation would let an attacker move bytes across a field boundary
 * — ("ab","c") and ("a","bc") would hash identically — and one of those fields
 * is a fingerprint. The frontend builds the same framing in
 * frontend/js/util/pairingCrypto.js; the two must not drift.
 */
QByteArray buildSignedMessage(const QVector<QByteArray>& fields);

/// The exact bytes the host signs (§4 step 1).
QByteArray hostDigestInput(const QString& hostId, const QString& browserKeyId,
                           const QByteArray& nonceB, const QString& fingerprintHost);

/// The exact bytes the browser signs (§4 step 3).
QByteArray browserDigestInput(const QString& hostId, const QByteArray& nonceH,
                              const QString& fingerprintHost, const QString& fingerprintBrowser);

// ── Sign / verify ──────────────────────────────────────────────────────────

/// Sign @p message with a PEM private key. Returns raw r‖s (64 bytes), or empty
/// on failure.
QByteArray sign(const QByteArray& privatePem, const QByteArray& message);

/// Verify a raw r‖s signature against an SPKI public key. False for anything
/// malformed — a caller must never be able to tell "bad signature" from
/// "unparseable key" and treat one of them as success.
bool verify(const QByteArray& spkiDer, const QByteArray& message, const QByteArray& signature);

// ── Nonces ─────────────────────────────────────────────────────────────────

/// A fresh 32-byte nonce from the system CSPRNG.
QByteArray generateNonce();

} // namespace PairingCrypto
