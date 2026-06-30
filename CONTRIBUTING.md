# Contributing to Moonlight‑Web

Thanks for wanting to contribute! This guide covers everything you need to build
the project from source, run the tests, and open a pull request.

Cross‑platform build via **CMake** — the single, canonical build system (qmake
removed). CMake also generates `compile_commands.json` for clangd / IDEs.

---

## 1. Install the toolchain

**All platforms**

| Tool | Why | Link |
|---|---|---|
| **Git** | clone + submodules | <https://git-scm.com/downloads> |
| **CMake ≥ 3.21** | build system | <https://cmake.org/download/> |
| **Ninja** | build generator | <https://ninja-build.org/> (bundled with VS / Qt) |
| **Qt 6.11** | Core, Network, **WebSockets** | <https://www.qt.io/download-qt-installer-oss> |
| **Node.js 22** | frontend lint / format / tests | <https://nodejs.org/> |

**Windows (additional)**

| Tool | Notes | Link |
|---|---|---|
| **Visual Studio 2022** (Community) | install the **“Desktop development with C++”** workload (MSVC v143, Windows SDK, CMake & Ninja components) | <https://visualstudio.microsoft.com/downloads/> |
| **OpenSSL 3.x** | already **bundled** in `backend/libs/windows/` — nothing to install | — |
| **OpenCppCoverage** (optional) | backend test coverage % | <https://github.com/OpenCppCoverage/OpenCppCoverage/releases> |

In the **Qt online installer**, select **Qt 6.11.0 → MSVC 2022 64‑bit** (or your
platform's kit) and the **Qt WebSockets** module. Default install path expected by
the build script: `C:\Qt\6.11.0\msvc2022_64` (override with the `QTDIR` env var).

**Linux** — `sudo apt install build-essential cmake ninja-build pkg-config libssl-dev`, plus Qt 6.11 (online installer or `aqtinstall` with the `qtwebsockets` module).
**macOS** — `brew install cmake ninja openssl@3 pkg-config`, plus Qt 6.11 (`clang_64`).

---

## 2. Clone & build

```bash
git clone <this-repo>
cd moonlight-web-deepseek
git submodule update --init --recursive   # moonlight-common-c, qmdnsengine, libdatachannel...

# Windows (MSVC) — detects VS 2022 + Qt, configures Ninja, builds Release:
cmd //c backend/build_msvc.bat
# Linux / macOS (CMake):
cmake -S backend -B backend/build -DCMAKE_BUILD_TYPE=Release && cmake --build backend/build -j

cd backend/build/release && ./MoonlightWeb   # Windows: MoonlightWeb.exe → open https://localhost
```

> If CMake can't find Qt, pass `-DCMAKE_PREFIX_PATH=<your Qt kit path>` (e.g. `C:/Qt/6.11.0/msvc2022_64`).

---

## 3. Build in Qt Creator (optional)

1. **File → Open File or Project…** and select **`backend/CMakeLists.txt`**.
2. In the **Configure Project** screen, tick the **Desktop Qt 6.11.0 MSVC2022 64‑bit** kit (untick others).
3. Set the build configuration to **Release** (or **RelWithDebInfo** to debug).
4. If Qt isn't auto‑detected: **Tools → Options → Kits**, point the kit's Qt version to your install.
5. Pick the **`MoonlightWeb`** run target, then **Build (Ctrl+B)** / **Run (Ctrl+R)**.

---

## 4. Frontend tooling & tests

The frontend is **Vanilla JS — no build step**, but it has lint/format/test tooling:

```bash
cd frontend
npm install
npm run check    # prettier + eslint (PR gate)
npm test         # Vitest unit tests
```

Run the **full non‑regression gate** (frontend Vitest + backend Qt Test/coverage)
before a PR:

```bash
bash scripts/run-tests.sh
```

---

## 5. Open a pull request

- **Branch** from `main`, keep changes focused on one domain (backend **or** frontend **or** config).
- **Commit messages** follow **[Conventional Commits](https://www.conventionalcommits.org/)** — e.g. `feat(stream): …`, `fix(webrtc): …`, `build:`, `ci:`, `docs:` (see the git history for the style).
- Make sure `bash scripts/run-tests.sh` and `npm run check` pass.
- Open the PR against **`main`** with a short description and, for UI changes, a screenshot.

---

## Code style

- Clean, simple, robust — avoid over‑engineering.
- **Comments in code: always in English**, concise (1–2 lines max).
- Backend: C++17 / Qt 6.11. Frontend: Vanilla JS (ES6 modules), no framework.

---

## DNS stack (Internet access)

To offer auto sub‑domain + TLS you need an authoritative DNS server on a domain you
own. [`deploy/powerdns/`](deploy/powerdns/) ships a turnkey Docker stack (dnsdist +
PowerDNS + Caddy). Install on a small Linux VM with `sudo ./install.sh`, open ports
53 (UDP/TCP), 80 and 443, register your nameservers at your registrar, then set
`MW_DOMAIN` / `MW_PDNS_URL` / `MW_PDNS_TOKEN` in the server's `.env`. See
[`deploy/powerdns/README.md`](deploy/powerdns/README.md).
