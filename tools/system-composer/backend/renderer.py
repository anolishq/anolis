"""
renderer.py — system.json → YAML config renderer.

Pure-function module. No file I/O. No subprocess calls. No HTTP.
Takes a system.json dict, returns a dict of rendered YAML strings keyed by
relative output path (e.g. "anolis-runtime.yaml", "providers/sim0.yaml").
"""

import yaml


def render(system: dict, project_name: str) -> dict[str, str]:
    """
    Render a system.json dict into YAML config strings.

    Args:
        system:       Parsed system.json dict (schema_version 1).
        project_name: Name of the project directory under systems/.
                      Used to build the provider config file paths that the
                      runtime will pass to each provider via --config.

    Returns:
        {
            "anolis-runtime.yaml": "<yaml content>",
            "providers/<provider_id>.yaml": "<yaml content>",
            ...
        }
    """
    topology = system["topology"]
    paths = system["paths"]
    rt = topology["runtime"]

    outputs: dict[str, str] = {}

    # -------------------------------------------------------------------------
    # anolis-runtime.yaml
    # -------------------------------------------------------------------------
    runtime_doc: dict = {}

    # runtime section
    runtime_section: dict = {}
    if rt.get("name"):
        runtime_section["name"] = rt["name"]
    if "shutdown_timeout_ms" in rt:
        runtime_section["shutdown_timeout_ms"] = rt["shutdown_timeout_ms"]
    if "startup_timeout_ms" in rt:
        runtime_section["startup_timeout_ms"] = rt["startup_timeout_ms"]
    if runtime_section:
        runtime_doc["runtime"] = runtime_section

    # http section
    http_section: dict = {
        "enabled": True,
        "bind": rt.get("http_bind", "127.0.0.1"),
        "port": rt["http_port"],
    }
    if "cors_origins" in rt:
        http_section["cors_allowed_origins"] = rt["cors_origins"]
    runtime_doc["http"] = http_section

    # providers section
    provider_list = []
    for p in rt.get("providers", []):
        pid = p["id"]
        config_arg = (
            f"tools/system-composer/systems/{project_name}/providers/{pid}.yaml"
        )
        entry: dict = {
            "id": pid,
            "command": paths["providers"][pid]["executable"],
            "args": ["--config", config_arg],
            "timeout_ms": p.get("timeout_ms", 5000),
        }
        if "hello_timeout_ms" in p:
            entry["hello_timeout_ms"] = p["hello_timeout_ms"]
        if "ready_timeout_ms" in p:
            entry["ready_timeout_ms"] = p["ready_timeout_ms"]

        rp = p.get("restart_policy")
        if rp is not None:
            rp_out: dict = {"enabled": rp.get("enabled", False)}
            if rp.get("enabled"):
                rp_out["max_attempts"] = rp["max_attempts"]
                rp_out["backoff_ms"] = rp["backoff_ms"]
                rp_out["timeout_ms"] = rp["timeout_ms"]
                if "success_reset_ms" in rp:
                    rp_out["success_reset_ms"] = rp["success_reset_ms"]
            entry["restart_policy"] = rp_out

        provider_list.append(entry)
    runtime_doc["providers"] = provider_list

    # polling
    if "polling_interval_ms" in rt:
        runtime_doc["polling"] = {"interval_ms": rt["polling_interval_ms"]}

    # telemetry
    runtime_doc["telemetry"] = {"enabled": rt.get("telemetry_enabled", False)}

    # automation
    runtime_doc["automation"] = {"enabled": rt.get("automation_enabled", False)}

    # logging
    runtime_doc["logging"] = {"level": rt.get("log_level", "info")}

    outputs["anolis-runtime.yaml"] = yaml.dump(
        runtime_doc, default_flow_style=False, sort_keys=False
    )

    # -------------------------------------------------------------------------
    # Per-provider config files
    # -------------------------------------------------------------------------
    for pid, pdata in topology.get("providers", {}).items():
        kind = pdata["kind"]

        if kind == "sim":
            doc = _render_sim(pdata)
        elif kind == "bread":
            doc = _render_bread(pdata, paths["providers"][pid])
        elif kind == "ezo":
            doc = _render_ezo(pdata, paths["providers"][pid])
        elif kind == "custom":
            # Custom providers manage their own config files.
            continue
        else:
            continue

        outputs[f"providers/{pid}.yaml"] = yaml.dump(
            doc, default_flow_style=False, sort_keys=False
        )

    return outputs


# ---------------------------------------------------------------------------
# Provider-specific renderers
# ---------------------------------------------------------------------------

def _hex_to_int(addr) -> int:
    """Convert a hex string like '0x0A' or a bare integer to int."""
    if isinstance(addr, str):
        return int(addr, 16)
    return int(addr)


def _render_sim(pdata: dict) -> dict:
    doc: dict = {}

    if "provider_name" in pdata:
        doc["provider"] = {"name": pdata["provider_name"]}

    if "startup_policy" in pdata:
        doc["startup_policy"] = pdata["startup_policy"]

    devices = []
    for dev in pdata.get("devices", []):
        d: dict = {"id": dev["id"], "type": dev["type"]}
        if dev["type"] == "tempctl" and "initial_temp" in dev:
            d["initial_temp"] = dev["initial_temp"]
        elif dev["type"] == "motorctl" and "max_speed" in dev:
            d["max_speed"] = dev["max_speed"]
        devices.append(d)
    doc["devices"] = devices

    mode = pdata.get("simulation_mode", "non_interacting")
    sim_section: dict = {"mode": mode}
    if mode != "inert" and "tick_rate_hz" in pdata:
        sim_section["tick_rate_hz"] = pdata["tick_rate_hz"]
    doc["simulation"] = sim_section

    return doc


def _render_bread(pdata: dict, path_data: dict) -> dict:
    doc: dict = {}

    if "provider_name" in pdata:
        doc["provider"] = {"name": pdata["provider_name"]}

    hardware: dict = {"bus_path": path_data["bus_path"]}
    if "require_live_session" in pdata:
        hardware["require_live_session"] = pdata["require_live_session"]
    if "query_delay_us" in pdata:
        hardware["query_delay_us"] = pdata["query_delay_us"]
    if "timeout_ms" in pdata:
        hardware["timeout_ms"] = pdata["timeout_ms"]
    if "retry_count" in pdata:
        hardware["retry_count"] = pdata["retry_count"]
    doc["hardware"] = hardware

    addresses = [
        _hex_to_int(dev["address"])
        for dev in pdata.get("devices", [])
        if "address" in dev
    ]
    doc["discovery"] = {"mode": "manual", "addresses": addresses}

    devices = []
    for dev in pdata.get("devices", []):
        d: dict = {"id": dev["id"], "type": dev["type"]}
        if "label" in dev:
            d["label"] = dev["label"]
        if "address" in dev:
            d["address"] = _hex_to_int(dev["address"])
        devices.append(d)
    doc["devices"] = devices

    return doc


def _render_ezo(pdata: dict, path_data: dict) -> dict:
    doc: dict = {}

    if "provider_name" in pdata:
        doc["provider"] = {"name": pdata["provider_name"]}

    hardware: dict = {"bus_path": path_data["bus_path"]}
    if "query_delay_us" in pdata:
        hardware["query_delay_us"] = pdata["query_delay_us"]
    if "timeout_ms" in pdata:
        hardware["timeout_ms"] = pdata["timeout_ms"]
    if "retry_count" in pdata:
        hardware["retry_count"] = pdata["retry_count"]
    doc["hardware"] = hardware

    doc["discovery"] = {"mode": "manual"}

    devices = []
    for dev in pdata.get("devices", []):
        d: dict = {"id": dev["id"], "type": dev["type"]}
        if "label" in dev:
            d["label"] = dev["label"]
        if "address" in dev:
            d["address"] = _hex_to_int(dev["address"])
        devices.append(d)
    doc["devices"] = devices

    return doc
