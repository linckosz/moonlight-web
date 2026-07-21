[← PowerDNS Stack](10-PowerDNS-Stack.md) · **Build, CI & Testing** · [Next: Agentic Coding →](12-Agentic-Coding.md)

---

# 11. Build, CI & Testing

## 11.1 Building from source

CMake is the **single canonical build system** (qmake was removed 2026-06-28). Prerequisites and Qt-Creator setup are in `CONTRIBUTING.md`; the short version:

```bash
git clone https://github.com/linckosz/moonlight-web.git && cd moonlight-web
git submodule update --init --recursive     # moonlight-common-c, qmdnsengine, libdatachannel, miniupnp

# Windows (MSVC): detects VS 2022 + Qt, configures Ninja, builds Release
cmd //c backend/build_msvc.bat
# Linux / macOS: same, via CMake (Ninja if available)
./backend/build.sh
#   …or the raw call the scripts wrap:
#   cmake -S backend -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

build/MoonlightWeb          # Windows: build\MoonlightWeb.exe → open https://localhost
```

Both convenience scripts (`backend/build_msvc.bat`, `backend/build.sh`) auto-init the submodules on first run, locate Qt (override with `QTDIR` / `CMAKE_PREFIX_PATH`) and drop the binary in `build/` — the same layout CI uses.

Toolchain: CMake ≥ 3.21, Ninja, **Qt 6.11** (Core, Network, **WebSockets**), C++17, Node.js 22 (frontend tooling only). Windows: VS 2022 (MSVC v143); OpenSSL 3 is **vendored** in `backend/libs/windows/`. `-DCMAKE_PREFIX_PATH=<Qt kit>` if Qt isn't found. CMake emits `compile_commands.json` for clangd.

`backend/CMakeLists.txt` also:

- builds the native submodules statically via `add_subdirectory` (libdatachannel, miniupnpc, moonlight-common-c, qmdnsengine) — no manual per-dep step,
- bakes `MW_VERSION` (overridden by the release tag in CI) and the **embedded env defaults** (`MW_DOMAIN`, `MW_PDNS_*`, `MW_ZEROSSL_*` from CI secrets) as compile definitions,
- can embed a fallback cert (`MW_CERT_PEM`/`MW_CERT_KEY` read from `.env` at build time),
- installs the frontend next to the binary and generates the app icon resource (`app_icon.rc.in`).

## 11.2 CI (`.github/workflows/ci.yml`)

Gated pipeline — quality and tests **block** the build matrix:

| Job | Runner | What |
|---|---|---|
| `frontend` | ubuntu | Prettier + ESLint (`npm run check`), then Vitest with **70% coverage gate scoped to pure-logic units** |
| `quality-cpp-format` | ubuntu | `clang-format` **19.1.7** check (house style in `backend/.clang-format`) |
| `quality-cppcheck` | ubuntu | cppcheck static analysis |
| `test-backend` | windows | Qt Test suites + OpenCppCoverage, 70% gate |
| `build` matrix | windows/ubuntu/macos | CMake builds: Windows x64 & ARM64, Linux x64, macOS arm64 |

CI-specific workarounds baked into the workflows: `aqtinstall` pinned to a master commit (Qt 6.11 layout change) + `py7zr 1.1.0`; macOS uses `clang_64`; Windows builds with **Ninja** (VS 18 generator issues); `gh` path fixes. `build-asan.yml` provides an AddressSanitizer build on demand.

**Before pushing**: run `clang-format==19.1.7` (pip, via `py`) on `backend/src` locally — the exact same check the CI runs — and `bash scripts/run-tests.sh`.

## 11.3 Release (`.github/workflows/release.yml`)

Tag push (or manual dispatch) → per-platform packaging:

- **setup** job computes the version from the tag (feeds `MW_VERSION`).
- **windows** (x64 + arm64): build → archive **PDB symbols** as an artifact → stage bundle → `windeployqt` + Qt OpenSSL TLS plugin + OpenSSL DLLs (vcpkg on arm64) → **Inno Setup** installer.
- **linux**: build → linuxdeploy **AppDir** (+ Wayland plugins best-effort) → **AppImage** → `make-packages.sh` → **.deb + .rpm**.
- **macos** (arm64, macos-15): build → assemble `.app` → `macdeployqt` + ad-hoc sign → **interactive `.pkg`** via `build-pkg.sh`.

Artifact naming is what `UpdateChecker` matches per-platform (`MoonlightWeb-installer-<v>-win-<arch>.exe`, `moonlightweb-<v>-linux-x64.{deb,rpm}`, AppImage, `moonlightweb-macos-arm64.pkg`).

The Windows crash minidumps (`crashes/*.dmp`) are symbolized with the archived PDBs + Qt PDBs via `cdb`.

## 11.4 Testing

Two-layer non-regression gate, both runnable locally via **`bash scripts/run-tests.sh`**:

### Frontend — Vitest (`frontend/test/`, jsdom)

Unit tests for the pure-logic modules: `Mp4Muxer` (NAL/avcC/hvcC), `Av1Utils`, `SdpUtils`, `JitterController`, `GamepadManager`, `BackendClient`, `VersionGuard`, `BrowserDetect`, `createRenderer`, `Toast`, `escapeHtml`, `i18n`, models. Coverage gate: 70% on those units (UI/DOM-heavy code is exempt by design).

### Backend — Qt Test (`backend/tests/`)

A lightweight in-repo framework (`test_framework.h`) + suites: `AppSettings`, `AuthManager`, `ConnectionGuard`, `HttpParser`, `InputCrypto`, `InputEncoder`, `RestRouter`, `StaticFiles`, `StreamConfig`, `UPNPClient` (with a dedicated `UPNP_TEST_PLAN.md` and `run_upnp_tests.bat`). `security_main.cpp` groups the security-sensitive suites. Coverage via OpenCppCoverage (`run_coverage.bat`, `check_coverage.ps1`) with the same 70%-on-pure-logic philosophy. `ConnectionGuard` takes injectable timestamps for deterministic tests.

### What is intentionally *not* unit-tested

The streaming pipeline (relays, shim, WebRTC) is validated end-to-end — `docs/testing.md` describes the approach, and `scripts/run-tests.sh` is the PR gate. Manual E2E remains the reference for transport changes (multiple browsers/devices/networks).

## 11.5 Code quality conventions

- C++17 / Qt style per `backend/.clang-format` + `.clang-tidy` (`backend/scripts/run_clang_tidy.sh`); comments always in English, concise.
- Frontend: Prettier config in `frontend/.prettierrc.json`, ESLint flat config, `tsconfig.json` with `checkJs` (advisory).
- Conventional Commits; PRs against `main`, focused on one domain; screenshots for UI changes.

---

[← PowerDNS Stack](10-PowerDNS-Stack.md) · [Home](Home.md) · [Next: Agentic Coding →](12-Agentic-Coding.md)
