<div align="center">

<img src="frontend/assets/logo.png" alt="Moonlight‑Web logo" width="128" />

# Moonlight‑Web

**Stream your PC games to any browser.**\
Install it on the gaming PC, open a URL on any other device — from your LAN or from anywhere.\
Under **20 ms** glass‑to‑glass over Wi‑Fi on a LAN, ~**25 ms** over the Internet.

**[Website](https://moonlightweb.top/)** · **[Install guides](https://moonlightweb.top/guides/windows.html)** · **[FAQ](https://moonlightweb.top/faq.html)** · **[Discord](https://discord.gg/wfbesPx4UB)**

[![Discord](https://img.shields.io/badge/Discord-join%20the%20chat-5865F2?logo=discord&logoColor=white)](https://discord.gg/wfbesPx4UB)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
![Qt](https://img.shields.io/badge/Qt-6.11-41CD52?logo=qt&logoColor=white)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)
![WebRTC](https://img.shields.io/badge/Transport-WebRTC-333?logo=webrtc)
![Platforms](https://img.shields.io/badge/Server-Windows%20%C2%B7%20Linux%20%C2%B7%20macOS-success)

![Home — the host list](docs/screenshots/home.png)

</div>

## Features

- **Streams its own machine** — built‑in capture and GPU encoding on Windows, Linux and macOS. No Sunshine, no pairing, no PIN.
- **Up to 4K HDR, 240 FPS** — H.264, HEVC and AV1 on NVENC, AMF, Quick Sync, VA‑API and VideoToolbox, with a software fallback.
- **Nothing to install on the client** — any modern browser on a PC, Mac, phone, tablet or TV.
- **Full input** — keyboard, mouse with pointer lock, touch trackpad, and Xbox/PlayStation gamepads with rumble.
- **Remote access (opt‑in)** — reach your PC at `stream.moonlightweb.top/<id>`, peer to peer, with no open web port and no DNS record.
- **Session sharing** — invite up to three people as viewer, gamepad player or full control.
- **Also a GameStream client** — pairs with Sunshine, Apollo, Wolf and MultiSeat hosts.
- **Video Enhancement** — GPU upscaling and sharpening in the browser.

<div align="center">

| Desktop | Mobile |
|:---:|:---:|
| ![Desktop streaming in the browser](docs/screenshots/desktop.png) | ![iPhone streaming with virtual keyboard](docs/screenshots/mobile.png) |

</div>

## Install

Install on the **gaming PC** only. Download from the **[latest release](https://github.com/linckosz/moonlight-web/releases/latest)** or **[moonlightweb.top](https://moonlightweb.top/#download)**.

| Platform | Package |
|---|---|
| **Windows 10/11** (x64, ARM64) | `MoonlightWeb-installer-<version>-win-<arch>.exe` |
| **macOS** (Apple Silicon) | `moonlightweb-<version>-macos-arm64.pkg` |
| **Debian, Ubuntu, Mint** | `.deb` |
| **Fedora, RHEL, openSUSE** | `.rpm` |
| **Arch** | [`moonlightweb-bin`](https://aur.archlinux.org/packages/moonlightweb-bin) (AUR) |
| **Other Linux** | `.AppImage` (streams paired hosts only, cannot capture its own screen) |

On **macOS and Linux**, the one‑liner installs the package and sets up the signed APT/DNF repository, so updates come with the system:

```sh
curl -fsSL https://moonlightweb.top/install.sh | bash
```

Step‑by‑step guides: [Windows](https://moonlightweb.top/guides/windows.html) · [macOS](https://moonlightweb.top/guides/macos.html) · [Linux](https://moonlightweb.top/guides/linux.html).

> **macOS:** the `.pkg` is not notarized. Install with the one‑liner or `brew install --cask linckosz/tap/moonlightweb` to avoid the Gatekeeper prompt, then allow **Screen Recording** when asked.

### Docker

For a NAS or mini PC that streams the hosts you pair with. A container has no screen of its own to capture.

```sh
docker run -d --name moonlightweb --network host \
  -v mw-data:/data --restart unless-stopped \
  ghcr.io/linckosz/moonlight-web:latest
docker exec moonlightweb moonlightweb --new-pin
```

Ports, volumes, Compose files and troubleshooting: [`docker/README.md`](docker/README.md).

## Getting started

1. Open **`https://localhost`** on the gaming PC, or `https://<PC-LAN-IP>` from another device, and accept the self‑signed certificate.
2. Your PC is already listed as **`<hostname> — MoonlightWeb Host`**, with one card per display.
3. Click a display to stream.

The in‑stream menu sets bitrate, resolution, frame rate, codec, HDR, 4:4:4 chroma and mouse mode. Other hosts on the LAN are discovered automatically and paired with the PIN they show.

## Remote access

Internet access is **off by default**. Once enabled from the admin page (`https://localhost/admin`), your PC keeps one outgoing connection to the rendezvous server and is reachable at `https://stream.moonlightweb.top/<id>`, still behind your access PIN.

The browser loads a small entry page from that server, then connects **directly to your PC** over WebRTC. Video, input and the whole API go over that connection, never through the server. UPnP opens a media port for each session and closes it at the end.

If UPnP is unavailable, forward UDP 48550‑48573 by hand. CGNAT is detected and reported.

## Architecture

```
 Browser (any device)                        Rendezvous server
 WebCodecs · WebGPU · AudioWorklet  ◄──────► entry page + signalling only
            │
            │  WebRTC, peer to peer, DTLS (WSS fallback)
            ▼
 Gaming PC — MoonlightWeb server (C++/Qt)
 ├─ Native engine: capture → GPU encode → WebRTC, zero copy
 └─ moonlight-common-c: Sunshine · Wolf · MultiSeat hosts you pair with
```

On an RTX 5060 Ti at 1440p, the native engine acquires a frame in 0.06 ms and encodes it in 3.5 ms. It uses one memory copy per frame, with no loopback hop, RTP or FEC. Details are in the [wiki](docs/wiki/02-Architecture.md).

## Configuration and privacy

Settings live in the UI and in `settings.json` (`%APPDATA%\MoonlightWeb\MoonlightWeb\` on Windows, `~/Library/Application Support/MoonlightWeb/MoonlightWeb/` on macOS, `~/.local/share/MoonlightWeb/MoonlightWeb/` on Linux). Every key is documented in the [settings reference](docs/wiki/07-Settings-Reference.md), including bringing your own domain and certificate.

Official builds send two anonymous counts:
- the version, OS and architecture with the update check;
- the shape of each session: resolution, codec, duration.

They never send an identifier, an address or the application you launched. **Settings → Privacy** lists exactly what is sent and switches it off. `MW_NO_TELEMETRY=1` disables both counts and the update check. Builds you compile yourself send nothing.

## Build from source

```bash
git clone --recursive https://github.com/linckosz/moonlight-web.git
cd moonlight-web
backend\build_msvc.bat      # Windows (MSVC + Qt)
./backend/build.sh          # Linux / macOS
```

Toolchain, Qt setup, tests and the PR workflow are in **[CONTRIBUTING.md](CONTRIBUTING.md)**. To set up a LAN‑only fork environment with Claude Code, see [`CLAUDE-LAN-DEV-SETUP.md`](CLAUDE-LAN-DEV-SETUP.md). The rendezvous server's DNS stack is in [`deploy/powerdns/`](deploy/powerdns/).

## About

Built by Bruno Martin, who also contributed **Video Super Resolution** to every major Moonlight client: [Qt](https://github.com/moonlight-stream/moonlight-qt/pull/1557), [Android](https://github.com/moonlight-stream/moonlight-android/pull/1567), [iOS/tvOS](https://github.com/moonlight-stream/moonlight-ios/pull/704) and [Xbox](https://github.com/TheElixZammuto/moonlight-xbox/pull/267).

If MoonlightWeb is useful to you, [a coffee](https://buymeacoffee.com/brunoocto) helps keep the servers running.

## License

[GPL‑3.0](LICENSE). Third‑party licenses are listed in [COPYRIGHT](COPYRIGHT).\
Copyright © 2026 Bruno Martin
