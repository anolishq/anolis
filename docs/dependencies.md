# Dependency, Build, and CI Governance

Anolis uses:

- **vcpkg** (manifest mode) for C++ dependencies
- **uv** (`pyproject.toml` `dev` extra, locked by `uv.lock`, installed with `uv sync --locked --extra dev`) for Python tooling/tests

This document defines governance for dependency pinning, CI lanes, presets, and cross-repo compatibility.

## vcpkg Policy

1. **Single baseline source**: `vcpkg-configuration.json` is canonical.
2. **No `builtin-baseline`** in manifests.
3. **Lockfile pinning not planned**: the baseline SHA is the lock (org decision in anolishq/.github#93).
4. **Determinism source**: pinned baseline + reviewed `vcpkg.json` changes.

## Cross-Repo Pinning and Compatibility

Anolis tracks provider/runtime compatibility in `.ci/dependency-pins.yml`, the
refs the compatibility lanes consume.

Rules:

1. Compatibility lane must consume pinned refs, never floating `main`.
2. Pin changes require rationale and date.
3. The `provider_sim` ref must be a release tag. The lane downloads that
   release's `linux-x86_64` tarball and checks it against the release's
   `SHA256SUMS`; it does not build the provider.

## Version Numbering Policy (Cross-Repo)

- `anolis`, `anolis-provider-sim`, `fluxgraph`, and `anolis-protocol` version independently using **SemVer** (`MAJOR.MINOR.PATCH`).
- Public contract/build-surface changes require version-bump decision + changelog note.

## CI Lane Tiers

- **Required PR lanes**: the `ok` job in the CI workflow gates formatting, Python lint/type, UI/runtime contracts
  (runtime HTTP, machine profile, telemetry), shell tests, observability, docs, version-sync, clang-tidy on changed lines,
  the provider-compat lane (which runs the Python integration and scenario tests, excluding those marked stress or slow), and core build/test matrix lanes (Linux x64, Linux ARM64, Windows x64; strict and no-automation variants).
- **Advisory lanes**: optional non-blocking lanes may be added as needed.
- **Extended lanes** (outside the required `ok` gate): sanitizer (ASan/UBSan/TSan), coverage, valgrind and fuzz run on
  every PR to and push to `main` (path-filtered) through `extended.yml` and `hardening.yml`.
- **Periodic lanes**: weekly: the restart-churn soak (Sundays), dependency-scan and metrics (Mondays); metrics also runs
  on every push to `main`.

Promotion rule:

- Advisory lane can be promoted to required only after **10 consecutive green default-branch runs** and an explicit promotion PR.

## Rollout Policy

When replacing legacy build/test/CI paths:

1. Run legacy and new paths in parallel.
2. Minimum gate: **5 consecutive green runs**.
3. Preferred gate: **10 runs**.
4. Remove legacy path only after gate is met and approved.

## Preset Baseline and Exception Policy

Shared preset naming baseline:

- `dev-debug`, `dev-release`, `ci-linux-release`, `ci-windows-release`
- Specialized where supported: `ci-asan`, `ci-ubsan`, `ci-tsan`, `ci-coverage`
- Architecture extension: `ci-linux-arm64-release` (base) and `ci-linux-arm64-release-strict`, which the ARM64 CI and release lanes run.
- Repo-specific extensions: `*-strict` and `*-no-automation` variants (core build/test matrix), `ci-linux-compat`
  (provider compatibility lane), `ci-fuzz` (hardening fuzz lane).

Rules:

1. CI jobs should call named presets directly.
2. CI-only deviations must be explicit and documented.
3. Repo-specific extension presets are allowed if documented (for example feature-specific lanes).
4. Every preset must have an active owner/use-case (CI lane, script default, or documented workflow); remove unreferenced presets.

Contributor quick check:

```bash
cmake --list-presets
ctest --list-presets
```

On Windows local development, use `dev-windows-*` presets (MSVC toolchain).

## Pin Bump Process (Compatibility Lane)

When updating runtime/provider compatibility pins:

1. Update `.ci/dependency-pins.yml` (`provider_sim.ref`, `updated_utc`, `rationale`).
2. Run `anolis-provider-compat` lane against the new pin.
3. Merge only when required lanes and compatibility lane are green.

## Current C++ Dependencies

| Package              | Purpose                                 |
| :------------------- | :-------------------------------------- |
| **protobuf**         | Serialization format for ADPP protocol. |
| **yaml-cpp**         | Parsing runtime/provider YAML config.   |
| **cpp-httplib**      | Embedded HTTP server and HTTP client.   |
| **nlohmann-json**    | JSON handling.                          |
| **behaviortree-cpp** | Automation engine.                      |
| **gtest**            | Unit testing framework.                 |
| **openssl**          | TLS for cpp-httplib; SHA-256 hashing.   |

## Update Workflow (Summary)

1. Update dependency refs/policies in one PR.
2. Validate required lanes and compatibility lane.
3. Add a changelog note.
4. Merge only after lane tier and rollout rules are satisfied.
