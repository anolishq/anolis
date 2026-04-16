# Local Verification

Use this workflow before lab sessions, branch handoff, or config-contract work.
It is intentionally narrower than full CI and focused on the checks most likely
to catch local drift quickly.

## Fast Path

From the repo root:

```bash
bash tools/verify-local.sh
```

This runs:

- runtime config contract validation (schema + `anolis-runtime --check-config`) when a local runtime binary is present
- machine profile contract validation
- docs local-link validation (`docs/**/*.md` + root `README.md`)
- runtime HTTP OpenAPI structural validation
- runtime HTTP example payload validation
- composer control OpenAPI structural validation
- runtime HTTP live conformance smoke validation when both local runtime and provider-sim binaries are present
- the full System Composer pytest suite
- the Workbench shell pytest suite
- Operator UI fixture contract tests when `node` is available
- focused C++ tests for runtime config parsing and ownership validation when a
  local CMake build directory is present

## What the Script Checks

### Composer contract coverage

```bash
python3 -m pytest tools/system-composer/tests -q
python3 -m pytest tools/workbench/tests -q
```

This covers renderer output, template parity, and validator behavior.
It also covers the Composer control API contract baseline (`/api/status`,
preflight/launch/stop/restart/logs behavior) and Workbench shell route support.

### Operator UI fixture contract coverage

If `node` is present, the script also runs:

```bash
node --test tools/operator-ui/tests/contracts.test.mjs
node --test tools/system-composer/tests/unit/launch_url_resolution.test.mjs
```

If `node` is not available, this step is skipped with an explicit message.

### Runtime config contract coverage

If a local runtime binary is present (`build/dev-release/core/anolis-runtime` or
`build/dev-windows-release/core/Release/anolis-runtime.exe`), the script runs:

```bash
python3 tools/contracts/validate-runtime-configs.py --runtime-bin <local-runtime-binary>
```

This enforces the runtime config contract across:

1. tracked runtime YAML profiles
2. contract fixture sets (`valid`, `invalid/schema`, `invalid/runtime`)

### Machine profile contract coverage

The script always runs:

```bash
python3 tools/contracts/validate-machine-profiles.py
```

This validates:

1. machine profile schema conformance
2. referenced file existence
3. referenced runtime profile compatibility checks

### Docs local-link coverage

The script always runs:

```bash
python3 tools/contracts/validate-doc-links.py
```

This validates:

1. local markdown links under `docs/`
2. local markdown links in root `README.md`

### Runtime HTTP contract coverage (structural + examples)

The script always runs:

```bash
python3 tools/contracts/validate-runtime-http-openapi.py
python3 tools/contracts/validate-runtime-http-examples.py
```

This checks:

1. OpenAPI document shape and metadata
2. Required `/v0` endpoint/method coverage
3. Internal `$ref` resolution
4. SSE media type contract on `/v0/events`
5. Example payload schema conformance from `tests/contracts/runtime-http/examples/manifest.yaml`

### Composer control contract coverage (structural)

The script also runs:

```bash
python3 tools/contracts/validate-composer-control-openapi.py
```

This checks:

1. OpenAPI document shape and metadata
2. Required control endpoint/method coverage
3. Internal `$ref` resolution
4. SSE media type contract on `/api/projects/{name}/logs`

### Runtime HTTP conformance smoke coverage (live fixture)

If both a runtime binary and provider-sim binary are present, the script also runs:

```bash
python3 tools/contracts/validate-runtime-http-conformance.py \
  --runtime-bin <local-runtime-binary> \
  --provider-bin <local-provider-sim-binary>
```

This starts a runtime fixture and validates live endpoint responses against the
OpenAPI schema declared for each observed status code.

The conformance run also includes deterministic negative checks for:

1. `400` (`POST /v0/call` invalid payload)
2. `404` (missing device capabilities path)
3. `503` (automation endpoints with automation disabled)

### Focused runtime coverage

If a build directory exists, the script also runs:

```bash
ctest --test-dir build/dev-release --output-on-failure -R "ConfigTest|RuntimeOwnershipValidationTest"
```

or, on Windows builds:

```bash
ctest --test-dir build/dev-windows-release --output-on-failure -R "ConfigTest|RuntimeOwnershipValidationTest"
```

These tests cover runtime YAML parsing, restart-policy validation, automation
config handling, and I2C ownership invariants.

## When to Run Full CI-Style Coverage Instead

Use the broader build and test flows when:

- changing core runtime execution behavior beyond config/restart hardening
- touching integration or scenario suites
- modifying platform build logic or dependencies

See [../CONTRIBUTING.md](../CONTRIBUTING.md) for the full build and test flow.
