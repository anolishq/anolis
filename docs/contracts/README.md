# Contract Baselines

Baselines in this directory freeze expected behavior at a point in time so contract changes are explicit.

## Baselines

1. [runtime-config-baseline.md](runtime-config-baseline.md) - Runtime YAML contract behavior.
2. [runtime-http-baseline.md](runtime-http-baseline.md) - Runtime `/v0` HTTP behavior.
3. [machine-profile-baseline.md](machine-profile-baseline.md) - Machine package and manifest behavior.
4. [composer-control-baseline.md](composer-control-baseline.md) - System Composer control API behavior.

## Canonical Machine Artifacts

1. Runtime config schema: `schemas/runtime-config.schema.json`
2. Runtime HTTP OpenAPI: `schemas/http/runtime-http.openapi.v0.yaml`
3. Machine profile schema: `schemas/machine-profile.schema.json`
4. Composer control OpenAPI: `schemas/tools/composer-control.openapi.v1.yaml`

## Change Rule

Treat baselines as compatibility snapshots:
update them only in the same change that intentionally modifies contract behavior, tests, and validator expectations.
