[← Agentic Coding](12-Agentic-Coding.md) · **Roadmap & Constraints** · [Next: Conclusion →](14-Conclusion.md)

---

# 13. Roadmap, Constraints & Improvement Leads

An honest inventory of what remains, what constrains the design, and where the leverage is. Sources: in-repo plans (`docs/moonlightweb-plan.md`, `docs/internet-plan.md`, `docs/audit-*`), TODO-class comments, and the development history.

## 13.1 Known remaining work

| Area | Item | Status / notes |
|---|---|---|
| Gamepad | **Phases 2–3**: non-standard controllers (wheels, HOTAS) per-device remapping, richer haptics | Phase 1 MVP shipped (standard mapping + rumble); ignoring non-standard pads is deliberate (`GamepadManager.js`) |
| Video Enhancement | **C7 (optional)**: HDR-aware enhancement path (f16/compute pipeline) | C1–C6 complete & validated; blocked on the canvas-HDR limitation ([ch. 5](05-Streaming-and-Transports.md#53-hdr--support-and-limitations)) |
| Frontend threading | OffscreenCanvas decode/render worker is **opt-in** (`mw_video_worker=1`) | Promote to default after broader device validation |
| Multi-instance NAT | Two instances behind one NAT coexist (deterministic fallback ports), but a "process dies" crash report in that setup remains to be root-caused | `upnp-multi-instance` follow-up |
| Input | Clipboard sync and NumLock host-sync shipped but flagged "to validate E2E" on more device matrices | |
| C++ quality tooling | clang-format + cppcheck gate exists; **clang-tidy is not yet a CI gate** (`run_clang_tidy.sh` is local-only) | |
| DC frame ordering | The ordered-DC + FrameSender→IDR fix for stutters needs validation on iPhone/Wi-Fi matrices | |

## 13.2 Structural constraints (accept, don't fight)

| Constraint | Consequence |
|---|---|
| **`moonlight-common-c` is a process-global singleton** | One stream per server process — hence take-over semantics, deferred starts, single-session backend. True multi-session would require multi-process relays. |
| **Browsers only** | Codec support is at the browser's mercy (HEVC/AV1 availability varies; MediaTrack path is H.264-only); no raw UDP from JS — everything rides WebRTC/WSS. |
| **Canvas is SDR-referred** | Real HDR requires the `<video>` sink → HDR and WebGPU enhancement are mutually exclusive. Until browsers expose HDR canvas (WebGPU `rgba16float` + HDR compositing is still maturing), this stands. |
| **The `/start` response precedes ICE** | The client must own transport fallback; the server can never know a transport failed. |
| **Self-signed cert on LAN** | A first-visit browser warning is unavoidable without a public domain (or user-provided cert). |
| **UPnP/NAT reality** | CGNAT/double-NAT cannot be traversed (detected + reported); routers rarely hairpin UDP (hence local-candidate advertisement to LAN clients). |
| **Single DNS box** | The shared-domain infrastructure is one VM: no anycast, no volumetric-DDoS absorption; `ns2` on a second IP is the documented cheap upgrade. |
| **No accounts / single settings store** | By design (server-side `settings.json`, no multi-user) — features requiring per-user server state should map to per-browser localStorage or be rethought. |
| **Sunshine's HTTP fragility** | Host polling must stay suspended during streams; one HTTPS request per host at a time. |

## 13.3 Improvement leads

**Streaming quality**
- Bandwidth estimation on the DC path (receiver-side rate feedback) to drive the degradation ladder proactively rather than reactively.
- FEC on the DataChannel video path (moonlight-common-c already exposes RTP FEC data) to reduce IDR dependence on lossy links.
- Extend `JitterController` telemetry into the stats overlay for user-visible network diagnosis.

**HDR**
- Track browser HDR-canvas proposals (WebGPU HDR compositing / `CanvasHighDynamicRangeOptions`); when available, unlock Enhancement+HDR together (the planned C7).

**Platform & ops**
- Make the ASan workflow a scheduled job; add clang-tidy to CI as advisory → blocking.
- Linux ARM (Raspberry-class LAN bridges) is a natural next build target given the CMake matrix.
- A second authoritative DNS instance (`ns2`) + automated zone replication for the shared domain.
- Symbol upload + crash-report ingestion (currently: local minidumps + manual cdb symbolization).

**Product**
- Multi-session via per-stream relay processes (lifts the singleton constraint — the largest architectural change on the table).
- Host-side virtual display management (resolution matching without changing the host desktop).
- More locales (the i18n runtime + Tolgee flow make this cheap; zh shipped recently).
- Optional TURN relay fallback for networks where even ICE-TCP fails but WSS latency is unacceptable.

**Documentation & DX**
- Keep this wiki in the PR definition-of-done for behavior changes (especially ch. 5 invariants).
- Commit the `.claude/` + `.github/instructions/` layouts from [ch. 12](12-Agentic-Coding.md) so agent-assisted contributors start configured.

---

[← Agentic Coding](12-Agentic-Coding.md) · [Home](Home.md) · [Next: Conclusion →](14-Conclusion.md)
