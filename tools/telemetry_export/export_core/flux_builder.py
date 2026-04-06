"""Flux query construction utilities."""

from __future__ import annotations

from .models import AUX_FIELDS_LAST_ONLY, NON_NUMERIC_VALUE_FIELDS, NUMERIC_VALUE_FIELDS, SignalsQuery


def flux_quote(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def build_selector_filter(field_name: str, values: list[str]) -> str | None:
    if not values:
        return None
    clauses = [f'r.{field_name} == "{flux_quote(v)}"' for v in values]
    return " or ".join(clauses)


def build_field_filter(field_names: tuple[str, ...]) -> str:
    clauses = [f'r._field == "{flux_quote(field)}"' for field in field_names]
    return " or ".join(clauses)


def build_base_flux_lines(request: SignalsQuery, bucket: str) -> list[str]:
    start_iso = request.start.isoformat().replace("+00:00", "Z")
    end_iso = request.end.isoformat().replace("+00:00", "Z")

    lines = [
        f'from(bucket:"{flux_quote(bucket)}")',
        f'  |> range(start: time(v: "{start_iso}"), stop: time(v: "{end_iso}"))',
        '  |> filter(fn:(r) => r._measurement == "anolis_signal")',
    ]

    for field_name, values in (
        ("runtime_name", request.runtime_names),
        ("provider_id", request.provider_ids),
        ("device_id", request.device_ids),
        ("signal_id", request.signal_ids),
    ):
        expr = build_selector_filter(field_name, values)
        if expr:
            lines.append(f"  |> filter(fn:(r) => {expr})")

    return lines


def build_base_flux_pipeline(request: SignalsQuery, bucket: str) -> str:
    return "\n".join(build_base_flux_lines(request, bucket))


def build_downsample_query(request: SignalsQuery, bucket: str) -> str:
    base = build_base_flux_pipeline(request, bucket)
    numeric_filter = build_field_filter(NUMERIC_VALUE_FIELDS)
    non_numeric_filter = build_field_filter(NON_NUMERIC_VALUE_FIELDS + AUX_FIELDS_LAST_ONLY)
    numeric_agg = request.resolution.aggregation

    return "\n".join(
        [
            "numeric = (",
            base,
            f"  |> filter(fn:(r) => {numeric_filter})",
            (
                "  |> aggregateWindow("
                f"every: {request.resolution.interval}, "
                f"fn: {numeric_agg}, "
                "createEmpty: false)"
            ),
            ")",
            "",
            "non_numeric = (",
            base,
            f"  |> filter(fn:(r) => {non_numeric_filter})",
            (
                "  |> aggregateWindow("
                f"every: {request.resolution.interval}, "
                "fn: last, "
                "createEmpty: false)"
            ),
            ")",
            "",
            "union(tables:[numeric, non_numeric])",
            (
                '  |> pivot(rowKey:["_time","runtime_name","provider_id","device_id","signal_id"], '
                'columnKey:["_field"], valueColumn:"_value")'
            ),
            (
                '  |> keep(columns:["_time","runtime_name","provider_id","device_id","signal_id","quality",'
                '"value_double","value_int","value_uint","value_bool","value_string"])'
            ),
            '  |> sort(columns:["_time","runtime_name","provider_id","device_id","signal_id"])',
        ]
    )


def build_raw_or_project_query(request: SignalsQuery, bucket: str) -> str:
    lines = build_base_flux_lines(request, bucket)

    lines.extend(
        [
            (
                '  |> pivot(rowKey:["_time","runtime_name","provider_id","device_id","signal_id"], '
                'columnKey:["_field"], valueColumn:"_value")'
            ),
            (
                '  |> keep(columns:["_time","runtime_name","provider_id","device_id","signal_id","quality",'
                '"value_double","value_int","value_uint","value_bool","value_string"])'
            ),
            '  |> sort(columns:["_time","runtime_name","provider_id","device_id","signal_id"])',
        ]
    )

    return "\n".join(lines)


def build_flux_query(request: SignalsQuery, bucket: str) -> str:
    if request.resolution.mode == "downsampled":
        return build_downsample_query(request, bucket)
    return build_raw_or_project_query(request, bucket)
