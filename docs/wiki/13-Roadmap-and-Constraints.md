[← Agentic Coding](12-Agentic-Coding.md) · **Roadmap & Constraints** · [Next: Conclusion →](14-Conclusion.md)

---

# 13. Roadmap, Constraints & Improvement Leads

An honest inventory of what remains, what constrains the design, and where the leverage is. Sources: in-repo plans (`docs/moonlightweb-plan.md`, `docs/audit-*` — `docs/internet-plan.md` describes a superseded design and is kept only as an archive — this wiki, not that file, is current), TODO-class comments, and the development history.

## 13.1 Known remaining work

| Area | Item | Status / notes |
|---|---|---|
| Gamepad | **Phases 2–3**: non-standard controllers (wheels, HOTAS) per-device remapping, richer haptics | Phase 1 MVP shipped (standard mapping + rumble); ignoring non-standard pads is deliberate (`GamepadManager.js`) |
| Video Enhancement | HDR-aware enhancement shipped for **AV1** (`rgba16float` linear path, FSR1 in HDR); HEVC HDR still goes through `<video>` with no Enhancer | Waits for Chrome to let a hardware-decoded HDR frame reach a shader untouched ([ch. 5 §5.3](05-Streaming-and-Transports.md#53-hdr--support-and-limitations), [ch. 15](15-Client-Presentation-Benchmarks.md)) |
| Frontend threading | OffscreenCanvas decode/render worker: `auto` resolves to off (measured no gain; WebGPU in a worker is a 22 ms hand-off on macOS); `video_worker: on` forces it | Revisit only with a measurement on Mac **and** Windows |
| Native host audio | **v0.3.0**: the host keeps hearing its own audio while streaming (WASAPI loopback captures what reaches the speakers), where Sunshine mutes it (`localAudioPlayMode`). Study a virtual audio endpoint or an equivalent that lets the capture run while the speakers stay silent | Decided 2026-09-04: accepted for v1, to be solved before v0.3.0 |
| Client presentation | Two measurements left open in [ch. 15](15-Client-Presentation-Benchmarks.md): Canvas2D slower than FSR1 WebGL2 in one windowed SDR series; WebGPU ahead of WebGL2 fullscreen | Confirm on game content before touching a default |
| Multi-instance NAT | Two instances behind one NAT coexist (deterministic fallback ports), but a "process dies" crash report in that setup remains to be root-caused | `upnp-multi-instance` follow-up |
| Input | Clipboard sync and NumLock host-sync shipped but flagged "to validate E2E" on more device matrices | |
| C++ quality tooling | clang-format + cppcheck gate exists; **clang-tidy is not yet a CI gate** (`run_clang_tidy.sh` is local-only) | |
| DC frame ordering | The ordered-DC + FrameSender→IDR fix for stutters needs validation on iPhone/Wi-Fi matrices | |

## 13.2 Structural constraints (accept, don't fight)

| Constraint | Consequence |
|---|---|
| **`moonlight-common-c` is a process-global singleton** | One session **per process** — hence take-over semantics and deferred starts. Lifted to *two* concurrent slots by running each session in a `--stream-worker` child (which is what makes seamless quality switching possible); going beyond that is a matter of generalizing the slot table, not of architecture. |
| **Browsers only** | Codec support is at the browser's mercy (HEVC/AV1 availability varies; MediaTrack path is H.264-only); no raw UDP from JS — everything rides WebRTC/WSS. |
| **Canvas import is SDR-referred** | `importExternalTexture`/`drawImage` tone-map HDR on import; the signal survives only via `copyTo` of software-decoded frames. Hence true HDR on a canvas = **AV1, software decoded** (~10 ms more than SDR), and HDR with HEVC = the `<video>` sink, the slowest presenter, without the Enhancer ([ch. 15](15-Client-Presentation-Benchmarks.md#155-decision-4--hdr-routing)). |
| **The `/start` response precedes ICE** | The client must own transport fallback; the server can never know a transport failed. |
| **Self-signed cert on LAN** | A first-visit browser warning is unavoidable without a user-provided domain and certificate. |
| **UPnP/NAT reality** | CGNAT/double-NAT cannot be traversed (detected + reported); routers rarely hairpin UDP (hence local-candidate advertisement to LAN clients). |
| **Single infrastructure box** | The introduction server, STUN and the authoritative DNS all run on one VM: no anycast, no volumetric-DDoS absorption. While it is down, no *new* remote session can be introduced — established streams are peer-to-peer and unaffected. `stream.` is a name of its own precisely so it can be repointed, or spread, without moving anything else ([§10.4](10-Infrastructure-Stack.md#104-caddy-caddy)). |
| **Reachability depends on a held line** | An instance is reachable exactly while its WebSocket is up. This is the price of publishing nothing, and it means a remote session — LAN included, since there is no separate local path any more — depends on the instance having internet. |
| **No accounts / single settings store** | By design (server-side `settings.json`, no multi-user) — features requiring per-user server state should map to per-browser localStorage or be rethought. |
| **Sunshine's HTTP fragility** | Host polling must stay suspended during streams; one HTTPS request per host at a time. |
| **macOS notarization needs a paid Apple Developer ID** | No free tier, no OSS programme — a *downloaded* `.pkg` will always be refused by Gatekeeper. The Homebrew cask and `install.sh` route around it (`installer(8)` never consults Gatekeeper), so the fix is a documented command, not a signature ([§9.2](09-Installers-and-Packaging.md)). |
| **Compile-time service credentials** | Any build system that compiles from source on its own infrastructure (PPA, COPR, OBS) yields a binary that silently loses the update relay and the censuses — hence self-hosted signed APT/DNF repositories shipping the same binary as the release ([§9.3](09-Installers-and-Packaging.md)). Likewise Flatpak/Snap are out: a sandbox can neither run the host package manager to install Sunshine nor open firewall ports. |

## 13.3 Improvement leads

**Streaming quality**
- Bandwidth estimation on the DC path (receiver-side rate feedback) to drive the degradation ladder proactively rather than reactively — **done for the native host** (September 2026: the receiver reports its one-way-delay rise and frame gaps twice a second, the host's rate governor moves the encoder's bitrate between two frames, the ladder stands down); the GameStream hosts still rely on the reactive ladder.
- FEC on the DataChannel video path (moonlight-common-c already exposes RTP FEC data) to reduce IDR dependence on lossy links.
- Extend `JitterController` telemetry into the stats overlay for user-visible network diagnosis.

**HDR**
- Track Chrome's handling of hardware-decoded HDR frames in WebGPU (`importExternalTexture` keeping PQ, or `copyTo` on hardware frames): either one lets HEVC HDR leave the `<video>` sink and cuts the software-AV1 cost of the `linear` path.
- HDR on the native host (FP16 capture → P010 → HEVC Main10 / AV1 10-bit) — the "HDR host, SDR client" ACES case is real only on Sunshine today.

**Platform & ops**
- Make the ASan workflow a scheduled job; add clang-tidy to CI as advisory → blocking.
- Linux ARM (Raspberry-class LAN bridges) is a natural next build target given the CMake matrix.
- A second introduction server behind `stream.` (and a real `ns2` on another IP): the name is already separate and the block self-contained, so what remains is sharing the claim store rather than moving anything.
- Symbol upload + crash-report ingestion (currently: local minidumps + manual cdb symbolization).

**Product**
- Multi-session beyond the current two slots: the per-stream worker process shipped, so what remains is a variable-size slot table (ports, take-over rules, UI) instead of a hard-coded pair.
- Bring-your-own-domain is configured by hand in `settings.json` ([§7.5](07-Settings-Reference.md#75-bring-your-own-domain--certificate)); an admin-page field + a certificate panel (CN, expiry, source) would remove the file editing and surface an ageing certificate before the browser does.
- Host-side virtual display management (resolution matching without changing the host desktop).
- More locales (the i18n runtime + Tolgee flow make this cheap; zh shipped recently).
- Optional TURN relay fallback for networks where even ICE-TCP fails but WSS latency is unacceptable.

**Documentation & DX**
- Keep this wiki in the PR definition-of-done for behavior changes (especially ch. 5 invariants).
- Commit the `.claude/` + `.github/instructions/` layouts from [ch. 12](12-Agentic-Coding.md) so agent-assisted contributors start configured.

---

[← Agentic Coding](12-Agentic-Coding.md) · [Home](Home.md) · [Next: Conclusion →](14-Conclusion.md)
