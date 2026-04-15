# Dependency, Build, and CI Governance

Anolis uses:

- **vcpkg** (manifest mode) for C++ dependencies
- **pip** (`requirements*.txt`) for Python tooling/tests

This document defines governance for dependency pinning, CI lanes, presets, and cross-repo compatibility.

## vcpkg Policy

1. **Single baseline source**: `vcpkg-configuration.json` is canonical.
2. **No `builtin-baseline`** in manifests.
3. **Lockfile pinning deferred** for now.
4. **Determinism source**: pinned baseline + reviewed `vcpkg.json` changes.

## Cross-Repo Pinning and Compatibility

Anolis tracks provider/runtime compatibility with two control files:

- `.ci/dependency-pins.yml`: refs consumed by compatibility lanes
- `.ci/compatibility-matrix.yml`: tested runtime/provider/protocol/fluxgraph combinations

Rules:

1. Pin updates and matrix updates must be in the same reviewed PR.
2. Compatibility lane must consume pinned refs, never floating `main`.
3. Pin changes require rationale and date.

## Version Numbering Policy (Cross-Repo)

- `anolis`, `anolis-provider-sim`, `fluxgraph`, and `anolis-protocol` version independently using **SemVer** (`MAJOR.MINOR.PATCH`).
- Public contract/build-surface changes require version-bump decision + changelog note.
- Compatibility matrix records tested cross-repo version/ref combinations.

## CI Lane Tiers

- **Required PR lanes**: CI workflow gates currently include formatting, Python lint/type/tests, UI/composer contracts,
  and core build/test matrix lanes (Linux x64, Linux ARM64, Windows x64; strict and no-automation variants).
- **Advisory lanes**: optional non-blocking lanes may be added as needed.
- **Nightly/periodic lanes**: heavy coverage/sanitizer/stress lanes.

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
- Architecture extension: `ci-linux-arm64-release` (ARM64 runtime portability lane).

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
2. Update matching entry in `.ci/compatibility-matrix.yml`.
3. Run `anolis-provider-compat` lane against the new pin.
4. Merge only when required lanes and compatibility lane are green.

## Current C++ Dependencies

| Package              | Purpose                                 |
| :------------------- | :-------------------------------------- |
| **protobuf**         | Serialization format for ADPP protocol. |
| **yaml-cpp**         | Parsing runtime/provider YAML config.   |
| **cpp-httplib**      | Embedded HTTP server.                   |
| **nlohmann-json**    | JSON handling.                          |
| **behaviortree-cpp** | Automation engine.                      |
| **gtest**            | Unit testing framework.                 |

## Update Workflow (Summary)

1. Update dependency refs/policies in one PR.
2. Validate required lanes and compatibility lane.
3. Update compatibility matrix + changelog note.
4. Merge only after lane tier and rollout rules are satisfied.
