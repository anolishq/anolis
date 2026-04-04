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

- the full System Composer pytest suite
- focused C++ tests for runtime config parsing and ownership validation when a
  local CMake build directory is present

## What the Script Checks

### Composer contract coverage

```bash
python3 -m pytest tools/system-composer/tests -q
```

This covers renderer output, template parity, and validator behavior.

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
