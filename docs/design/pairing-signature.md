# MW-BIND-v1 — binding the DTLS fingerprint to the pairing key

**Status**: design, not implemented. No code should be written against this
document until it has been reviewed by someone other than its author.
**Audience**: reviewers. It assumes no prior knowledge of this codebase.
**Date**: 2026-08-22.

---

## 1. Why this document exists

MoonlightWeb is moving from "each instance publishes its own subdomain" to
"each instance is reached through an introduction server". That server's only
job is to put two peers in touch and then get out of the way: the media never
flows through it.

But WebRTC cannot start until both peers have exchanged the **fingerprint** of
their DTLS certificate, and that exchange necessarily goes through the
introduction server. A fingerprint is not a secret — it is a hash of a public
certificate, and nothing is lost by the server reading it.

The problem is not reading. It is **substitution**.

A compromised introduction server can present its own fingerprint to each side,
terminate DTLS itself, and sit in the middle of the session. It then sees the
video and — this is the part that matters — it can **inject keyboard and mouse
events**. MoonlightWeb forwards input to a real desktop, so a man in the middle
of the signaling channel is a man in control of the machine.

Without a countermeasure, the honest answer to *"can the operator of the
introduction server connect to a user's PC?"* is **yes**.

**The PIN does not help.** It authenticates a browser at pairing time; it does
not protect the channel afterwards. Concretely:

- against an **already-paired** browser, the attacker slips inside a session
  that is already authenticated — no PIN is ever presented;
- through a **modified bootstrap**, the attacker runs code in the victim's
  browser — the PIN is never involved;
- against a **never-paired** browser, the attacker serves the PIN page itself
  and simply waits for the legitimate user to type it in.

So the countermeasure has to bind the fingerprint to a secret the server has
never seen. That is what this document specifies.

**Calibration, stated honestly.** This is a *targeted* risk, not mass
exploitation, and it is the ordinary threat model of any online service. What
justifies treating it is the **consequence** — full control of a desktop — not
the probability.

---

## 2. What exists today

| Piece | Where | Relevance |
|---|---|---|
| PIN → session cookie | `AuthManager` (`SessionInfo`, `AuthManager.h:33`) | Authenticates a browser once. **No per-browser key exists**, so today there is nothing to sign with. |
| The backend is the **offerer** | `SignalingServer.cpp:325` handles `type == "sdp"`, which is the browser's *answer* | Determines the message ordering available to us (§4). |
| Browser side | `WebRtcDataChannel.js:898-904`, `WebRtcMedia.js:689-693` — `setRemoteDescription(offer)` then `createAnswer()` | The two points where a verification must be inserted *before* any DTLS state exists. |
| `IdentityManager` | `backend/src/backend/IdentityManager.h:38` — X509 + `EVP_PKEY` | This is the **GameStream client identity** used towards Sunshine. Reusing it would conflate two roles; do not. |
| OpenSSL 3 | Already a direct dependency (input encryption, `CertManager`, TLS forced to OpenSSL) | ECDSA P-256 signing and verification are available server-side with no new dependency. |
| `NetClassify` | `backend/src/server/NetClassify.h` (shipped in phase 0) | Reused by the migration rule (§7). |

There is **no fingerprint extraction helper** in the tree today; §6 says where
one belongs.

---

## 3. The pairing step (once per browser × host, done locally)

1. The browser generates an **ECDSA P-256** key pair via WebCrypto with
   `extractable: false`, and stores the `CryptoKeyPair` in **IndexedDB**.
   JavaScript can ask for a signature; it can never read the private key —
   including a malicious bootstrap (§8).

   *Why P-256 and not Ed25519*: Ed25519 in WebCrypto only landed recently
   across Chrome/Safari/Firefox. P-256 is universal, and non-extractable
   everywhere.

2. The browser sends its **public key** (SPKI — always exportable) together
   with the PIN, on the existing authentication route.

3. The host validates the PIN, **stores the public key bound to that session**,
   and returns **its own host public key**, which the browser stores alongside
   its own.

The host needs a **dedicated host identity key**, ECDSA P-256, generated on
first start and persisted next to the application state. Not
`IdentityManager`'s — different role — and it must survive application updates,
or every browser would silently re-pair.

---

## 4. Per connection: two signatures, each verifiable *before* DTLS

This is the design decision that determines whether the scheme is sound.

A naive version has the host sign a digest that includes the browser's
fingerprint. But the host only learns that fingerprint from the answer — so it
could only sign *after* the answer, which means the browser cannot verify the
host until it has already started its own DTLS handshake. That leaves a window
of a few hundred milliseconds with an open channel to an unverified peer.
**Unacceptable for a channel that carries keyboard input.**

→ **The host's signature must not depend on the browser's fingerprint.**

```
0.  browser → host :  hello  { keyId, nonceB }

1.  host → browser :  offer  { sdp, nonceH, sigH }
       sigH = Sign_host( "MW-BIND-v1|host" ‖ hostId ‖ nonceB ‖ fpH )

2.  browser: verify sigH BEFORE setRemoteDescription.
             On failure → abort. No DTLS state is ever created.

3.  browser → host :  answer { sdp, sigB }
       sigB = Sign_browser( "MW-BIND-v1|browser" ‖ hostId ‖ nonceH ‖ fpH ‖ fpB )

4.  host: verify sigB BEFORE setRemoteDescription. On failure → abort.
```

- `fpH` / `fpB` — the SHA-256 fingerprint extracted from the SDP, normalised
  (uppercase hex, bytes separated by `:`).
- `hostId` — the instance identifier (the future 16-character base36 id).
- `nonceB` / `nonceH` — ≥ 128 bits, single use.
- `‖` — concatenation of length-prefixed fields, so no field boundary is
  ambiguous.

### Why it holds

| What the server tries | What stops it |
|---|---|
| Substitute `fpH` towards the browser | It cannot produce `sigH`: it does not hold the host key. Step 2 fails. |
| Substitute `fpB` towards the host | It cannot produce `sigB`: it does not hold the browser key. Step 4 fails. |
| Replay a `sigH` captured elsewhere | `nonceB` is fresh and chosen by *this* browser. |
| Replay a `sigB` captured elsewhere | `nonceH` is fresh, and `fpH` ties the signature to *this* host fingerprint. |
| Relay a legitimate `sigB` to the host while showing `fpEvil` to the browser | `sigB` covers `fpH`: the host recomputes with its real fingerprint and rejects. |
| Strip the signatures entirely | Both sides **fail closed** — a missing signature is a failure, never a skip. |

---

## 5. Non-negotiable hardenings

- **Sign an extracted canonical tuple, never the raw SDP text.** The frontend
  rewrites SDP (low-latency transformations, `modifiedAnswer`) and the backend
  rewrites candidates. Signing the SDP would break at the first adjustment.
- **Collect *every* `a=fingerprint:` line**, require them to be **identical**
  and to be **`sha-256`**. Reject otherwise. Without this, an attacker adds a
  second fingerprint line next to the legitimate one and picks which one the
  stack uses.
- **Domain separation** (`|host` / `|browser`) so a signature produced in one
  role can never be replayed in the other.
- **Fail closed everywhere**: absent, malformed, or unverifiable signature ⇒
  abort. There must be no configuration flag that disables verification.
- **Verify before `setRemoteDescription`**, not after. The whole point is that
  no DTLS state exists when verification fails.
- Constant-time comparison for anything secret-adjacent; nonces from a CSPRNG.

---

## 6. Where this lives in the code

| Element | Location |
|---|---|
| Fingerprint extraction + normalisation | New helper next to `RelayBase` — both WebRTC relays need it, and neither has it today. |
| Signature emission / verification | `SignalingServer` — **not** `HttpServer`. The signaling channel is what must refuse, before `setRemoteDescription` (`SignalingServer.cpp:325-330`). |
| Host key, paired browser public keys | `AuthManager` + `AppSettings`, next to `SessionInfo`. |
| Browser side | `BackendClient.js` (pairing, IndexedDB) and both `WebRtc*.js` (sign/verify around the SDP exchange). |

---

## 7. Migrating existing installations

**Retained: silent TOFU, restricted to LAN and tunnel peers.**

On the first authenticated request from a session that has no registered public
key, the frontend generates the key pair and POSTs the public key **with its
existing cookie**; the host binds it. That is trust-on-first-use over a channel
that is already authenticated.

**Guard rail**: this silent binding is allowed **only** when the session comes
from a `Loopback | Private | Tunnel` peer — exactly
`NetClassify::isTrustedPeer()`, already shipped in phase 0
(`backend/src/server/NetClassify.h`). A remote session must go through the PIN
again, and sees it in the UI. Zero friction for home use, and nothing binds
blindly from the internet.

While no introduction server exists there is no man in the middle to fear —
which makes now exactly the right time to migrate the installed base.

---

## 8. What this scheme does **not** cover

State plainly, do not paper over:

- **A malicious bootstrap is unchanged.** A non-extractable key cannot be
  *exfiltrated*, but it can be *used* while the malicious code runs in the
  victim's browser. The mitigations there are separate: hosting the bootstrap
  as static, immutable files on infrastructure distinct from the dynamic
  introduction server, pinning its hash, and watching it from outside that
  infrastructure. Never present the watchdog as a guarantee: an attacker who
  serves the good file to the watchdog and a bad one to a chosen victim stays
  green.
- **Metadata.** Even a perfectly blind introduction server knows who is online,
  when, from which IP, and how often. Inherent to the role; it belongs in the
  privacy policy, not in a mitigation list.
- **Passkeys / WebAuthn** would close the "used while the malicious code runs"
  window too — key held in the TPM via Windows Hello, bound to the origin, with
  the signed challenge able to cover the DTLS fingerprint. Worth keeping as an
  evolution, at the cost of a biometric gesture per session. Not the base layer.

---

## 9. How this gets tested

A small **hostile local WebSocket proxy** sits between the browser and
`SignalingServer` and rewrites the `a=fingerprint` line in both directions.

**Expected: both sides refuse, before any DTLS state exists.**

This is *the* test of the architecture. Without it the promise is hoped for,
not verified — so the harness is a deliverable of the implementation phase, not
a note of intent. It should also cover: a stripped signature, a replayed
signature from another session, a second `a=fingerprint` line added alongside
the legitimate one, and an `sha-1` fingerprint.

---

## 10. Open questions for the reviewer

1. Is the asymmetry of §4 (host signature independent of `fpB`) correct, or is
   there an attack that exploits the host committing to `fpH` before it knows
   anything about the browser beyond `nonceB` and `keyId`?
2. `hostId` is inside both digests to stop a signature being replayed towards a
   different instance. Is that sufficient, or should the browser's key id be in
   the host's digest as well?
3. Is TOFU over an authenticated cookie (§7), restricted to non-public peers,
   an acceptable migration — or should every existing installation be made to
   re-enter a PIN once?
4. Renewal and revocation are deliberately absent from v1: a browser that loses
   its IndexedDB re-pairs with a PIN, and a host that loses its key invalidates
   every pairing. Is that acceptable, or does v1 need an explicit revocation
   path?
