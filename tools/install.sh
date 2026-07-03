#!/usr/bin/env bash
# shellcheck disable=SC2034  # unused vars are future flags (telemetry, observability)
# Anolis Runtime — Standalone Provisioning Script
#
# Config-driven: a deployment is a config (a machine-profile + configs + behaviors).
# install.sh is the one generic engine — it builds a bundle from a config (from the
# component versions the machine-profile pins) and installs it. Providers are read
# from the config; nothing is hardcoded.
#
# Usage:
#   Online (host has internet):  sudo ./install.sh --project ./my-config
#   Offline — build on a dev box, install on the device:
#     ./install.sh --stage ./out --project ./my-config --arch arm64        # dev box
#     sudo ./install.sh --local ./out/anolis-<profile>-<ver>-arm64.tar.gz  # device
#
# See --help for all options.

set -euo pipefail

# =============================================================================
# Constants
# =============================================================================

readonly DEFAULT_PREFIX="/opt/anolis"
readonly DEFAULT_PORT=8080
readonly HEALTH_TIMEOUT=30
readonly HEALTH_INTERVAL=2
readonly ANOLIS_USER="anolis"
readonly ANOLIS_GROUPS="i2c,gpio,dialout"
readonly SYSTEMD_DIR="/etc/systemd/system"

# =============================================================================
# Globals (set by argument parsing)
# =============================================================================

LOCAL_PATH=""
WITH_TELEMETRY=0
WITH_OBSERVABILITY=0
NO_START=0
UNINSTALL=0
DRY_RUN=0
PREFIX="${DEFAULT_PREFIX}"
REBOOT_NEEDED=0

# Stage mode (build an offline bundle from a local config; dev-side, no root)
STAGE_DIR=""
PROJECT_DIR=""
TARGET_ARCH=""
STAGE_TMP=""
ASSEMBLED_PROFILE=""
ASSEMBLED_RUNTIME_VERSION=""

# =============================================================================
# Output helpers
# =============================================================================

_green()  { printf '\033[0;32m%s\033[0m\n' "$*"; }
_yellow() { printf '\033[0;33m%s\033[0m\n' "$*"; }
_red()    { printf '\033[0;31m%s\033[0m\n' "$*"; }

log_ok()   { _green  "✓ $*"; }
log_skip() { _yellow "→ $*"; }
log_warn() { _yellow "⚠ $*"; }
log_err()  { _red    "✗ $*"; }
log_info() { printf  "  %s\n" "$*"; }

die() { log_err "$*"; exit 1; }

# =============================================================================
# Argument parsing
# =============================================================================

show_help() {
    cat <<'EOF'
Usage: install.sh [OPTIONS]

Config-driven provisioning: a deployment is a config (machine-profile.yaml +
config/ + behaviors/). install.sh builds a bundle from that config — at the
component versions the machine-profile pins — and installs it.

Install:
  --project <dir>        Install online from a config: assemble + install
                         (needs internet + python3/pyyaml on this machine)
  --local <path>         Install a pre-built bundle tarball or directory (offline)

Build only (no install; run on a dev box, no root):
  --stage <out-dir>      Build an offline bundle into <out-dir> from --project
  --arch <arm64|x86_64>  Target architecture for --stage (default: host)

Common:
  --with-telemetry       Also install telemetry-export service
  --with-observability   Also install observability stack (Docker Compose)
  --no-start             Install but don't start services
  --uninstall            Remove anolis installation
  --dry-run              Print what would happen without doing it
  --prefix <path>        Override install prefix (default: /opt/anolis)
  --help, -h             Show this help

Environment:
  GITHUB_TOKEN           GitHub token (optional, avoids release-download rate limits)

Examples:
  # Online install from a config (host has internet):
  sudo ./install.sh --project ./my-bioreactor

  # Offline — build a bundle on a dev box, then install it on the device:
  ./install.sh --stage ./out --project ./my-bioreactor --arch arm64
  sudo ./install.sh --local ./out/anolis-my-bioreactor-<ver>-arm64.tar.gz
EOF
    exit 0
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --local)     LOCAL_PATH="${2:-}"; shift 2 ;;
            --with-telemetry)    WITH_TELEMETRY=1; shift ;;
            --with-observability) WITH_OBSERVABILITY=1; shift ;;
            --no-start)  NO_START=1; shift ;;
            --uninstall) UNINSTALL=1; shift ;;
            --dry-run)   DRY_RUN=1; shift ;;
            --prefix)    PREFIX="${2:-}"; shift 2 ;;
            --stage)     STAGE_DIR="${2:-}"; shift 2 ;;
            --project)   PROJECT_DIR="${2:-}"; shift 2 ;;
            --arch)      TARGET_ARCH="${2:-}"; shift 2 ;;
            --help|-h)   show_help ;;
            *)           die "Unknown option: $1 (use --help for usage)" ;;
        esac
    done
}

# =============================================================================
# Phase 1: Root check
# =============================================================================

phase_root_check() {
    if [[ $EUID -ne 0 ]]; then
        die "This script must be run as root. Use: sudo $0"
    fi
    log_ok "root check"
}

# =============================================================================
# Phase 2: Detect architecture
# =============================================================================

phase_detect() {
    ARCH=$(uname -m)
    case "${ARCH}" in
        aarch64|arm64) ARCH="arm64" ;;
        x86_64)        ARCH="x86_64" ;;
        *)             die "Unsupported architecture: ${ARCH}" ;;
    esac
    log_ok "detect: arch=${ARCH}"
}

# =============================================================================
# Phase 3: Prepare the bundle to install (sets BUNDLE_DIR)
#
#   --project <dir>  → config-driven online install: assemble a bundle from the
#                      config (download components + render) into a temp dir.
#   --local <path>   → offline: use a pre-built bundle tarball or directory.
# =============================================================================

phase_prepare_bundle() {
    if [[ -n "${PROJECT_DIR}" ]]; then
        # Config-driven online install: build the bundle from the config, here.
        stage_check_deps
        BUNDLE_DIR=$(mktemp -d)
        trap 'rm -rf "${BUNDLE_DIR}"' EXIT
        assemble_bundle "${BUNDLE_DIR}" "${ARCH}"
        log_ok "prepare: assembled bundle from ${PROJECT_DIR}"
        return
    fi

    if [[ -n "${LOCAL_PATH}" ]]; then
        if [[ -d "${LOCAL_PATH}" ]]; then
            BUNDLE_DIR="${LOCAL_PATH}"
        elif [[ -f "${LOCAL_PATH}" ]]; then
            BUNDLE_DIR=$(mktemp -d)
            trap 'rm -rf "${BUNDLE_DIR}"' EXIT
            tar -xzf "${LOCAL_PATH}" -C "${BUNDLE_DIR}" --strip-components=1
        else
            die "Local path is neither a file nor directory: ${LOCAL_PATH}"
        fi
        log_ok "prepare: using local bundle"
        return
    fi

    die "one of --project <dir> (online) or --local <bundle> (offline) is required"
}

# =============================================================================
# Phase 5: Verify checksums
# =============================================================================

phase_verify() {
    local checksums_file="${BUNDLE_DIR}/checksums.sha256"
    if [[ ! -f "${checksums_file}" ]]; then
        log_warn "verify: no checksums.sha256 in bundle (skipping verification)"
        return
    fi

    # Verify each file listed in checksums
    local failed=0
    while IFS='  ' read -r expected_hash filepath; do
        [[ -z "${expected_hash}" ]] && continue
        [[ "${expected_hash}" =~ ^# ]] && continue
        local full_path="${BUNDLE_DIR}/${filepath}"
        if [[ ! -f "${full_path}" ]]; then
            log_err "verify: file missing: ${filepath}"
            failed=1
            continue
        fi
        local actual_hash
        actual_hash=$(sha256sum "${full_path}" | awk '{print $1}')
        if [[ "${actual_hash}" != "${expected_hash}" ]]; then
            log_err "verify: checksum mismatch: ${filepath}"
            log_info "  expected: ${expected_hash}"
            log_info "  actual:   ${actual_hash}"
            failed=1
        fi
    done < "${checksums_file}"

    if [[ ${failed} -ne 0 ]]; then
        die "Checksum verification failed — bundle may be corrupted"
    fi
    log_ok "verify: all checksums pass"
}

# =============================================================================
# Phase 6: System user
# =============================================================================

phase_system_user() {
    if id "${ANOLIS_USER}" &>/dev/null; then
        # User exists — ensure groups are correct
        usermod -aG "${ANOLIS_GROUPS}" "${ANOLIS_USER}" 2>/dev/null || true
        log_skip "system user: ${ANOLIS_USER} already exists"
    else
        useradd --system --shell /usr/sbin/nologin --home-dir "${PREFIX}" --create-home "${ANOLIS_USER}" || \
            die "Failed to create system user: ${ANOLIS_USER}"
        usermod -aG "${ANOLIS_GROUPS}" "${ANOLIS_USER}" 2>/dev/null || \
            log_warn "system user: could not add to all groups (${ANOLIS_GROUPS}) — some may not exist yet"
        log_ok "system user: created ${ANOLIS_USER}"
    fi
}

# =============================================================================
# Phase 7: I2C
# =============================================================================

phase_i2c() {
    # Skip on non-Pi (x86_64 dev machines)
    if [[ "${ARCH}" == "x86_64" ]]; then
        log_skip "i2c: skipped on x86_64"
        return
    fi

    if ls /dev/i2c-* &>/dev/null; then
        log_ok "i2c: already enabled"
        return
    fi

    # Try Bookworm path first, then legacy
    local config_file=""
    if [[ -f /boot/firmware/config.txt ]]; then
        config_file="/boot/firmware/config.txt"
    elif [[ -f /boot/config.txt ]]; then
        config_file="/boot/config.txt"
    else
        log_warn "i2c: cannot find boot config — enable I2C manually"
        return
    fi

    if grep -q "^dtparam=i2c_arm=on" "${config_file}"; then
        log_warn "i2c: configured in ${config_file} but /dev/i2c-* not present — reboot required"
        REBOOT_NEEDED=1
        return
    fi

    echo "dtparam=i2c_arm=on" >> "${config_file}"
    REBOOT_NEEDED=1
    log_ok "i2c: enabled in ${config_file} (reboot required)"
}

# =============================================================================
# Phase 8: Dependencies
# =============================================================================

phase_deps() {
    # Skip if offline (no LOCAL_PATH means online, but check if apt works)
    if ! command -v apt-get &>/dev/null; then
        log_skip "deps: apt-get not available"
        return
    fi

    if dpkg -s i2c-tools &>/dev/null 2>&1; then
        log_skip "deps: i2c-tools already installed"
        return
    fi

    # Only install if online
    if [[ -n "${LOCAL_PATH}" ]] && ! curl -fsS --connect-timeout 3 http://archive.ubuntu.com &>/dev/null 2>&1; then
        log_warn "deps: i2c-tools not installed (offline — install manually: apt-get install i2c-tools)"
        return
    fi

    # shellcheck disable=SC2015  # intentional: error handler covers both failures
    apt-get update -qq && apt-get install -y -qq i2c-tools || {
        log_warn "deps: failed to install i2c-tools (non-fatal)"
        return
    }
    log_ok "deps: i2c-tools installed"
}

# =============================================================================
# Phase 9: Directories
# =============================================================================

phase_directories() {
    mkdir -p "${PREFIX}"/{bin,config/providers,projects,.prev}
    chown -R "${ANOLIS_USER}:${ANOLIS_USER}" "${PREFIX}"
    log_ok "directories: ${PREFIX}/"
}

# =============================================================================
# Phase 10: Backup existing binaries
# =============================================================================

phase_backup() {
    if [[ ! -f "${PREFIX}/bin/anolis-runtime" ]]; then
        log_skip "backup: no existing install"
        return
    fi

    cp "${PREFIX}"/bin/* "${PREFIX}/.prev/" 2>/dev/null || {
        log_warn "backup: failed to copy binaries to .prev/"
        return
    }
    log_ok "backup: existing binaries saved to .prev/"
}

# =============================================================================
# Phase 11: Install binaries
# =============================================================================

phase_install_binaries() {
    if [[ ! -d "${BUNDLE_DIR}/bin" ]]; then
        die "Bundle missing bin/ directory"
    fi

    cp "${BUNDLE_DIR}"/bin/* "${PREFIX}/bin/"
    chmod +x "${PREFIX}"/bin/*
    chown "${ANOLIS_USER}:${ANOLIS_USER}" "${PREFIX}"/bin/*
    log_ok "install binaries: $(find "${BUNDLE_DIR}/bin/" -maxdepth 1 -type f -printf '%f ' 2>/dev/null || ls "${BUNDLE_DIR}/bin/")"
}

# =============================================================================
# Phase 12: Config (project) — always overwrite
# =============================================================================

phase_config_project() {
    # Determine profile name from bundle
    local profile_dir
    profile_dir=$(find "${BUNDLE_DIR}/projects" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | head -1)

    if [[ -z "${profile_dir}" ]]; then
        log_warn "config (project): no projects/ in bundle"
        return
    fi

    local profile_name
    profile_name=$(basename "${profile_dir}")
    INSTALLED_PROFILE="${profile_name}"

    rm -rf "${PREFIX}/projects/${profile_name}"
    cp -r "${profile_dir}" "${PREFIX}/projects/${profile_name}"
    chown -R "${ANOLIS_USER}:${ANOLIS_USER}" "${PREFIX}/projects/${profile_name}"
    log_ok "config (project): ${profile_name}"
}

# =============================================================================
# Phase 13: Config (runtime) — skip if exists
# =============================================================================

phase_config_runtime() {
    local src="${BUNDLE_DIR}/config/runtime.yaml"
    local dest="${PREFIX}/config/runtime.yaml"

    if [[ ! -f "${src}" ]]; then
        log_warn "config (runtime): no config/runtime.yaml in bundle"
        return
    fi

    if [[ -f "${dest}" ]]; then
        log_skip "config (runtime): ${dest} exists (preserved)"
        return
    fi

    cp "${src}" "${dest}"
    chown "${ANOLIS_USER}:${ANOLIS_USER}" "${dest}"
    log_ok "config (runtime): written"
}

# =============================================================================
# Phase 14: Config (providers) — skip each if exists
# =============================================================================

phase_config_providers() {
    local src_dir="${BUNDLE_DIR}/config/providers"
    if [[ ! -d "${src_dir}" ]]; then
        log_skip "config (providers): no config/providers/ in bundle"
        return
    fi

    local installed=0 skipped=0
    for src_file in "${src_dir}"/*.yaml; do
        [[ -f "${src_file}" ]] || continue
        local filename
        filename=$(basename "${src_file}")
        local dest="${PREFIX}/config/providers/${filename}"

        if [[ -f "${dest}" ]]; then
            skipped=$((skipped + 1))
        else
            cp "${src_file}" "${dest}"
            chown "${ANOLIS_USER}:${ANOLIS_USER}" "${dest}"
            installed=$((installed + 1))
        fi
    done

    if [[ ${skipped} -gt 0 ]]; then
        log_skip "config (providers): ${skipped} preserved, ${installed} written"
    else
        log_ok "config (providers): ${installed} written"
    fi
}

# =============================================================================
# Phase 15: Manifest
# =============================================================================

phase_manifest() {
    local src="${BUNDLE_DIR}/manifest.json"
    if [[ ! -f "${src}" ]]; then
        log_warn "manifest: no manifest.json in bundle"
        return
    fi
    cp "${src}" "${PREFIX}/manifest.json"
    chown "${ANOLIS_USER}:${ANOLIS_USER}" "${PREFIX}/manifest.json"
    log_ok "manifest: written"
}

# =============================================================================
# Phase 16: systemd units
# =============================================================================

phase_systemd() {
    local src_dir="${BUNDLE_DIR}/systemd"
    if [[ ! -d "${src_dir}" ]]; then
        log_warn "systemd: no systemd/ in bundle"
        return
    fi

    local installed=0
    for unit_file in "${src_dir}"/*.service; do
        [[ -f "${unit_file}" ]] || continue
        local filename
        filename=$(basename "${unit_file}")
        cp "${unit_file}" "${SYSTEMD_DIR}/${filename}"
        installed=$((installed + 1))
    done

    if [[ ${installed} -eq 0 ]]; then
        log_warn "systemd: no .service files in bundle"
        return
    fi

    systemctl daemon-reload

    # Enable all anolis services
    for unit_file in "${src_dir}"/*.service; do
        [[ -f "${unit_file}" ]] || continue
        local filename
        filename=$(basename "${unit_file}")
        systemctl enable "${filename}" 2>/dev/null || true
    done

    log_ok "systemd: ${installed} units installed and enabled"
}

# =============================================================================
# Phase 17: Hostname
# =============================================================================

phase_hostname() {
    # Skip on x86_64 (dev machines)
    if [[ "${ARCH}" == "x86_64" ]]; then
        log_skip "hostname: skipped on x86_64"
        return
    fi

    local serial_file="/sys/firmware/devicetree/base/serial-number"
    if [[ ! -f "${serial_file}" ]]; then
        log_warn "hostname: no serial number found (skipping)"
        return
    fi

    local serial
    serial=$(tr -d '\0' < "${serial_file}")
    local suffix="${serial: -8}"
    local new_hostname="anolis-${suffix}"
    local current_hostname
    current_hostname=$(hostname)

    if [[ "${current_hostname}" == "${new_hostname}" ]]; then
        log_skip "hostname: already ${new_hostname}"
        return
    fi

    hostnamectl set-hostname "${new_hostname}" 2>/dev/null || {
        # Fallback for systems without hostnamectl
        echo "${new_hostname}" > /etc/hostname
        hostname "${new_hostname}"
    }
    log_ok "hostname: ${new_hostname}"
}

# =============================================================================
# Phase 18: Start services
# =============================================================================

phase_start() {
    if [[ ${NO_START} -eq 1 ]]; then
        log_skip "start: --no-start specified"
        return
    fi

    systemctl restart anolis-runtime 2>/dev/null || {
        log_warn "start: failed to restart anolis-runtime"
        return
    }
    log_ok "start: anolis-runtime restarted"
}

# =============================================================================
# Phase 19: Health check
# =============================================================================

phase_health() {
    if [[ ${NO_START} -eq 1 ]]; then
        log_skip "health: skipped (services not started)"
        return
    fi

    local url="http://localhost:${DEFAULT_PORT}/v0/runtime/status"
    local elapsed=0

    while [[ ${elapsed} -lt ${HEALTH_TIMEOUT} ]]; do
        if curl -fsS "${url}" &>/dev/null; then
            local version
            version=$(curl -fsS "${url}" 2>/dev/null | grep -o '"version":"[^"]*"' | head -1 | sed 's/"version":"\(.*\)"/\1/' || echo "unknown")
            log_ok "health: runtime responding (v${version})"
            return
        fi
        sleep ${HEALTH_INTERVAL}
        elapsed=$((elapsed + HEALTH_INTERVAL))
    done

    log_warn "health: runtime not responding after ${HEALTH_TIMEOUT}s"
    if [[ ${REBOOT_NEEDED} -eq 1 ]]; then
        log_info "This may be normal — I2C was just enabled. Reboot and check:"
    else
        log_info "Check logs:"
    fi
    log_info "  sudo systemctl status anolis-runtime"
    log_info "  sudo journalctl -u anolis-runtime --no-pager -n 20"
}

# =============================================================================
# Phase 20: Summary
# =============================================================================

phase_summary() {
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo " Anolis installation complete"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    log_info "Prefix:   ${PREFIX}"
    log_info "Profile:  ${INSTALLED_PROFILE:-unknown}"
    log_info "Port:     ${DEFAULT_PORT}"

    if [[ "${ARCH}" != "x86_64" ]]; then
        local hostname_display
        hostname_display=$(hostname)
        log_info "Hostname: ${hostname_display}"
        log_info "Access:   http://${hostname_display}.local:${DEFAULT_PORT}"
    fi

    if [[ ${REBOOT_NEEDED} -eq 1 ]]; then
        echo ""
        log_warn "REBOOT REQUIRED for I2C changes to take effect"
        log_info "  sudo reboot"
    fi
    echo ""
}

# =============================================================================
# Uninstall
# =============================================================================

do_uninstall() {
    log_info "Uninstalling anolis from ${PREFIX}..."

    # Stop and disable services
    for unit in "${SYSTEMD_DIR}"/anolis-*.service; do
        [[ -f "${unit}" ]] || continue
        local name
        name=$(basename "${unit}")
        systemctl stop "${name}" 2>/dev/null || true
        systemctl disable "${name}" 2>/dev/null || true
        rm -f "${unit}"
    done
    systemctl daemon-reload 2>/dev/null || true

    # Remove installation directory
    if [[ -d "${PREFIX}" ]]; then
        rm -rf "${PREFIX}"
        log_ok "removed ${PREFIX}"
    fi

    log_ok "uninstall complete"
    log_info "Note: system user '${ANOLIS_USER}' was preserved"
}

# =============================================================================
# Dry run wrapper
# =============================================================================

dry_run_phase() {
    local phase_name="$1"
    log_info "[dry-run] would execute: ${phase_name}"
}

# =============================================================================
# Stage: build an offline bundle from a local config
#
# Dev-side (no root, no install): given a config directory (a machine-profile +
# its configs + behaviors — e.g. a tweaked copy of an example), fetch the pinned
# component binaries, render production configs, and package a self-contained
# bundle that `install.sh --local` installs offline. Generic over whatever
# providers the machine-profile declares.
# =============================================================================

# The canonical single service unit, shipped with install.sh (not the examples
# repo). The runtime supervises providers as child subprocesses, so there are no
# per-provider units.
emit_systemd_unit() {
    cat <<'UNIT'
# Canonical Anolis service unit — ONE unit for the whole stack.
#
# The runtime spawns and supervises each provider as a child subprocess (per the
# `providers:` block in runtime.yaml), over the child's stdin/stdout. Providers
# are NOT independent systemd services. Do not add per-provider units or `Wants=`
# them; that double-spawns each provider and contends for the I2C bus. The
# provider children inherit this unit's SupplementaryGroups for bus access.
[Unit]
Description=Anolis Runtime
After=network.target

[Service]
Type=simple
User=anolis
Group=anolis
SupplementaryGroups=i2c gpio dialout
ExecStart=/opt/anolis/bin/anolis-runtime --config /opt/anolis/config/runtime.yaml
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
UNIT
}

# Download one release asset into <dir>.
_stage_fetch() {
    local repo="$1" tag="$2" asset="$3" dir="$4"
    local auth=()
    [[ -n "${GITHUB_TOKEN:-}" ]] && auth=(-H "Authorization: Bearer ${GITHUB_TOKEN}")
    curl -fsSL "${auth[@]}" -o "${dir}/${asset}" \
        "https://github.com/${repo}/releases/download/${tag}/${asset}" \
        || die "stage: failed to download ${asset} from ${repo} ${tag}"
}

# Tools needed to build a bundle from a config (staging / online install).
stage_check_deps() {
    local tool
    for tool in curl tar python3 sha256sum; do
        command -v "${tool}" >/dev/null 2>&1 \
            || die "'${tool}' is required to build a bundle from a config"
    done
    python3 -c 'import yaml' 2>/dev/null \
        || die "python3 'pyyaml' is required to build from a config (pip install pyyaml)"
}

# Assemble a bundle (bin + rendered config + systemd + manifest + checksums)
# from the --project config into <dest> for <arch>. Shared by --stage (offline
# build) and config-driven online install. Generic over the profile's providers.
# Sets ASSEMBLED_PROFILE / ASSEMBLED_RUNTIME_VERSION for the caller.
assemble_bundle() {
    local dest="$1" arch="$2"

    [[ -n "${PROJECT_DIR}" ]] || die "--project <config-dir> is required"
    [[ -d "${PROJECT_DIR}" ]] || die "project dir not found: ${PROJECT_DIR}"
    local mp="${PROJECT_DIR}/machine-profile.yaml"
    [[ -f "${mp}" ]] || die "machine-profile.yaml not found in ${PROJECT_DIR}"

    local profile
    profile=$(basename "$(cd "${PROJECT_DIR}" && pwd)")

    # Parse components generically from the machine-profile.
    local parsed
    parsed=$(python3 - "${mp}" <<'PY'
import sys, yaml
mp = yaml.safe_load(open(sys.argv[1]))
c = (mp or {}).get("components")
if not c:
    sys.exit("no 'components:' block")
r = c["runtime"]
print(f"runtime\t{r['repo']}\t{r['version']}")
for name, cfg in (c.get("providers") or {}).items():
    print(f"provider\t{name}\t{cfg['repo']}\t{cfg['version']}")
PY
) || die "could not parse ${mp}"

    local runtime_repo="" runtime_version=""
    local -a p_names=() p_repos=() p_vers=()
    local kind f1 f2 f3
    while IFS=$'\t' read -r kind f1 f2 f3; do
        case "${kind}" in
            runtime)  runtime_repo="${f1}"; runtime_version="${f2}" ;;
            provider) p_names+=("${f1}"); p_repos+=("${f2}"); p_vers+=("${f3}") ;;
        esac
    done <<< "${parsed}"
    [[ -n "${runtime_version}" ]] || die "no runtime component in ${mp}"
    ASSEMBLED_PROFILE="${profile}"
    ASSEMBLED_RUNTIME_VERSION="${runtime_version}"

    log_ok "assemble: profile=${profile} arch=${arch} runtime=v${runtime_version} providers=(${p_names[*]-})"

    local dl
    dl=$(mktemp -d)
    mkdir -p "${dest}/bin" "${dest}/systemd"

    # Runtime binary + install.sh (rides in the bundle for later --local).
    _stage_fetch "${runtime_repo}" "v${runtime_version}" "anolis-${runtime_version}-linux-${arch}.tar.gz" "${dl}"
    mkdir -p "${dl}/rt"
    tar -xzf "${dl}/anolis-${runtime_version}-linux-${arch}.tar.gz" -C "${dl}/rt"
    cp "${dl}/rt/bin/anolis-runtime" "${dest}/bin/"
    _stage_fetch "${runtime_repo}" "v${runtime_version}" "install.sh" "${dest}"
    chmod +x "${dest}/install.sh"

    # Provider binaries — generic over the declared providers (no hardcoding).
    local i
    for i in "${!p_names[@]}"; do
        local n="${p_names[$i]}" repo="${p_repos[$i]}" ver="${p_vers[$i]}"
        local asset="anolis-provider-${n}-${ver}-linux-${arch}.tar.gz"
        _stage_fetch "${repo}" "v${ver}" "${asset}" "${dl}"
        mkdir -p "${dl}/p-${n}"
        tar -xzf "${dl}/${asset}" -C "${dl}/p-${n}"
        cp "${dl}/p-${n}/bin/anolis-provider-${n}" "${dest}/bin/"
    done

    # Render production configs (dev-relative → ${PREFIX} absolute; bind 0.0.0.0).
    if ! python3 - "${PROJECT_DIR}" "${dest}" "${profile}" "${PREFIX}" <<'PY'
import re, shutil, sys
from pathlib import Path
project, out, profile, prefix = sys.argv[1], Path(sys.argv[2]), sys.argv[3], sys.argv[4]
pd = Path(project)
cfg = pd / "config"
pcmd = re.compile(r"\.\./anolis-provider-([a-z0-9-]+)/build/[^/]+/anolis-provider-([a-z0-9-]+)")
ppath = re.compile(r"\.\./anolis-projects/projects/([a-z0-9-]+)/(config|behaviors)/([^\s\"']+)")


def render(c):
    c = pcmd.sub(lambda m: f"{prefix}/bin/anolis-provider-{m.group(2)}", c)
    c = ppath.sub(lambda m: f"{prefix}/projects/{m.group(1)}/{m.group(2)}/{m.group(3)}", c)
    return re.sub(r"^(\s*bind:\s*)127\.0\.0\.1\s*$", r"\g<1>0.0.0.0", c, flags=re.M)


(out / "config" / "providers").mkdir(parents=True, exist_ok=True)
pcfg = out / "projects" / profile / "config"
pcfg.mkdir(parents=True, exist_ok=True)
beh = pd / "behaviors"
if beh.is_dir():
    bo = out / "projects" / profile / "behaviors"
    bo.mkdir(parents=True, exist_ok=True)
    for f in beh.iterdir():
        if f.is_file():
            shutil.copy2(f, bo / f.name)
rts = sorted(cfg.glob("anolis-runtime.*.yaml"))
if not rts:
    sys.exit(f"no runtime configs in {cfg}")
for rc in rts:
    (pcfg / rc.name).write_text(render(rc.read_text()))
manual = [rc for rc in rts if ".manual." in rc.name] or rts
(out / "config" / "runtime.yaml").write_text(render(manual[0].read_text()))
for pc in sorted(cfg.glob("provider-*.yaml")):
    name = pc.stem.removeprefix("provider-").split(".")[0]
    (out / "config" / "providers" / f"{name}.yaml").write_text(render(pc.read_text()))
    (pcfg / pc.name).write_text(render(pc.read_text()))
mp = pd / "machine-profile.yaml"
if mp.exists():
    shutil.copy2(mp, out / "projects" / profile / "machine-profile.yaml")
PY
    then
        die "config render failed"
    fi

    emit_systemd_unit > "${dest}/systemd/anolis-runtime.service"

    # Manifest — generic over providers.
    if ! python3 - "${dest}/manifest.json" "${profile}" "${runtime_version}" "${arch}" "${p_names[*]-}" "${p_vers[*]-}" <<'PY'
import json, sys
out, profile, version, arch, names, vers = sys.argv[1:7]
names, vers = names.split(), vers.split()
json.dump(
    {
        "schema_version": 1,
        "profile": profile,
        "version": version,
        "arch": arch,
        "components": {
            "runtime": {"version": version},
            "providers": {n: {"version": v} for n, v in zip(names, vers)},
        },
    },
    open(out, "w"),
    indent=2,
)
PY
    then
        die "manifest generation failed"
    fi

    cat > "${dest}/README.txt" <<EOF
Anolis Bundle: ${profile} v${runtime_version} (${arch})

Offline install:
    sudo ./install.sh --local <this-bundle>.tar.gz

Verify:
    anolis-runtime --version
    systemctl status anolis-runtime

See manifest.json for component versions.
EOF

    ( cd "${dest}" && find . -type f -not -name 'checksums.sha256' | sort \
        | xargs sha256sum > checksums.sha256 )

    rm -rf "${dl}"
}

# Resolve a target architecture: explicit --arch, else the host.
resolve_target_arch() {
    local arch="${TARGET_ARCH:-}"
    if [[ -z "${arch}" ]]; then
        arch=$(uname -m)
        case "${arch}" in
            aarch64|arm64) arch="arm64" ;;
            x86_64)        arch="x86_64" ;;
            *) die "unsupported host arch '${arch}' — pass --arch arm64|x86_64" ;;
        esac
    fi
    printf '%s' "${arch}"
}

# --stage: build an offline bundle from a config and tar it (no install).
do_stage() {
    stage_check_deps
    local arch
    arch=$(resolve_target_arch)

    STAGE_TMP=$(mktemp -d)
    # Cleanup fires at script exit; STAGE_TMP is a global so it is still set then
    # (function-locals would be out of scope and unbound under `set -u`).
    trap 'rm -rf "${STAGE_TMP:-}"' EXIT
    local staging="${STAGE_TMP}/staging"

    assemble_bundle "${staging}" "${arch}"

    local bundle="anolis-${ASSEMBLED_PROFILE}-${ASSEMBLED_RUNTIME_VERSION}-${arch}"
    mkdir -p "${STAGE_DIR}"
    local abs_out
    abs_out=$(cd "${STAGE_DIR}" && pwd)
    ( cd "$(dirname "${staging}")" \
        && cp -r "$(basename "${staging}")" "${bundle}" \
        && tar -czf "${abs_out}/${bundle}.tar.gz" "${bundle}" \
        && rm -rf "${bundle}" )
    sha256sum "${abs_out}/${bundle}.tar.gz" | awk '{print $1}' > "${abs_out}/${bundle}.tar.gz.sha256"

    log_ok "stage: ${abs_out}/${bundle}.tar.gz"
    log_info "Install offline with: sudo ./install.sh --local ${abs_out}/${bundle}.tar.gz"
}

# =============================================================================
# Main
# =============================================================================

main() {
    parse_args "$@"

    # Stage mode: build an offline bundle from a local config. Dev-side — no
    # root, no install, no network beyond fetching release assets.
    if [[ -n "${STAGE_DIR}" ]]; then
        do_stage
        exit 0
    fi

    # Phase 1: root check
    phase_root_check

    # Handle uninstall early
    if [[ ${UNINSTALL} -eq 1 ]]; then
        if [[ ${DRY_RUN} -eq 1 ]]; then
            log_info "[dry-run] would uninstall from ${PREFIX}"
            exit 0
        fi
        do_uninstall
        exit 0
    fi

    echo ""
    echo "Anolis Provisioning"
    echo "━━━━━━━━━━━━━━━━━━━"
    echo ""

    if [[ ${DRY_RUN} -eq 1 ]]; then
        log_info "DRY RUN — no changes will be made"
        echo ""
        dry_run_phase "detect architecture"
        if [[ -n "${PROJECT_DIR}" ]]; then
            dry_run_phase "assemble bundle from config ${PROJECT_DIR} (online)"
        else
            dry_run_phase "use local bundle ${LOCAL_PATH:-<required: --project or --local>}"
        fi
        dry_run_phase "verify checksums"
        dry_run_phase "create system user ${ANOLIS_USER}"
        dry_run_phase "enable I2C"
        dry_run_phase "install dependencies (i2c-tools)"
        dry_run_phase "create directories at ${PREFIX}"
        dry_run_phase "backup existing binaries"
        dry_run_phase "install binaries to ${PREFIX}/bin/"
        dry_run_phase "install project config"
        dry_run_phase "install runtime config (skip if exists)"
        dry_run_phase "install provider configs (skip each if exists)"
        dry_run_phase "write manifest.json"
        dry_run_phase "install systemd units"
        dry_run_phase "set hostname"
        [[ ${NO_START} -eq 0 ]] && dry_run_phase "start services"
        [[ ${NO_START} -eq 0 ]] && dry_run_phase "health check"
        echo ""
        exit 0
    fi

    # Execute phases
    phase_detect
    phase_prepare_bundle
    phase_verify
    phase_system_user
    phase_i2c
    phase_deps
    phase_directories
    phase_backup
    phase_install_binaries
    phase_config_project
    phase_config_runtime
    phase_config_providers
    phase_manifest
    phase_systemd
    phase_hostname
    phase_start
    phase_health
    phase_summary
}

# Run main() in every real invocation — direct (`bash install.sh`), piped
# (`curl ... | bash -s --`), and `bash -c`. A BASH_SOURCE==$0 guard cannot be
# used here: the primary install path pipes the script to `bash -s`, where
# BASH_SOURCE is empty (and would trip `set -u`). Unit tests that source this
# file for function-level testing set ANOLIS_INSTALL_SH_NO_MAIN=1 to skip main.
if [[ -z "${ANOLIS_INSTALL_SH_NO_MAIN:-}" ]]; then
    main "$@"
fi
