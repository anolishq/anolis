# Contract Baselines

Baselines in this directory freeze expected behavior at a point in time so contract changes are explicit.

## Baselines

1. [runtime-config-baseline.md](runtime-config-baseline.md) - Runtime YAML contract behavior.
2. [runtime-http-baseline.md](runtime-http-baseline.md) - Runtime `/v0` HTTP behavior.
3. [machine-profile-baseline.md](machine-profile-baseline.md) - Machine package and manifest behavior.
4. [telemetry-timeseries-baseline.md](telemetry-timeseries-baseline.md) - Telemetry `anolis_signal` row contract behavior.

## Canonical Machine Artifacts

1. Runtime config schema: `schemas/runtime/runtime-config.schema.json`
2. Runtime HTTP OpenAPI: `schemas/http/runtime-http.openapi.v0.yaml`
3. Machine profile schema: `schemas/machine/machine-profile.schema.json`
4. Telemetry timeseries schema: `schemas/telemetry/telemetry-timeseries.schema.v1.json`

Commissioning-specific contracts (System Composer + Workbench) are maintained in
[`anolis-workbench`](https://github.com/anolishq/anolis-workbench), including:

1. `contracts/composer-control.openapi.v1.yaml`
2. `contracts/validate-composer-control-openapi.py`
3. `docs/contracts/composer-control-baseline.md`
4. `docs/contracts/handoff-package-baseline.md`
5. `docs/contracts/handoff-package-v1.md`
6. `docs/commissioning-handoff-runbook.md`

## Change Rule

Treat baselines as compatibility snapshots:
update them only in the same change that intentionally modifies contract behavior, tests, and validator expectations.
