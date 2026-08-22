# Security Policy

## Reporting a vulnerability

Email **brunoocto@gmail.com** with `[MoonlightWeb security]` in the subject.

Please include what you need to reproduce it: version (`MoonlightWeb --version`),
platform, transport mode if relevant, and the steps. A proof of concept helps but
is not required to report.

- **Acknowledgement:** within 7 days.
- **Assessment:** within 30 days, with a decision on whether it will be fixed and
  a rough timeline.
- **Disclosure:** please give 90 days before publishing, or until a fix ships if
  that comes sooner. Credit is given in the release notes unless you'd rather not
  be named.

This is a single-maintainer project, not a company with an on-call rotation. The
timelines above are what one person can commit to, and they are targets rather
than guarantees.

## Supported versions

Only the **latest release** receives fixes. There are no backports to older
versions.

## What this software does

MoonlightWeb streams a desktop and forwards keyboard, mouse and gamepad input to
it. Anyone who gets past authentication controls that machine — not just the
application. Treat a MoonlightWeb host the way you would treat any remote-desktop
host.

**Internet access is opt-in and off by default.** While it is enabled, the
router is asked (UPnP) to open a streaming port for the duration of each
session, and the peers of a WebRTC connection see each other's public IP
address. Nothing is published: no DNS record is created, no certificate is
issued, and the web interface's ports (80/443) are not opened. Disabling it
closes the mappings immediately.

**Installs that enabled internet access on v0.2.4 or earlier** registered a
`{id}.moonlightweb.top` subdomain with a publicly trusted TLS certificate.
Certificates issued by a publicly trusted CA are recorded in public Certificate
Transparency logs, so **the existence of those instances and their hostnames
are public information, permanently** — CT logs are immutable, and retiring the
mechanism stops new entries but cannot remove past ones. That is inherent to
publicly trusted TLS, not specific to this project. Those installs keep their
subdomain, certificate renewals and 80/443 forwards unchanged until the shared
DNS service **shuts down in February 2027**; disabling internet access from the
admin page ends all of it at any time.

## In scope

- Authentication and session handling (PIN, session tokens, pairing).
- The HTTP/WebSocket surface served by the backend.
- The signaling and WebRTC transport paths.
- The admin API and the local-request exemption.
- Anything reachable before authentication.

## Out of scope

- **Sunshine, Apollo and Wolf.** Report those upstream. In particular, the
  GameStream pairing and `/serverinfo` exchange on port 47989 is plaintext by
  protocol design; MoonlightWeb speaks it but does not define it. That port is
  never exposed to the internet by MoonlightWeb and is never mapped via UPnP —
  it matters only when Sunshine runs on a machine other than the one MoonlightWeb
  runs on, where a third party *on that network* could observe the exchange.
- A self-signed certificate warning on a LAN address. Expected: the LAN is
  served with a self-signed certificate unless you configure your own domain
  and certificate.
- The peer learning your public IP address. Inherent to peer-to-peer WebRTC, as
  it is for every product built on it.
- Physical access, or an already-compromised host or browser.
- Denial of service that requires an already-authenticated session.

## Dependency updates

The media port accepts unauthenticated packets only as far as the ICE and DTLS
handshakes: a datagram without valid ICE credentials is dropped without a reply,
and the DTLS handshake requires the fingerprint exchanged during signaling. That
handshake code is therefore the only surface an unauthenticated party can reach,
so **`libdatachannel`, `libjuice` and OpenSSL are kept current**, and a security
release in any of them is treated as a security release here.

## Third-party components

Vendored dependencies and their licenses are listed in [COPYRIGHT](COPYRIGHT).
Vulnerabilities in those belong upstream first; tell us anyway if MoonlightWeb
exposes them in a way upstream would not.
