#!/usr/bin/env python3
"""
Validate local markdown links in docs and root README.

Checks:
1. docs/**/*.md
2. README.md at repo root

Ignores:
1. External URLs (http/https/mailto/data)
2. In-page anchors (#...)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

LINK_RE = re.compile(r"\[[^\]]+\]\(([^)]+)\)")
SKIP_PREFIXES = ("http://", "https://", "mailto:", "data:", "#")


def _normalize_target(raw: str) -> str:
    target = raw.strip()
    if target.startswith("<") and target.endswith(">"):
        target = target[1:-1].strip()
    if (target.startswith('"') and target.endswith('"')) or (target.startswith("'") and target.endswith("'")):
        target = target[1:-1].strip()
    target = target.split("#", 1)[0].split("?", 1)[0].strip()
    return target


def _is_skipped(raw_target: str) -> bool:
    lowered = raw_target.lower()
    if not lowered:
        return True
    if lowered.startswith(SKIP_PREFIXES):
        return True
    if "://" in lowered:
        return True
    return False


def _resolve_link(base_file: Path, target: str, repo_root: Path) -> Path:
    if target.startswith("/"):
        return (repo_root / target.lstrip("/")).resolve()
    return (base_file.parent / target).resolve()


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    docs_root = repo_root / "docs"

    files = [repo_root / "README.md"]
    files.extend(sorted(docs_root.rglob("*.md")))

    missing: list[tuple[str, str]] = []

    for md_file in files:
        text = md_file.read_text(encoding="utf-8", errors="replace")
        for match in LINK_RE.finditer(text):
            raw_target = match.group(1).strip()
            if _is_skipped(raw_target):
                continue

            target = _normalize_target(raw_target)
            if not target or _is_skipped(target):
                continue

            resolved = _resolve_link(md_file, target, repo_root)
            if not resolved.exists():
                missing.append((str(md_file.relative_to(repo_root)), raw_target))

    if missing:
        print("Markdown link validation failed.")
        for file_path, target in missing:
            print(f"  - {file_path}: {target}")
        return 1

    print(f"Markdown link validation passed ({len(files)} files checked).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
