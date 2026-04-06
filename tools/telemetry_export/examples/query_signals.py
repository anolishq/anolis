#!/usr/bin/env python3
"""Minimal programmatic client example for telemetry export MVP service."""

from __future__ import annotations

import argparse
import json
import sys

import requests


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Query Anolis telemetry export service")
    parser.add_argument("--base-url", default="http://127.0.0.1:8091", help="Export service base URL")
    parser.add_argument("--token", default="export-dev-token", help="Bearer token")
    parser.add_argument("--start", required=True, help="RFC3339 UTC start timestamp")
    parser.add_argument("--end", required=True, help="RFC3339 UTC end timestamp")
    parser.add_argument("--format", choices=["json", "csv"], default="json")
    parser.add_argument("--provider", action="append", default=[], help="provider_id filter (repeatable)")
    parser.add_argument("--device", action="append", default=[], help="device_id filter (repeatable)")
    parser.add_argument("--signal", action="append", default=[], help="signal_id filter (repeatable)")
    parser.add_argument("--requester", default="example-client", help="Requester ID for audit metadata")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    body: dict[str, object] = {
        "time_range": {
            "start": args.start,
            "end": args.end,
        },
        "resolution": {
            "mode": "raw_event",
        },
        "format": args.format,
    }

    selector: dict[str, list[str]] = {}
    if args.provider:
        selector["provider_ids"] = args.provider
    if args.device:
        selector["device_ids"] = args.device
    if args.signal:
        selector["signal_ids"] = args.signal
    if selector:
        body["selector"] = selector

    response = requests.post(
        f"{args.base_url.rstrip('/')}/v1/exports/signals:query",
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {args.token}",
            "X-Requester-Id": args.requester,
        },
        json=body,
        timeout=20,
    )

    request_id = response.headers.get("X-Request-Id", "")
    if request_id:
        print(f"request_id={request_id}", file=sys.stderr)

    if response.status_code != 200:
        print(response.text)
        return 1

    content_type = response.headers.get("Content-Type", "")
    if content_type.startswith("application/json"):
        payload = response.json()
        print(json.dumps(payload, indent=2))
        return 0

    if content_type.startswith("text/csv"):
        manifest = response.headers.get("X-Export-Manifest", "")
        if manifest:
            print("manifest=" + manifest, file=sys.stderr)
        print(response.text)
        return 0

    print(response.text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
