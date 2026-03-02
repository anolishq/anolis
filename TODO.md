# Anolis Runtime - TODO

## CI / Quality

- [ ] Setup precommit hooks for relevant tooling
- [ ] Add fuzzing targets for ADPP/protocol and runtime config parsing surfaces.

## Security / Production Hardening

### Runtime / App Security

- [ ] Implement HTTP/API authentication and authorization before non-localhost exposure.
- [ ] Define and enforce telemetry redaction/classification + secret-safe logging policy.

### Supply Chain Security

- [ ] Add dependency/CVE automation (Dependabot + security advisory checks).

### Compiler / Binary Hardening

- [ ] Add hardened "Release-Hardened" preset (separate from sanitizer builds).
- [ ] Enable stack protection for all non-sanitized, production-targeted builds (`-fstack-protector-strong`/`/GS`).
- [ ] Enable `_FORTIFY_SOURCE=2` (or 3 where supported) for compile-time buffer overflow checks on supported platforms for release builds.
- [ ] Enable PIE for executables to ensure full ASLR on supported platforms (`-fPIE` + `-pie` linker flag).
- [ ] Enable RELRO + immediate binding on ELF platforms (`-Wl,-z,relro,-z,now`).
- [ ] Tighten security-focused static analysis in CI (clang-tidy is already enabled; expand security-specific checks and enforce policy).
- [ ] Add CI lane to verify hardened build flags are present in production builds (check build flags + `checksec` on Linux).

## Performance / Reliability

- [ ] Run valgrind leak analysis on Linux for runtime lifecycle paths.
  - Command: `valgrind --leak-check=full --show-leak-kinds=all ./build/core/Release/anolis-runtime --config test.yaml`
  - Focus: event emitter queues, provider supervision, HTTP server threads
- [ ] Add benchmark baselines and regression tracking for core runtime paths.
- [ ] Run long-duration soak/stress tests (>24h), including rapid provider restart cycles.
- [ ] Conditional: move to async provider polling only if provider-count/latency metrics demand it.
- [ ] Conditional: run lock-contention profiling and tune identified hotspots.

## Operator UI (Demand-Driven)

- [ ] Event filtering/search in event trace.
- [ ] Session export + replay support.
- [ ] Device state snapshots for validation/reporting.
- [ ] Runtime-configurable polling interval.

## Protocol / Hardware Evolution

- [ ] Re-evaluate ADPP Configure extension only if decentralized provider config proves insufficient.
- [ ] Expand hardware mock/fault-injection strategy for broader real-hardware validation.

## Ecosystem / Provider SDKs (Late Future)

- [ ] Design and ship provider SDKs (C/C++/Rust) only after:
  - at least 3 real hardware providers are implemented and validated
  - runtime/app interfaces and ADPP/provider integration seams have stabilized
  - SDK versioning/compatibility policy is defined (including deprecation strategy)
