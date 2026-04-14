"""Cross-provider system-level validation for the Anolis System Composer."""


def _parse_i2c_address(value: object) -> int | None:
    """Parse I2C addresses from canonical hex or decimal string/int forms."""
    if isinstance(value, int):
        return value
    if not isinstance(value, str):
        return None
    text = value.strip()
    if text == "":
        return None
    try:
        return int(text, 0)
    except ValueError:
        pass
    try:
        return int(text, 16)
    except ValueError:
        return None


def validate_system(system: dict) -> list[str]:
    """
    Returns a list of error strings. Empty list means the system is valid.
    """
    errors: list[str] = []

    topology = system.get("topology", {})
    providers = topology.get("providers", {})
    runtime = topology.get("runtime", {})
    paths = system.get("paths", {})
    provider_paths = paths.get("providers", {})
    runtime_providers = runtime.get("providers", [])

    if not paths.get("runtime_executable"):
        errors.append("Runtime executable path is missing from paths.runtime_executable.")

    pids = [p.get("id") for p in runtime_providers if p.get("id")]
    if len(pids) != len(set(pids)):
        errors.append("Duplicate provider IDs in runtime config.")

    if runtime.get("http_port") == 3002:
        errors.append("Runtime HTTP port 3002 conflicts with the composer's own port.")

    owned: dict = {}
    for pid, pcfg in providers.items():
        if pcfg.get("kind") not in ("bread", "ezo"):
            continue
        bus_path = provider_paths.get(pid, {}).get("bus_path", "")
        for dev in pcfg.get("devices", []):
            addr_str = dev.get("address", "")
            addr = _parse_i2c_address(addr_str)
            if addr is None:
                continue
            key = (bus_path, addr)
            if key in owned:
                errors.append(
                    f"I2C address 0x{addr:02X} on bus '{bus_path}' is claimed by both '{owned[key]}' and '{pid}'."
                )
            else:
                owned[key] = pid

    for p in runtime_providers:
        pid = p.get("id")
        if pid and pid not in providers:
            errors.append(f"Provider '{pid}' is in the runtime list but has no config entry.")

    runtime_ids = {p.get("id") for p in runtime_providers if p.get("id")}
    for pid in providers:
        if pid not in runtime_ids:
            errors.append(f"Provider '{pid}' has a config entry but is not in the runtime list.")

    for pid, pcfg in providers.items():
        device_ids = [dev.get("id") for dev in pcfg.get("devices", []) if dev.get("id")]
        if len(device_ids) != len(set(device_ids)):
            errors.append(f"Provider '{pid}' has duplicate device IDs.")

    for p in runtime_providers:
        pid = p.get("id")
        if not pid:
            continue

        path_entry = provider_paths.get(pid)
        if path_entry is None:
            errors.append(f"Provider '{pid}' is in the runtime list but has no paths.providers entry.")
            continue

        if not path_entry.get("executable"):
            errors.append(f"Provider '{pid}' is missing paths.providers.{pid}.executable.")

        kind = p.get("kind") or providers.get(pid, {}).get("kind")
        if kind in ("bread", "ezo") and not path_entry.get("bus_path"):
            errors.append(f"Provider '{pid}' requires paths.providers.{pid}.bus_path.")

    for p in runtime_providers:
        pid = p.get("id", "<unknown>")
        restart_policy = p.get("restart_policy")
        if not isinstance(restart_policy, dict) or not restart_policy.get("enabled"):
            continue

        max_attempts = restart_policy.get("max_attempts")
        if not isinstance(max_attempts, int) or max_attempts < 1:
            errors.append(f"Provider '{pid}' restart_policy.max_attempts must be >= 1.")

        backoff_ms = restart_policy.get("backoff_ms")
        if not isinstance(backoff_ms, list) or not backoff_ms:
            errors.append(f"Provider '{pid}' restart_policy.backoff_ms must be a non-empty array.")
        else:
            if isinstance(max_attempts, int) and len(backoff_ms) != max_attempts:
                errors.append(
                    f"Provider '{pid}' restart_policy.backoff_ms length must match restart_policy.max_attempts."
                )
            if any(not isinstance(value, int) or value < 0 for value in backoff_ms):
                errors.append(f"Provider '{pid}' restart_policy.backoff_ms values must be integers >= 0.")

        timeout_ms = restart_policy.get("timeout_ms")
        if not isinstance(timeout_ms, int) or timeout_ms < 1000:
            errors.append(f"Provider '{pid}' restart_policy.timeout_ms must be >= 1000.")

        success_reset_ms = restart_policy.get("success_reset_ms")
        if success_reset_ms is not None and (not isinstance(success_reset_ms, int) or success_reset_ms < 0):
            errors.append(f"Provider '{pid}' restart_policy.success_reset_ms must be >= 0.")

    behavior_tree_path = runtime.get("behavior_tree_path") or runtime.get("behavior_tree")
    if runtime.get("automation_enabled") and not behavior_tree_path:
        errors.append("Automation is enabled but topology.runtime.behavior_tree_path is not set.")

    return errors
