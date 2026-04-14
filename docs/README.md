# Anolis Documentation

Minimal living documentation for Anolis Core.

## Contents

- [concept.md](concept.md) - What Anolis is and why it exists
- [architecture.md](architecture.md) - System design and component relationships
- [cpp-documentation-standard.md](cpp-documentation-standard.md) - C++ API documentation and comment conventions
- [getting-started.md](getting-started.md) - Build and run instructions
- [configuration.md](configuration.md) - Configuration guide (`config/anolis-runtime.yaml` and composer-generated runtime YAML)
- [configuration-schema.md](configuration-schema.md) - Human-readable runtime config reference
- [../schemas/README.md](../schemas/README.md) - Machine-validated runtime config contract artifacts
- [contracts/runtime-config-baseline.md](contracts/runtime-config-baseline.md) - Baseline behavior snapshot for runtime config contract
- [providers.md](providers.md) - Provider protocol and development
- [http-api.md](http-api.md) - HTTP REST API reference
- [automation.md](automation.md) - Behavior trees and automation system
- [local-verification.md](local-verification.md) - Focused local verification workflow
- [../CONTRIBUTING.md](../CONTRIBUTING.md) - Development guide and common pitfalls

Local C++ API docs: run `doxygen docs/Doxyfile` from the repo root.
Generated output goes to `build/docs/doxygen/html/` and remains untracked.

Keep docs short and to-the-point. Update as system evolves.
