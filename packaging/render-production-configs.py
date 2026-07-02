#!/usr/bin/env python3
"""Render source configs into production configs with absolute paths.

Used by the bundle-build (packaging layer, anolishq/anolis#140) to produce
deployment-ready configs that install.sh copies verbatim into /opt/anolis/.

The deployment descriptors live in a separate repo (anolishq/anolis-projects);
pass its checkout with --projects-root so this script is not coupled to being
co-located with the profiles.

Usage:
    python packaging/render-production-configs.py <profile> <output-dir> \\
        --projects-root <anolis-projects-checkout> [--prefix /opt/anolis]
"""

import argparse
import re
import shutil
import sys
from pathlib import Path

# Patterns to replace (dev-relative → production-absolute)
PROVIDER_CMD_RE = re.compile(r"\.\./anolis-provider-([a-z0-9-]+)/build/[^/]+/anolis-provider-([a-z0-9-]+)")
PROJECT_PATH_RE = re.compile(r"\.\./anolis-projects/projects/([a-z0-9-]+)/(config|behaviors)/([^\s\"']+)")


def render_config(content: str, profile: str, prefix: str) -> str:
    """Apply production path transformations to a config file."""

    # Provider command: ../anolis-provider-bread/build/dev-release/anolis-provider-bread
    #                 → /opt/anolis/bin/anolis-provider-bread
    content = PROVIDER_CMD_RE.sub(lambda m: f"{prefix}/bin/anolis-provider-{m.group(2)}", content)

    # Project paths: ../anolis-projects/projects/bioreactor-v1/config/foo.yaml
    #              → /opt/anolis/projects/bioreactor-v1/config/foo.yaml
    content = PROJECT_PATH_RE.sub(lambda m: f"{prefix}/projects/{m.group(1)}/{m.group(2)}/{m.group(3)}", content)

    # Bind address: 127.0.0.1 → 0.0.0.0 (allow LAN access in production)
    content = re.sub(
        r"^(\s*bind:\s*)127\.0\.0\.1\s*$",
        r"\g<1>0.0.0.0",
        content,
        flags=re.MULTILINE,
    )

    return content


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", help="Profile name (e.g. bioreactor-v1)")
    parser.add_argument("output_dir", help="Output directory for rendered configs")
    parser.add_argument(
        "--projects-root",
        default=".",
        help="Checkout of anolishq/anolis-projects (contains projects/<profile>/)",
    )
    parser.add_argument("--prefix", default="/opt/anolis", help="Install prefix (default: /opt/anolis)")
    args = parser.parse_args()

    projects_root = Path(args.projects_root).resolve()
    profile_dir = projects_root / "projects" / args.profile
    if not profile_dir.is_dir():
        print(f"ERROR: Profile directory not found: {profile_dir}", file=sys.stderr)
        return 1

    config_dir = profile_dir / "config"
    if not config_dir.is_dir():
        print(f"ERROR: Config directory not found: {config_dir}", file=sys.stderr)
        return 1

    out = Path(args.output_dir)
    out_config = out / "config"
    out_providers = out / "config" / "providers"
    out_project_config = out / "projects" / args.profile / "config"
    out_config.mkdir(parents=True, exist_ok=True)
    out_providers.mkdir(parents=True, exist_ok=True)
    out_project_config.mkdir(parents=True, exist_ok=True)

    # Copy behaviors as-is (XML, no path transformation needed)
    behaviors_dir = profile_dir / "behaviors"
    if behaviors_dir.is_dir():
        out_behaviors = out / "projects" / args.profile / "behaviors"
        out_behaviors.mkdir(parents=True, exist_ok=True)
        for f in behaviors_dir.iterdir():
            if f.is_file():
                shutil.copy2(f, out_behaviors / f.name)

    # Render runtime configs
    runtime_configs = sorted(config_dir.glob("anolis-runtime.*.yaml"))
    if not runtime_configs:
        print(f"ERROR: No runtime configs found in {config_dir}", file=sys.stderr)
        return 1

    for rc in runtime_configs:
        rendered = render_config(rc.read_text(), args.profile, args.prefix)
        # All profile configs go into projects/{profile}/config/
        (out_project_config / rc.name).write_text(rendered)

    # The "manual" profile is the default active config
    manual_candidates = [rc for rc in runtime_configs if ".manual." in rc.name]
    if manual_candidates:
        rendered = render_config(manual_candidates[0].read_text(), args.profile, args.prefix)
        (out_config / "runtime.yaml").write_text(rendered)
    else:
        # Fall back to first runtime config
        rendered = render_config(runtime_configs[0].read_text(), args.profile, args.prefix)
        (out_config / "runtime.yaml").write_text(rendered)

    # Render provider configs
    provider_configs = sorted(config_dir.glob("provider-*.yaml"))
    for pc in provider_configs:
        rendered = render_config(pc.read_text(), args.profile, args.prefix)
        # provider-bread.bioreactor.yaml → bread.yaml
        # Extract provider name: strip "provider-" prefix, take first segment before "."
        name = pc.stem.removeprefix("provider-").split(".")[0]
        (out_providers / f"{name}.yaml").write_text(rendered)
        # Also keep full version in project config dir
        (out_project_config / pc.name).write_text(rendered)

    # Copy machine-profile.yaml into project output
    mp = profile_dir / "machine-profile.yaml"
    if mp.exists():
        out_project = out / "projects" / args.profile
        shutil.copy2(mp, out_project / "machine-profile.yaml")

    print(f"Rendered production configs for '{args.profile}' → {out}")
    print(f"  Runtime configs: {len(runtime_configs)}")
    print(f"  Provider configs: {len(provider_configs)}")
    print("  Default active config: config/runtime.yaml (manual profile)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
