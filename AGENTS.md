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
  shared `setup-clang-tools` action (install the same pinned binary locally) — do NOT use
  pip/apt/pre-commit/container versions. Run `clang-format -i` before **every**
  commit (CI fails otherwise). vcpkg comes from the shared `setup-vcpkg` action.
- Shared `.github` actions/workflows are SHA-pinned with a `# <tag>` comment so
  Renovate can track them — keep that comment when bumping.

## Repo-specific gotchas

- **C++20**. In `core/`, log with the **`LOG_*` stream macros**
  (`LOG_WARN("..." << x)`); build non-log strings with **`std::format`**. Use
  `std::cout`/`std::cerr` only in the logger's sink (`core/logging/logger.cpp`)
  and in `core/src/main.cpp`'s command-line output; no printf-style
  formatting. Tests are outside the rule: a stub provider writes its framed
  stdio to `std::cout`, and a test redirects `std::cerr` to capture the log.
- **Test-only clang-tidy relaxations** live in `tests/.clang-tidy` — it disables
  gtest test-noise such as `bugprone-unchecked-optional-access` (false positives
  against gtest macros). Don't "fix" test code to satisfy a check that the test
  config already suppresses.
- **clang-tidy in CI is diff-based**: the `clang-tidy` job in `ci.yml` calls
  the shared `clang-tidy-diff.yml` workflow (preset `ci-linux-release`,
  blocking) and lints only the lines a PR changes. It is part of the `ok` gate
  and skips when no C++ file or `.clang-tidy` changed. No CI job runs
  clang-tidy over the whole repo; `just lint` does that locally.
- **HTTP API** lives under `core/http/` (cpp-httplib) with **Bearer auth** and a
  loopback bind gate (`core/http/auth.hpp`, `core/http/server.cpp`). Don't widen
  the bind or weaken auth without intent.
- **BehaviorTree automation** uses behaviortree-cpp; automation can be disabled
  via the `*-no-automation` presets.

## Backlog

Backlog lives in GitHub issues, not a `TODO.md`.
