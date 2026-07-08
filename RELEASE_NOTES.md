## MoonlightWeb v0.1.0 — first public release

**MoonlightWeb streams your [Sunshine](https://github.com/LizardByte/Sunshine) gaming PC to any device with a browser** — phone, tablet, laptop or TV. Free and open source. You install one app on the gaming PC only; every other device just opens a URL. Nothing to install on the client.

### Highlights

- **Zero-install client** — the client *is* the browser. Open the server URL on any phone, tablet, computer or TV and start playing.
- **Low-latency WebRTC streaming** — video, audio and input travel over WebRTC (DataChannels + native media tracks) with an automatic transport fallback chain, so it works across LAN, NAT and restrictive networks.
- **Modern codecs** — H.264, HEVC and AV1, with real **HDR** passthrough and optional **YUV 4:4:4** chroma for crisp text and desktop use.
- **WebGPU Video Enhancement** — in-browser upscaling and sharpening (SGSR + FSR-style Video Super Resolution) makes a lower-bitrate stream look noticeably sharper.
- **One-click Internet access** — turn on a secure public URL with automatic DNS and a real TLS certificate (ZeroSSL/ACME), or stay strictly LAN-only. UPnP NAT traversal with STUN fallback gets you connected without manual port forwarding.
- **Full input support** — keyboard, mouse (with a gaming pointer-lock mode), **Xbox/PlayStation gamepads with rumble**, touch trackpad gestures on mobile, and an on-screen virtual keyboard.
- **Automatic host discovery & pairing** — Sunshine hosts on your network are found automatically; pairing is guided and fully asynchronous.
- **Fullscreen, HiDPI, live stats** — proper fullscreen on desktop and mobile, HiDPI rendering, and an on-stream stats overlay (bitrate, FPS, RTT).
- **Guided setup wizards** on every platform, plus an admin page to manage Internet access, streaming defaults (resolution / frame rate / bitrate / codec) and Sunshine.
- **Privacy-first Internet opt-in** — Internet access is off by default and requires explicit consent, which is recorded for traceability; a host-key mechanism lets the machine you installed on reach its own admin over the public domain safely.
- **English & French** interface.

### Downloads

Install on the **gaming PC** only:

| Platform | Asset |
|---|---|
| Windows x64 | `MoonlightWeb-installer-win-x64.exe` |
| Windows ARM64 | `MoonlightWeb-installer-win-arm64.exe` |
| macOS (Apple Silicon) | `moonlightweb-macos-arm64.pkg` |
| Linux (Debian/Ubuntu/Mint) | `moonlightweb-linux-x64.deb` |
| Linux (Fedora/RHEL/openSUSE) | `moonlightweb-linux-x64.rpm` |
| Linux (portable) | `moonlightweb-linux-x64.AppImage` |

Playing from a phone, tablet or TV? Nothing to download — just open your server's URL in the browser.

### Requirements

- A gaming PC running **Sunshine** (the installer can set it up for you).
- A modern browser on the client device (Chrome/Edge recommended for full codec, HDR and WebGPU support).

---

Built on the shoulders of [Moonlight](https://moonlight-stream.org/) and [Sunshine](https://github.com/LizardByte/Sunshine). Licensed under GPL-3.0.
