# Runtime Config Contract Fixtures

Fixture layout:

1. `valid/*.yaml`
   - Must pass schema validation.
   - Must pass `anolis-runtime --check-config`.
2. `invalid/schema/*.yaml`
   - Must fail schema validation.
3. `invalid/runtime/*.yaml`
   - Must pass schema validation.
   - Must fail `anolis-runtime --check-config`.

Use these fixtures to lock compatibility behavior while the runtime-config contract evolves.

Edge-case coverage includes quoted scalar values in automation hook args
(numeric-like, bool-like, and text). The loader keeps a quoted hook argument as
a string whatever its text (#637); these fixtures check that such configs load.

Runtime parser semantics (`yaml-cpp`) remain authoritative; fixture outcomes must
be verified through `anolis-runtime --check-config` as part of the contract gate.

Parser-alignment guardrails:

1. Duplicate mapping keys are contract-invalid and must fail schema-layer checks.
2. Runtime-invalid fixtures must remain schema-valid and fail only at runtime semantic validation.
