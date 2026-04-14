# Machine Profile Contract Fixtures

Fixture layout:

1. `valid/*.yaml`
   - Must pass machine-profile schema validation.
   - Must pass reference integrity validation.
   - Referenced runtime profile YAML must pass `runtime-config.schema.json`.
2. `invalid/schema/*.yaml`
   - Must fail machine-profile schema validation.
3. `invalid/references/*.yaml`
   - Must pass machine-profile schema validation.
   - Must fail reference integrity checks.

Machine-profile contract validation is performed by:

1. `tools/contracts/validate-machine-profiles.py`

The validator also checks tracked machine manifests under:

1. `config/**/machine-profile.yaml`
