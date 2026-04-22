# Machine Profile Contract Fixtures

Fixture layout:

1. `valid/*.yaml`
   - Must pass machine-profile schema validation.
   - Must pass reference integrity validation.
   - Referenced runtime profile YAML must pass `runtime-config.schema.json`.
   - Runtime profile paths should follow discoverable naming tokens
     (`manual`, `telemetry`, `automation`, `full`).
   - `manifest.providers` IDs must align with `providers[].id` in each referenced runtime profile.
2. `invalid/schema/*.yaml`
   - Must fail machine-profile schema validation.
3. `invalid/references/*.yaml`
   - Must pass machine-profile schema validation.
   - Must fail reference integrity checks.

Machine-profile contract validation is performed by:

1. `tests/contracts/machine-profile/validate-machine-profiles.py`
