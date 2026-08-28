#pragma once

#include <QString>

/// The identifier an instance is reached by: https://stream.{domain}/{id}
///
/// It replaces the per-instance sub-domain, so it is a LOCATOR, not a secret.
/// Anyone may hold one; what grants access is the pairing signature (MW-BIND-v1).
/// That is why it is long — 128 bits, so the set cannot be walked — and why it
/// never rotates: an address that changes cannot be bookmarked.
///
/// Encoding is Crockford base32 in lower case, 26 characters, alphabet
/// `0123456789abcdefghjkmnpqrstvwxyz` (no i, l, o or u). Those four are absent
/// precisely so the identifier survives being read aloud or copied off a screen.
///
/// ⚠️ normalise() MUST agree with normaliseID() in
/// deploy/powerdns/mw-rendezvous/store.go, character for character. The server
/// keys its ownership store on the normalised form: if the two ever disagree, a
/// claim made under one spelling becomes unfindable under the other, and the
/// instance silently loses its own identifier. The unit tests pin the pairs.
namespace RendezvousId {

/// Number of characters in a well-formed identifier.
constexpr int kLength = 26;

/// Draw a fresh identifier from the system CSPRNG.
QString generate();

/// Fold the forms a human might type back to canonical: trim, lower-case, drop
/// grouping hyphens and spaces, then map the ambiguous letters
/// (i,l → 1 · o → 0 · u → v). Returns the folded string without validating it.
QString normalise(const QString& raw);

/// Whether `id` is exactly kLength characters of the Crockford alphabet.
/// Expects an already-normalised string — upper case does not pass.
bool isValid(const QString& id);

/// Convenience: normalise, then validate.
inline bool isValidLoose(const QString& raw)
{
    return isValid(normalise(raw));
}

} // namespace RendezvousId
