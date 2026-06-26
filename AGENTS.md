# AGENTS.md — anolis

> Per-repo conventions for coding agents (Claude Code, OpenCode, …). The
> canonical cross-repo rules — Conventional Commits, minimal-first/YAGNI, no
> secrets, run checks before asserting success — live in the user's **global**
> `AGENTS.md` and are not repeated here. This file records only what is
> **specific to this repo**: the commands, the gate, and the non-obvious things
> agents get wrong here.

C++20 control runtime for the Anolis system.

## Build / test

- Configure + build: `cmake --preset ci-linux-release` then
  `cmake --build --preset ci-linux-release`.
- Test: `ctest --preset ci-linux-release` (or `cd` into the preset's build dir
  and run `ctest`).
- The required CI status check is the **`ok`** job (it aggregates the lanes,
  `.github/workflows/ci.yml`); never bypass it, and never merge red.

## Tooling

- **C++ repos:** clang-format / clang-tidy are pinned to **18.1.8** via the
  shared `setup-clang-tools` action (matches workstation-configs) — do NOT use
  pip/apt/pre-commit/container versions. Run `clang-format -i` before **every**
  commit (CI fails otherwise). vcpkg comes from the shared `setup-vcpkg` action.
- Shared `.github` actions/workflows are SHA-pinned with a `# <tag>` comment so
  Renovate can track them — keep that comment when bumping.

## Repo-specific gotchas

- **C++20**, and use **`std::format`** for diagnostics/log messages (not
  iostreams or printf-style formatting).
- **Test-only clang-tidy relaxations** live in `tests/.clang-tidy` — it disables
  gtest test-noise such as `bugprone-unchecked-optional-access` (false positives
  against gtest macros). Don't "fix" test code to satisfy a check that the test
  config already suppresses.
- There is a **whole-repo clang-tidy job** in `extended.yml` (preset
  `ci-linux-clang-tidy`) separate from the per-PR `ok` gate — a change can pass
  `ok` and still trip extended tidy.
- **HTTP API** lives under `core/http/` (cpp-httplib) with **Bearer auth** and a
  loopback bind gate (`core/http/auth.hpp`, `core/http/server.cpp`). Don't widen
  the bind or weaken auth without intent.
- **BehaviorTree automation** uses behaviortree-cpp; automation can be disabled
  via the `*-no-automation` presets.

## Backlog

Backlog lives in GitHub issues, not a `TODO.md`.
