# Testing & non-regression (TNR)

Moonlight-Web ships a two-layer automated test gate used to validate pull
requests. It protects the project's hand-written logic against regressions
**without** freezing the architecture: tests assert *observable behavior*
(inputs → outputs, emitted bytes, sent messages, persisted JSON), never private
internals, so refactors stay free.

| Layer | Framework | Coverage tool | Gate |
|---|---|---|---|
| Frontend (Vanilla JS, ES modules) | [Vitest](https://vitest.dev) (jsdom) | `@vitest/coverage-v8` | ≥ 70% lines/functions/statements on in-scope modules |
| Backend (C++17 / Qt) | Qt + a tiny assertion runner | [OpenCppCoverage](https://github.com/OpenCppCoverage/OpenCppCoverage) | ≥ 70% lines on in-scope units |

## Run everything

```bash
bash scripts/run-tests.sh      # frontend + backend, aggregated pass/fail
```

## Frontend only

```bash
cd frontend
npm install
npm test            # run the suite
npm run coverage    # run + enforce the 70% gate, writes coverage/ (HTML report)
```

## Backend only (Windows + MSVC + Qt)

```bash
cd backend/tests
./run_coverage.bat  # qmake6 + nmake, runs run_tests.exe under OpenCppCoverage,
                    # then check_coverage.ps1 enforces the 70% gate
```

One-time prerequisite for the coverage **percentage** (tests still run without it):

```bash
winget install OpenCppCoverage.OpenCppCoverage
```

The HTML report lands in `backend/tests/build/covhtml/index.html`; the machine
report is `backend/tests/build/cov.xml` (Cobertura).

## What is in scope (and why)

The 70% gate is measured **only** over deterministic, pure-logic units. The
heavy I/O glue — WebRTC/libdatachannel relays, sockets, the Qt event loop, the
DOM, WebGPU/WebCodecs, audio worklets — is deliberately **out of scope**:
covering it would require large, brittle mocks that couple tests to the
implementation and make the code rigid, the opposite of a TNR safety net.

**Frontend in-scope** (`frontend/vitest.config.js` → `coverage.include`):
`util/Av1Utils`, `util/Mp4Muxer`, `util/BrowserDetect`, `util/VersionGuard`,
`stream/JitterController`, `stream/GamepadManager`, `stream/renderers/createRenderer`,
`models/App`, `models/Host`, `i18n/i18n`, `api/BackendClient`, `ui/Toast`.

**Backend in-scope** (`backend/tests/check_coverage.ps1` → `$Files`):
`streaming/InputEncoder`, `streaming/StreamConfig`, `streaming/InputCrypto`,
`server/RestRouter`, `server/AppSettings`, `server/AuthManager`.

## Adding tests when you open a PR

- **New pure-logic module?** Add it to the in-scope list (Vitest `coverage.include`
  or the backend `tests.pro` + `check_coverage.ps1 $Files`) and ship a matching
  test. Keep assertions on behavior, not internals.
- **Changing existing logic?** Update/extend the relevant test so it still
  describes the intended behavior. A red test means either a real regression or
  an intended behavior change — in the latter case, update the test in the same PR.
- **I/O / UI glue?** No unit test is expected; rely on the E2E `test-stream`
  flow and manual verification.

CI runs both layers on every pull request (`.github/workflows/tests.yml`).
