[← Build, CI & Testing](11-Build-CI-Testing.md) · **Agentic Coding** · [Next: Roadmap & Constraints →](13-Roadmap-and-Constraints.md)

---

# 12. Agentic Coding — AI-assisted development on this repo

This chapter defines how AI coding agents (Claude Code, GitHub Copilot, or any LLM ingesting the repo) should be configured to work on MoonlightWeb productively **and safely**. The codebase has several "hard-won invariant" zones (streaming teardown ordering, transport chain, IDR discipline) where a plausible-looking change is often a regression — agent configuration must steer models toward the documented invariants before they touch code.

## 12.1 Ground rules for any agent

1. **Read this wiki first.** [Architecture](02-Architecture.md) → the chapter matching the touched area. The [Streaming workarounds table](05-Streaming-and-Transports.md#57-notable-workarounds-catalog) is a list of things that look like bugs but are fixes.
2. **Respect the invariants** (non-exhaustive): shim stops before relay destruction; new session start deferred on `destroyed()`; browser owns transport fallback; no nested `QEventLoop` in HTTP dispatch; no frontend reorder buffer; no JS audio gain; latency = sum of measured legs; WS URLs from `window.location.host`; never bind mDNS 5353 at boot.
3. **Match the local style**: C++17/Qt idioms, English comments (1–2 lines max, explaining constraints not mechanics); frontend vanilla ES6, no new dependencies, no build step.
4. **Gate before proposing**: `bash scripts/run-tests.sh` (Vitest + Qt Test) and `cd frontend && npm run check`; `clang-format==19.1.7` on `backend/src`. Conventional Commits.
5. **Never commit secrets** (`.env`, `MW_PDNS_TOKEN`, EAB creds) and never edit `deploy/powerdns/certs/`.

## 12.2 Recommended architecture: one master agent + on-demand skills + scoped sub-agents

The recommended model is a **single senior "master" agent, selected by default**, that:

- **plans first** — for any non-trivial task it produces a short plan (files, invariants at risk, test strategy) before editing;
- **executes itself** — it has full tool access (read/edit/build/test) and carries senior-level generalist skills (C++/Qt, WebRTC, vanilla JS, CMake, Docker);
- **pulls in skills on demand** — instead of front-loading context, it loads a domain skill (a markdown playbook) only when the task enters that domain;
- **delegates to sub-agents sparingly** — for parallelizable simple work (bulk searches, mechanical refactors across many files, doc sweeps) or for isolated expertise (reference codebases), never for the core reasoning of the task.

```
                    ┌────────────────────────────┐
   user request ──► │  MASTER AGENT (default)    │  plan → execute → verify
                    │  senior generalist         │
                    └──────┬───────────┬─────────┘
              loads when   │           │  spawns when parallel/simple
              relevant     ▼           ▼
                  ┌────────────┐   ┌──────────────────────────┐
                  │   SKILLS   │   │        SUB-AGENTS         │
                  │ build      │   │ explore (read-only scans) │
                  │ test-e2e   │   │ bulk-edit (mechanical)    │
                  │ sunshine-  │   │ moonlight-refs (expert on │
                  │   api      │   │   moonlight-qt/xbox/rust  │
                  │ transports │   │   reference codebases)    │
                  │ powerdns   │   └──────────────────────────┘
                  │ release    │
                  └────────────┘
```

### Suggested skills (playbooks the master loads on demand)

| Skill | Content | Trigger |
|---|---|---|
| `build` | How to configure/build per platform (MSVC/Ninja quirks, Qt paths), interpret common CMake/linker failures | any backend compile |
| `test-e2e` | Launch the server, probe `/api/health` and key endpoints, drive a smoke stream | verifying runtime behavior |
| `sunshine-api` | The two Sunshine APIs (GameStream NvHTTP/XML/pairing/RTSP vs modern REST :47990), ports, auth | anything touching `backend/src/backend/` |
| `transports` | The five modes, the fallback chain, framing/IDR/backpressure invariants (distills [ch. 5](05-Streaming-and-Transports.md)) | anything under `streaming/` or the stream JS |
| `powerdns` | Stack layout, pdnsutil commands, delegation debugging (distills [ch. 10](10-PowerDNS-Stack.md)) | `deploy/powerdns/` changes |
| `release` | Artifact naming, installer builds, symbolizing minidumps with cdb+PDB | packaging/CI changes |

### When the master should spawn a sub-agent

- **Parallelize simple, verifiable work**: "find every caller of X across backend+frontend", "apply this mechanical rename in 30 files", "summarize these 5 audit docs".
- **Isolated expertise**: questions about the *reference* Moonlight codebases (moonlight-qt, moonlight-xbox, web-stream) go to a read-only expert agent so their conventions don't bleed into this repo's diffs.
- **Never** for the central design decision of a task — a sub-agent starts cold and re-derives context; the master keeps the plan and the invariants.

## 12.3 Claude Code configuration

Concrete, repo-committable layout (paths are the Claude Code conventions):

```
CLAUDE.md                        # ≤ 1 page: pointers to docs/wiki, invariants list, gates
.claude/
├── agents/
│   ├── master.md                # default agent: plan-first + full tools (see frontmatter below)
│   ├── explore.md               # read-only search agent (Glob/Grep/Read only)
│   └── moonlight-refs.md        # reference-codebase expert (read-only, opt-in)
├── skills/
│   ├── build/SKILL.md           # + scripts the skill may call
│   ├── test-e2e/SKILL.md
│   ├── sunshine-api/SKILL.md
│   ├── transports/SKILL.md
│   ├── powerdns/SKILL.md
│   └── release/SKILL.md
└── settings.json                # permissions allowlist (cmake/ninja/npm/ctest…), hooks
```

Key choices:

- **`CLAUDE.md` stays thin** — a table of contents into `docs/wiki/` plus the invariant list; large context is pulled lazily via skills. (This wiki is formatted headings-first precisely so agents can ingest chapters selectively.)
- **`master.md`** frontmatter: default model at the strongest available tier, `tools: *`; body instructs: *plan before edit on multi-file tasks; run the gates; consult the transports skill before touching `streaming/`; prefer editing over adding dependencies*.
- **Sub-agents default to a cheaper model** (cost discipline); the reference-codebase expert is invoked **only with explicit user authorization**.
- **Hooks**: a pre-commit-style hook running `clang-format` and `npm run check` on stop, so formatting never reaches CI.

This repository's actual development already follows this model (build/test/sunshine-api skills, an opt-in `expert-moonlight-refs` sub-agent, sonnet-by-default sub-agents) — the layout above is the generalized, shareable version.

## 12.4 GitHub Copilot configuration

Copilot's repo-level steering uses different files:

```
.github/
├── copilot-instructions.md      # global: project map, invariants, style, gates
└── instructions/
    ├── backend.instructions.md      # applyTo: "backend/**/*.{cpp,h}"
    ├── frontend.instructions.md     # applyTo: "frontend/**/*.js"
    ├── streaming.instructions.md    # applyTo: "backend/src/streaming/**,frontend/js/stream/**,frontend/js/api/**"
    └── deploy.instructions.md       # applyTo: "deploy/**"
```

- **`copilot-instructions.md`** mirrors `CLAUDE.md`: one page — architecture summary, link to `docs/wiki/`, the invariants, the test gates, Conventional Commits.
- **Path-scoped `*.instructions.md`** files (with `applyTo` frontmatter) carry the per-domain rules that Claude gets via skills — e.g. the streaming file embeds the teardown-ordering and IDR rules verbatim, because Copilot's coding agent won't reliably fetch a wiki chapter on its own.
- For **Copilot coding agent** (autonomous PRs): add a `copilot-setup-steps.yml` workflow installing Qt/Node so its ephemeral environment can run the gates; restrict its firewall allowlist to package registries.
- Copilot has no master/sub-agent hierarchy — the equivalent discipline is: use **Copilot Chat "Plan" first** on multi-file tasks, and keep autonomous-agent assignments to issues that name the exact files and the acceptance gate.

## 12.5 Formatting this repo for AI ingestion (why the wiki looks like this)

- Chapters are **self-contained** with explicit cross-links — an agent (or a ChatGPT/Claude user pasting a single file) gets a complete picture of one domain without the rest.
- **Tables over prose** for enumerable contracts (settings keys, routes, transports, workarounds) — highest fact density per token.
- **Invariants are stated imperatively** ("never X", "X before Y") — the phrasing models retain best.
- File paths are always repo-relative and exact, so retrieval-augmented tools can jump from doc to code.

---

[← Build, CI & Testing](11-Build-CI-Testing.md) · [Home](Home.md) · [Next: Roadmap & Constraints →](13-Roadmap-and-Constraints.md)
