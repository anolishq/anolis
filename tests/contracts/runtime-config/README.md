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
(numeric-like, bool-like, and text) to guard current loader scalar parsing behavior.
