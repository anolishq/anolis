# Anolis Documentation

Welcome to the Anolis documentation! Use this file as the canonical documentation index.

## Start Here

1. [quickstart-simulator.md](quickstart-simulator.md) - Run the released runtime with simulated devices and control one over HTTP.
2. [getting-started.md](getting-started.md) - Build, run, and first validation.
3. [architecture.md](architecture.md) - Runtime architecture and subsystem boundaries.
4. [decoupling.md](decoupling.md) - The ecosystem model and the invariant every change is tested against.
5. [system-surfaces.md](system-surfaces.md) - Boundary & ownership map of every external surface (org-wide).
6. [configuration.md](configuration.md) - How to author and validate runtime YAML.
7. [http-api.md](http-api.md) - Human guide for runtime `/v0` HTTP usage.

## Deployment

1. [deploy/README.md](deploy/README.md) - One provisioning engine, and the routes to it.
2. [deploy/install-sh.md](deploy/install-sh.md) - Canonical headless deploy, online or offline.
3. [deploy/workbench-ssh.md](deploy/workbench-ssh.md) - Provision a remote machine over SSH.
4. [deploy/developer.md](deploy/developer.md) - Running a locally built runtime on a rig.

## Contracts and Schemas

1. [schemas/README.md](https://github.com/anolishq/anolis/blob/main/schemas/README.md) - Machine-readable contract artifacts.
2. [contracts/README.md](contracts/README.md) - Contract baselines and change policy.
3. [http/README.md](http/README.md) - Runtime HTTP contract validation workflow.

## Runtime Reference

1. [cli.md](cli.md) - `anolis-runtime` options, environment variables and exit codes.
2. [configuration-schema.md](configuration-schema.md) - Compact narrative map of schema sections.
3. [observability.md](observability.md) - Telemetry to InfluxDB and the Grafana dashboards.
4. [automation.md](automation.md) - Automation architecture, mode semantics, and operation.
5. [providers.md](providers.md) - Provider model and runtime interaction rules.
6. [safety.md](safety.md) - Safety boundaries and operational expectations.
7. [rfcs/provider-declared-safe-state.md](rfcs/provider-declared-safe-state.md) - Accepted-with-revisions design for provider-owned safe state.
8. [rfcs/stop-chain.md](rfcs/stop-chain.md) - Increments 1 to 5 of 6 implemented (1 to 4 in v0.1.53 and v0.1.55, 5 in anolis-workbench; the `/v0/estop` rename is not): observing the emergency stop, and the reset that follows it.

## Contributor Reference

1. [local-verification.md](local-verification.md) - Fast local verification workflow.
2. [dependencies.md](dependencies.md) - Dependency and toolchain expectations.
3. [cpp-documentation-standard.md](cpp-documentation-standard.md) - C++ doc style rules.
4. [CONTRIBUTING.md](https://github.com/anolishq/anolis/blob/main/CONTRIBUTING.md) - Contributor workflow and repo policy.

## Commissioning Tooling

System Composer and Workbench planning/contracts are maintained in
[`anolis-workbench`](https://github.com/anolishq/anolis-workbench).

Local C++ API docs: run `doxygen docs/Doxyfile` from repo root.
