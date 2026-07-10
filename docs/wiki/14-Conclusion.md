[← Roadmap & Constraints](13-Roadmap-and-Constraints.md) · **Conclusion**

---

# 14. Conclusion

MoonlightWeb's bet is simple to state and hard to execute: **the browser is a good enough streaming client that nothing should need installing on it**. Everything in this repository serves that bet:

- A **C++/Qt server** reuses the canonical GameStream implementation (`moonlight-common-c`) instead of reinventing it, and spends its complexity budget where the web platform demands it — re-encapsulating a UDP-native, latency-critical protocol into transports a browser can actually open (WebRTC DataChannels, RTP tracks, WSS), with a fallback chain that degrades gracefully instead of failing.
- A **framework-less frontend** keeps the client honest: no build step, no dependencies to rot, just the web platform's real capabilities — WebCodecs for decode, WebGPU for rendering and enhancement, AudioWorklet for glitch-free audio, Pointer Lock and the Gamepad API for input.
- The **operational story** is treated as a feature, not an afterthought: one-click public exposure (DNS + ACME + UPnP with port parity and ownership guarantees), native installers that leave a paired, running system behind, self-healing entry points, consent auditing, and an in-process abuse shield.
- The **infrastructure** (PowerDNS/dnsdist/Caddy) is deliberately boring: official images, one process per container, a documented installer, and a short list of manual steps that only a domain owner can perform.

The codebase's most valuable asset is its **accumulated negative knowledge** — the reorder buffer that was removed, the clock-offset latency display that froze, the nested event loop that crashed, the Schannel backend that couldn't load a PEM key. This wiki exists to keep that knowledge cheaper to read than to rediscover. If you contribute one thing, make it this: when you fight the pipeline and win, write the invariant down — in [chapter 5](05-Streaming-and-Transports.md)'s tables for humans, and in the agent instructions of [chapter 12](12-Agentic-Coding.md) for the machines that increasingly write code here too.

**Where to go next**

- Contribute: [CONTRIBUTING.md](../../CONTRIBUTING.md) → [Build, CI & Testing](11-Build-CI-Testing.md)
- Understand a stream end-to-end: [Architecture](02-Architecture.md) → [Streaming & Transports](05-Streaming-and-Transports.md)
- Operate the DNS side: [PowerDNS Stack](10-PowerDNS-Stack.md)
- Open problems worth taking: [Roadmap & Constraints](13-Roadmap-and-Constraints.md)

---

[← Roadmap & Constraints](13-Roadmap-and-Constraints.md) · [Home](Home.md)
