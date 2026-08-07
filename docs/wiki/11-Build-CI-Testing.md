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

Gated pipeline — quality and tests **block** the packaging stage. Runs on **`v*` tags**, on pull requests, and on manual dispatch — **not on branch pushes**: an ordinary push spends no CI minutes, so installers for an untagged commit are a deliberate act (Actions → CI → *Run workflow* on that branch). A tag is the one automatic push trigger, because this pipeline is what publishes the release (`release.yml` has no tag trigger of its own — see 11.3).

| Job | Runner | What |
|---|---|---|
| `frontend` | ubuntu | Prettier + ESLint (`npm run check`), then Vitest with **70% coverage gate scoped to pure-logic units** |
| `quality-cpp-format` | ubuntu | `clang-format` **19.1.7** check (house style in `backend/.clang-format`) |
| `quality-cppcheck` | ubuntu | cppcheck static analysis |
| `test-backend` | windows | Qt Test suites + OpenCppCoverage, 70% gate |
| `package` | (reusable) | Calls `release.yml`: builds Windows x64 & ARM64, Linux x64, macOS arm64 **and** packages them into the real installers, uploaded as run artifacts. Skipped on `pull_request` |
| `docker` | (reusable) | Calls `docker.yml` on a `v*` tag only: the multi-arch container image (11.3bis). Independent of `package` — a Debian container has nothing to wait on the desktop bundles for |

There is **no compile-only build stage**: packaging compiles the same four targets and additionally exercises `cmake --install`, `windeployqt`, the Inno Setup script, linuxdeploy and `macdeployqt` — the parts a bare build never touches, and where breakage actually happens. To test any commit, open its run and download the `MoonlightWeb-windows-<arch>-v<version>` / `moonlightweb-linux-x64-v<version>` / `moonlightweb-macos-arm64-v<version>` artifact (GitHub serves each as a zip). Untagged versions read `<last v* tag>-<3-char sha>`, e.g. `0.2.0-a71`.

`package` is skipped on `pull_request`: a fork's PR token is read-only, so the job's `contents: write` request would fail outright. Fork PRs still run quality + tests.

CI-specific workarounds baked into the workflows: `aqtinstall` pinned to a master commit (Qt 6.11 layout change) + `py7zr 1.1.0`; macOS uses `clang_64`; Windows builds with **Ninja** (VS 18 generator issues); `gh` path fixes. `build-asan.yml` provides an AddressSanitizer build on demand.

**Before pushing**: run `clang-format==19.1.7` (pip, via `py`) on `backend/src` locally — the exact same check the CI runs — and `bash scripts/run-tests.sh`.

## 11.3 Release (`.github/workflows/release.yml`)

Manual dispatch or a `workflow_call` from CI → per-platform packaging. It has **no tag trigger**: a `v*` tag fires `ci.yml`, which validates the commit and then calls this workflow, so a release is always gated on green quality/tests and the packaging matrix runs exactly once (two tag-triggered runs would race to upload the same Release assets).

- **setup** job computes the version (feeds `MW_VERSION`): the tag when the run's ref is a `v*` tag, otherwise `<last v* tag>-<3-char sha>`. It also exports `pkg_version`, the same string with `-` → `.`, because rpm rejects dashes in `Version` — the Linux job uses that one throughout.
- Publishing to the GitHub Release only happens on a `refs/tags/` ref; every other run stops at the workflow artifacts.
- **windows** (x64 + arm64): build → archive **PDB symbols** as an artifact → stage bundle → `windeployqt` + Qt OpenSSL TLS plugin + OpenSSL DLLs (vcpkg on arm64) → **SignPath** signs `MoonlightWeb.exe` → **Inno Setup** installer → SignPath signs the installer. Each signed file overwrites the unsigned one in place; the two `unsigned-*` artifacts exist only because SignPath reads its input from the artifact store (1-day retention). See [Installers §9.1](09-Installers-and-Packaging.md).
- **linux**: build → security TNR (`ctest`) → linuxdeploy **AppDir** (+ Wayland plugins best-effort) → **AppImage** → `make-packages.sh` → **.deb + .rpm** (AppStream metainfo included).
- **macos** (arm64, macos-15): build → security TNR (`ctest`) → assemble `.app` → `macdeployqt` + ad-hoc sign → **interactive `.pkg`** via `build-pkg.sh`.

Artifact naming is what `UpdateChecker` matches per-platform (`MoonlightWeb-installer-<v>-win-<arch>.exe`, `moonlightweb-<v>-linux-x64.{deb,rpm}`, AppImage, `moonlightweb-macos-arm64.pkg`).

### Publishing jobs (tag-only)

Four jobs run **after** the packaging matrix and only on a `refs/tags/` ref — each publishes to a service outside this repository, so it must never advertise a version with no release behind it. All of them consume the artifact the packaging job just uploaded rather than re-downloading the release asset, which may not be visible yet.

| Job | Needs | What it publishes |
|---|---|---|
| `homebrew` | `macos` | Renders `backend/packaging/homebrew/moonlightweb.rb` (version + sha256 of the built pkg) and pushes it to `linckosz/homebrew-tap` as `Casks/moonlightweb.rb`. |
| `linux-repo` | `linux` | Imports the GPG key, runs `make-repo.sh`, uploads the signed APT+DNF tree with `upload-pages-artifact`. |
| `linux-repo-deploy` | `linux-repo` | `deploy-pages`. A separate job because a job may declare only **one** environment, and `linux-repo` needs `MoonlightWeb` for the key while deployment needs `github-pages`. `concurrency: pages` with `cancel-in-progress: false` — a release must never land half-published. |
| `aur` | `linux` | Renders `PKGBUILD` + `.SRCINFO` from one substitution pass and pushes to `ssh://aur@aur.archlinux.org/moonlightweb-bin.git`; the commit is skipped when nothing changed, so re-running a tag build is idempotent. |

**A called workflow can never exceed the caller's token.** Every permission scope any job in `release.yml` requests — `contents` for the release assets, `pages` for `configure-pages`/`upload-pages-artifact`, `id-token` for the OIDC token `deploy-pages` exchanges — must also be granted by the `package` job in `ci.yml`. The check runs when the run starts, so a missing scope fails the whole pipeline with **`startup_failure`** before a single step executes, and with no per-job log to read.

**Neither a missing credential nor a rejected one fails a release.** Every publishing step is gated on a `steps.*.outputs.enabled` flag computed by a first step that emits a `::warning` when its secret is unset — the release ships, minus that channel. (The `secrets` context is unavailable in a job-level `if:`, hence the step-plus-output pattern rather than a job condition.) The steps that actually reach a third party — the two SignPath requests, the tap push, the AUR push — additionally carry `continue-on-error: true` and are followed by a step that annotates the run when `outcome` is not `success`. By the time any of them runs the assets are already on the Release, so a refused token leaves that channel stale rather than turning a good release red. The SignPath swaps are gated on the request's `outcome`, so a failed signature leaves the unsigned file in place instead of a half-signed bundle.

| Secret (environment `MoonlightWeb`) | Without it |
|---|---|
| `SIGNPATH_API_TOKEN`, `SIGNPATH_ORGANIZATION_ID` | Windows binaries ship **unsigned** (SmartScreen warns). |
| `HOMEBREW_TAP_TOKEN` (PAT with Contents:write on the tap — `GITHUB_TOKEN` is scoped to this repo) | The cask stays on the previous version. |
| `GPG_PRIVATE_KEY` (passphrase-less repository signing key) | No APT/DNF repository is built or deployed. |
| `AUR_SSH_KEY` | `moonlightweb-bin` stays on the previous version. |

`github-pages` is created by GitHub on the first deployment and defaults to *"deployments from the default branch only"* — `linux-repo-deploy` runs on a **tag** ref, so it needs a `v*` rule of type *tag* under Settings → Environments → `github-pages` → Deployment branches and tags (or *No restriction*). Symptom when it is missing: `Branch "refs/tags/vX.Y.Z" is not allowed to deploy to github-pages due to environment protection rules` — add the rule and re-run the job; the release assets are already published at that point.

The Windows crash minidumps (`crashes/*.dmp`) are symbolized with the archived PDBs + Qt PDBs via `cdb`.

## 11.3bis Container images (`.github/workflows/docker.yml`)

Publishes **two** GHCR packages as multi-arch manifests (`linux/amd64` + `linux/arm64`), and the split is the design:

| Package | Visibility | Written by | Tags |
|---|---|---|---|
| `moonlight-web` | public | a `v*` tag, through `ci.yml` | `latest`, `X.Y.Z`, `X.Y`, `sha-<commit>` |
| `moonlight-web-dev` | private — GHCR's default for a new package (measured: anonymous `tags/list` returns 403, against 200 for the public one) | `workflow_dispatch` with `publish` ticked | `dev`, `sha-<commit>` |

There is no `edge` and no nightly: an ordinary day of commits publishes nothing, and nobody following `latest` can land on work in progress. Testing a development build is a deliberate manual run, and it goes to the private package — unreachable without `docker login ghcr.io`, not merely unadvertised.

**One job per architecture on its own native runner** — `ubuntu-latest` and `ubuntu-24.04-arm` (free for public repositories) — each pushing an *untagged* blob with `push-by-digest=true`; a `merge` job then stitches the two digests into one manifest list with `docker buildx imagetools create` and applies every tag once. Tagging from the per-arch jobs would have the two architectures overwrite each other. QEMU is deliberately not used: emulating a full Qt + libdatachannel C++ build turns 20 minutes into hours and regularly hits the job timeout. The BuildKit cache is scoped per architecture (`scope=docker-amd64` / `-arm64`) — one shared scope has the two runners evict each other's layers on every run.

Triggers, and the reasoning behind each:

| Trigger | Effect |
|---|---|
| `workflow_call` | How a `v*` tag publishes: `ci.yml`'s `docker` job calls this after the quality/test gates, exactly as it calls `release.yml`. There is deliberately **no `push: tags`** trigger — that would build every tag twice and race the two runs to the same manifest. |
| `workflow_dispatch` | `publish` defaults to **false**: both architectures compile with `outputs: type=cacheonly` and nothing is pushed, which is the "does it still build?" check. Ticking it publishes to the private `-dev` package. There is deliberately no `push: branches` trigger, which also restores this repository's rule that an ordinary push spends no CI minutes. |
| `pull_request` (paths-filtered) | Build only, `outputs: type=cacheonly`, never pushed, and only when the image's own files change — so a Dockerfile edit is never merged unbuilt. Both architectures build, so an arm64-only break is caught. `backend/**` is deliberately absent from this filter: it would spend two full C++ builds on every backend PR, and the push trigger already covers a backend dependency that breaks the image. |
| `workflow_dispatch` | An image from any branch, on demand. |

GHCR authenticates with the run's own `GITHUB_TOKEN` — no PAT, nothing to rotate. As with `package`, every scope this workflow requests (`contents: read`, `packages: write`) has to be granted by the calling job in `ci.yml` or the run dies at startup. `secrets: inherit` matters here too — the `MW_*` DNS/ACME secrets reach the build as **build-stage `ARG`s**, and without them the published image would be LAN-only. The `MoonlightWeb` environment that holds them is requested conditionally (`${{ github.event_name != 'pull_request' && 'MoonlightWeb' || '' }}`): a fork's PR cannot read environment secrets anyway, and an environment with a required reviewer would leave the PR check pending on a build that is thrown away. `${GITHUB_REPOSITORY@L}` lower-cases the image path, which `ghcr.io` requires and a fork's owner may well not have.

The version string is computed exactly as `release.yml`'s `setup.ver` does — a `v*` tag ships `1.2.3`, anything else `<last tag>-<3-char sha>` — so `moonlightweb --version` inside a container names the commit the image came from. The image itself is documented in [Installers §9.3bis](09-Installers-and-Packaging.md) and [`docker/README.md`](../../docker/README.md).

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
