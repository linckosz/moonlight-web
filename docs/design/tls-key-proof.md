# MW-TLS-v1 and MW-PIN-v1 — a TLS key proved by the host key, a PIN bound to its channel

**Status**: proposed, for the security review. Nothing here ships before that review.
**Audience**: reviewers. It assumes [pairing-signature.md](pairing-signature.md) (MW-BIND-v1).
**Date**: 2026-11-05, revised 2026-11-06.

Two additions for a client that pins this host's TLS key on first contact instead of trusting a CA — the Android TV
app first. Both are additive: a client that knows neither sees no change, and `POST /api/auth/validate` stays.

---

## 1. Why

**A TLS key that changes is not always an attack.** `CertManager` keeps the local key across restarts since v0.4.0,
but `local-key.pem` can be deleted or damaged, an owner's own certificate (served to a client that sends no SNI, as
Android does for an IP address) can be renewed with a new key, and a settings folder can be copied to another PC
without `cert/`. Today the pinning client can only say "the server's key changed" and ask for a new PIN. The host
key, which MW-BIND already trusts for every stream, can say instead that the new key is its own.

**A PIN bound to the TLS key the client sees.** `/api/auth/validate` takes the PIN itself, inside the TLS connection
the client pinned. MW-PIN-v1 proves the PIN instead, and binds the proof to the TLS key each end sees, so that a run
relayed between two TLS keys proves nothing. Binding a PIN to that key takes more than one might think:

- a field "here is the key I see" in the login body: whoever ends the TLS can rewrite it;
- a hash of the PIN with that key: six digits fall to an offline search in well under a second;
- a PIN derived from the key: 2^20 keys are cheap to try.

The only secret the two ends share is the PIN, and only a password-authenticated key exchange keeps a low-entropy
secret from an offline search. Hence SPAKE2.

**What that holds** is the pairing with a host the client has already seen prove PINs: the client sends that host no
PIN any other way. A first contact can still be downgraded to the plain PIN route (§3, *Compatibility*).

---

## 2. MW-TLS-v1

`GET /api/auth/tls-proof?nonce=<base64url>`, public (it comes before anything of the client's session goes out on a
key the client does not recognise yet).

```
sig = Sign_host( "MW-TLS-v1|host" ‖ hostId ‖ nonce ‖ SHA-256(SPKI) )      buildSignedMessage framing
reply {protocol: "MW-TLS-v1", host_id, host_public_key, tls_key: base64url(SHA-256(SPKI)), sig}
```

- **SPKI is the key this very connection was served** (`HttpRequest::tlsSpki`, set by `HttpServer` from the socket
  once its handshake is done). Never a key the caller names: the route cannot be used to vouch for anyone else's
  certificate. 404 where the client does not see that key: no TLS (plain HTTP, the tunnel), or a proxy in between —
  a loopback peer (Tailscale Funnel, cloudflared) or a request carrying `CF-Connecting-IP`, `X-Forwarded-For`,
  `Forwarded` or `X-Real-IP` (`servedKeyIsSeen` in `PinProofRoutes.cpp`). A machine in the middle gains nothing by
  writing those headers: it can answer 404 itself.
- The nonce is the client's, 16 to 64 bytes.
- **A budget**: 10 proofs per caller (rate-limit bucket) and per minute, then 429 (`PinBinding::CallBudget`, at most
  4096 callers a minute, then nobody new until the minute has passed). A client asks once per key that changed. The
  host key is read once, when the routes are registered.
- **Domain separation**: the first field `MW-TLS-v1|host` begins no other message the host key signs
  (`MW-BIND-v1|host` for streams and the line), so a proof can never pass for an MW-BIND signature, nor the reverse.

What the client does (the TV app, `TlsProof.kt`): when a call fails on a pin mismatch, it asks on a connection of its
own, **pinned to the key it was shown** (its handshake refuses any other, so the answer came over that key), with no
cookie and no redirect; it rebuilds the signed bytes from its own values only (the host key and id it was given at
pairing, its nonce, the key it saw) and re-pins only on a valid signature. Anything else leaves today's "key changed"
screen; a 404 or a refusal is not asked again for that key, a 429 or a 5xx may be.

---

## 3. MW-PIN-v1

SPAKE2, RFC 9382, ciphersuite P256-SHA256-HKDF-HMAC (`common/PinBinding.{h,cpp}`), with the RFC's M and N. The three
routes are in `server/routes/PinProofRoutes.cpp`, registered by `registerAuthRoutes`.

| | |
|---|---|
| A | the client, goes first, uses M. Identity: its MW-BIND public key (SPKI DER) |
| B | this host, uses N. Identity: its host key (SPKI DER) |
| w | SHA-512(`buildSignedMessage("MW-PIN-v1\|w", PIN)`) mod n, 32 bytes. No memory-hard function: it would only protect a w read out of this process, where the PIN is |
| channel | direct: `buildSignedMessage("tls", SHA-256(SPKI served))`; tunnel: `buildSignedMessage("line", SHA-256(host key))` |
| AAD | `buildSignedMessage("MW-PIN-v1", channel)`, into KcA ‖ KcB as RFC 9382 §4 says |

```
client                                               host
POST /api/auth/pin/start {protocol, public_key, pa}  pa not a point? 400. No TLS key here? 404.
                                                     Table of runs full? 503, nothing counted.
                                                     Locked out? 429. Else the attempt is counted now
                                                     (beginPinProof); y, pB, K, TT, cA and cB expected.
                  {exchange, pb, host_public_key, host_id, tls_key (not on the tunnel)}
client: tls_key is not the key it sees? It stops here.
POST /api/auth/pin/finish {exchange, confirm: cA, machine_name, remember}
                                                     run taken once, same address, PIN unchanged, cA checked
                                                     (finishPinProof): a good one clears the count, a wrong
                                                     one is 401 and not counted a second time.
                  as /validate, plus confirm: cB, and the session cookie
client checks cB: it was this host, on its channel, that knew the PIN.
```

- **A relay fails.** The machine in the middle can pass both rounds along untouched: the exchange and K are the same
  at both ends, but the client binds the key it is shown and this host the key it served, so KcA and KcB differ and
  `finish` refuses. A party in the middle gets one guess per run, counted on the PIN's bucket and lockout tiers.
- **The run is the attempt, counted at `start`.** For pA = w·M with the right w, the host's K is the identity
  (RFC 9382 §7). Had `start` answered that case differently, and counted nothing, a million requests would have named
  the PIN. So `start` checks up front only what does not depend on the PIN (pa is a point of the group), answers a run
  whose K is the identity like any other, and keeps it as one that never proves. K is computed as
  y·pA − ((y·w) mod n)·M, so that in K, w never meets a point without the run's fresh y; the share, y·P + w·N, and K
  each take two single-scalar multiplications (OpenSSL's constant-time path) and one addition.
- **No channel the client shares** (plain HTTP, or a proxy in front, by the rule of §2): `start` answers 404, as an
  older host does. The client then sends the PIN to `/validate`, or refuses to, towards a host it has seen prove PINs.
  A proxy that re-encrypts to this host (cloudflared pointed at `https://localhost`) would otherwise bind its own key
  at the client's end and this host's at this end, and no honest proof could ever match.
- **The key this socket served, named.** The `start` answer carries `tls_key`, base64url of the SHA-256 of the SPKI
  this socket served, as the TLS proof names it. A client that sees another key (something between them ends its TLS
  and adds none of the headers above) stops before `finish` and tells its user so. The field proves nothing: rewritten
  on the way, it only leaves the run to fail at `finish` like any relay.
- **The runs** (`PinBinding::Exchanges`): in memory, single use, 60 s, 4 per rate-limit bucket (that bucket's oldest
  gives way to its new one) and 256 in all. Full, the table takes no new caller's run: `start` answers 503 before it
  counts anything, and no run already kept gives way to it. Only the bucket that began a run can take it. A run made
  under a PIN since spent or replaced proves nothing.
- **The key bound to the session** is the run's `public_key`, never one the `finish` body names. The host identity in
  the `finish` answer is the one `start` gave: the host key as read when the routes were registered.
- **On the tunnel** the line's `hello` already named the client's key: `start` refuses another one. The line's
  channel names the host key, which the transcript already carries as B; what ties the run to this host there is
  what the client checks of the line: the host key's MW-BIND signature over the line's DTLS fingerprint, and
  `host_public_key` equal to the key the line's offer showed.
- `--noadmin` refuses both rounds, as it refuses `/validate` (`RequestGuard::lockedByNoAdmin`).

**Compatibility.** A host without the routes answers 404, and the client falls back to `/validate`, except towards a
host it has seen prove PINs before: it then refuses to send the PIN at all. That is what MW-PIN-v1 protects: the
pairing with a host already seen to prove PINs. A first contact can still be downgraded to the plain PIN route, since
anything that answers in this host's place can answer 404 as an older host does. A sign the user could check out of
band is left for a later study.

---

## 4. What this does not cover

- **A first contact**, which can still be downgraded to the plain PIN route (§3, *Compatibility*).
- **A stolen host key** (`settings.json`) now also lets its holder prove any TLS key to a pinning client on the LAN,
  where it also needed the TLS key before. Both live in the same folder of this machine; on the internet path the
  host key alone was already enough.
- **A stolen TLS key**: it is the pin itself, and the cookie travels inside that TLS. Only a per-connection proof of
  the client's key would help (below).
- **The cookie alone still drives this host on the LAN.** Requiring the key there would take a proof per TLS
  connection (a nonce tied to the socket, signed by the session's key over the SPKI the client sees), state per
  socket, and a flag so that only clients that promise it (sessions made by MW-PIN-v1) are held to it. Not here.
- **The web page** keeps `/validate`. It could adopt MW-PIN-v1 with a JavaScript P-256, chiefly for the rendezvous.
- **Every certificate this host serves is vouched for.** SNI picks the local certificate or the owner's; both the
  proof and the PIN's channel say "a key this host serves". Whoever holds the private key of the owner's certificate
  (renewed on another machine, say) can relay with the SNI that selects it. Putting the SNI into the signed fields
  would not help a client that sends none, as Android does for an IP address. That key is the host's TLS key, like
  `local-key.pem`.
- **Availability under a flood from many addresses** (IPv6 /64s are cheap): the table of runs and the proof budget
  have a size. Full, the table answers 503 to a new run without counting it, and the budget gives nobody new a proof
  for a minute or two; nothing already begun is dropped for it. Options for the review: group IPv6 by /56 for these
  two tables, a larger table.

---

## 5. Tests

`tests/test_pin_binding.cpp` (the cross-platform security runner, next to MW-BIND's):

- RFC 9382's four vectors, element by element, transcript, keys and both confirmations;
- the framing of the channel, the AAD and the proof message, checked byte by byte against hand-written hex, and one
  full MW-PIN-v1 run with fixed inputs whose outputs the TV app's tests computed;
- a wrong PIN, and a relay between two TLS keys: no confirmation in either direction;
- elements that are not points of the group, scalars out of range, what `isElement` refuses;
- the share w·M: a point like any other, no keys under the right PIN, keys under another;
- the runs: single use, another caller's refused, expiry, the caps (a caller's own oldest gives way, a full table
  takes no new caller's run and keeps those it holds); `sameBytes` on inputs of different lengths;
- MW-TLS-v1: a proof holds for this key, this nonce and this host only; a certificate's SPKI as a client reads it;
  the budget of proofs, per caller, per minute, and under a flood of callers.

`tests/test_auth_manager.cpp` (`run_tests`):

- `beginPinProof` and `finishPinProof` — counted at the start, cleared by a proof, a wrong proof not counted twice, a
  run left unfinished, a changed PIN, the sentinel, three runs locking the bucket with the third still good, one
  bucket with `/validate`;
- the three routes, driven on a router of their own (`registerPinProofRoutes`) with a real `AuthManager`: a run from
  start to finish (the session, the key bound to it, `tls_key`, the host's confirmation and identity, the count
  cleared); the same `finish` again, and one from another address, 410; a wrong PIN and a confirmation one bit off,
  401 and counted once; pA or pB replaced on the way by another point of the group; a client behind another TLS key;
  the PIN shown anew, or spent by another run, between the two rounds; the share w·M answered in the same shape as
  any run and never proved; 404 and nothing counted for plain HTTP, a loopback peer and each forwarding header; the
  tunnel (the hello's key and no other, no `tls_key`, no TLS proof); what `start` refuses up front, uncounted; the
  TLS proof (its signature, the nonce's bounds, ten a minute); a full table (503 uncounted, the first run still
  proves, room again once it has).

`tests/test_request_guard.cpp`: both rounds closed under `--noadmin`.

## 6. Questions for the review

1. SPAKE2 over CPace: CPace needs hashing onto the curve, SPAKE2 only the operations OpenSSL and the client already
   have. RFC 9382 warns that SPAKE2 was not the CFRG's pick; its security proof holds for this use.
2. `start` counts the attempt and a good `finish` clears it. Counting at `finish` only would leave `start` free, and
   any difference in what it answers, or how fast, would be a free test of a PIN. The cost: a run left between its
   rounds (a dropped connection, a client that stops at a TLS key it does not see) counts as a wrong PIN.
3. Should `/validate` refuse a client that announced MW-PIN-v1 elsewhere? The host cannot tell who is who; the client
   side refuses its own downgrade instead.
