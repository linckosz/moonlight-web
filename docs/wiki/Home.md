# MoonlightWeb — Technical Wiki

> **Stream your PC games from any browser.** MoonlightWeb is a 100% web [Sunshine](https://github.com/LizardByte/Sunshine)/GameStream client: a C++/Qt server that speaks the GameStream protocol to Sunshine and relays video/audio/input to any modern browser over WebRTC. Nothing to install on the client — just a URL.

This wiki is the canonical technical documentation for contributors and for AI coding agents. It only covers what is committed to this repository. Every chapter is self-contained, cross-linked, and written so that both a human developer preparing a pull request and an LLM ingesting the repo can build an accurate mental model of the system.

## How to read this wiki

- **New contributor?** Read [Overview](01-Overview.md) → [Architecture](02-Architecture.md) → the chapter that matches the area you want to change, then [Build, CI & Testing](11-Build-CI-Testing.md) before opening a PR.
- **AI agent?** Start with [Architecture](02-Architecture.md) (component map + data flow), then [Settings Reference](07-Settings-Reference.md) and [REST API](08-REST-API.md) (the machine-readable contracts). The [Agentic Coding](12-Agentic-Coding.md) chapter describes the recommended agent configuration for this repo.
- **Deploying the DNS stack?** Go straight to [PowerDNS Stack](10-PowerDNS-Stack.md).

## Table of contents

| # | Chapter | What it covers |
|---|---------|----------------|
| 1 | [Overview](01-Overview.md) | The product from the User and Administrator point of view, with screenshots |
| 2 | [Architecture](02-Architecture.md) | System diagram, tech stack & rationale, repository layout, browser ↔ MoonlightWeb ↔ Sunshine exchange |
| 3 | [Backend](03-Backend.md) | C++/Qt server: modules, code architecture, HTTP/HTTPS server, session lifecycle |
| 4 | [Frontend](04-Frontend.md) | Vanilla JS app: views/overlays, renderers, audio pipeline, input, i18n |
| 5 | [Streaming & Transports](05-Streaming-and-Transports.md) | Transport fallback chain, video/audio/input mechanics, canvas vs `<video>`, HDR limitations, workarounds |
| 6 | [Security](06-Security.md) | Auth (PIN/certificate/host-key), sessions, rate limiting, TLS/ACME, DNS ownership, consent audit |
| 7 | [Settings Reference](07-Settings-Reference.md) | Every `settings.json` key, `.env` variables, build-time embedded secrets |
| 8 | [REST API](08-REST-API.md) | Every HTTP endpoint and WebSocket surface exposed by the server |
| 9 | [Installers & Packaging](09-Installers-and-Packaging.md) | Windows (Inno Setup), macOS (.pkg), Linux (.deb/.rpm/AppImage), services, auto-update |
| 10 | [PowerDNS Stack](10-PowerDNS-Stack.md) | Self-hosted DNS infrastructure: Docker stack, installer, network, manual steps (cloud/registrar) |
| 11 | [Build, CI & Testing](11-Build-CI-Testing.md) | CMake build, GitHub Actions pipelines, test suites and coverage gates |
| 12 | [Agentic Coding](12-Agentic-Coding.md) | Recommended AI-agent setup (Claude Code, GitHub Copilot): master agent, skills, sub-agents |
| 13 | [Roadmap & Constraints](13-Roadmap-and-Constraints.md) | Known remaining work, current constraints, improvement leads |
| 14 | [Conclusion](14-Conclusion.md) | Design philosophy summary and pointers |

## Project at a glance

| | |
|---|---|
| **License** | GPL-3.0 (© 2026 Bruno Martin) |
| **Server platforms** | Windows x64/ARM64, Linux x64, macOS arm64 |
| **Client platforms** | Any modern browser (Chrome, Edge, Safari — desktop & mobile) |
| **Backend** | C++17 / Qt 6.11, `moonlight-common-c`, `libdatachannel`, `miniupnpc`, `qmdnsengine`, OpenSSL 3 |
| **Frontend** | Vanilla JS (ES6 modules, no build step), WebCodecs, WebGPU, AudioWorklet, WebRTC |
| **Build system** | CMake (single canonical build system; qmake removed) |
| **Repository root** | `backend/` (server), `frontend/` (web app), `deploy/powerdns/` (DNS stack), `website/` (landing page), `.github/` (CI), `scripts/`, `docs/` |

## Conventions used throughout

- File references are repo-relative, e.g. `backend/src/streaming/Session.cpp`.
- "Host" = the Sunshine gaming PC. "Server" = the MoonlightWeb process. "Client" = the browser.
- Code comments in the repo are always English; commit messages follow [Conventional Commits](https://www.conventionalcommits.org/).
