#!/usr/bin/env bash
# shellcheck disable=SC2034  # unused vars are future flags (telemetry, observability)
# Anolis Runtime — Standalone Provisioning Script
# Downloads and installs the anolis runtime + providers from a bundle.
#
# Usage:
#   Online:  curl -fsSL https://github.com/anolishq/anolis/releases/latest/download/install.sh | sudo bash -s -- --profile bioreactor-v1
#   Offline: sudo ./install.sh --local ./anolis-bioreactor-v1-0.3.0-arm64.tar.gz
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
readonly GITHUB_API="https://api.github.com"
readonly PROJECTS_REPO="anolishq/anolis-projects"
readonly ANOLIS_USER="anolis"
readonly ANOLIS_GROUPS="i2c,gpio,dialout"
readonly SYSTEMD_DIR="/etc/systemd/system"

# =============================================================================
# Globals (set by argument parsing)
# =============================================================================

PROFILE=""
VERSION=""
LOCAL_PATH=""
WITH_TELEMETRY=0
WITH_OBSERVABILITY=0
NO_START=0
UNINSTALL=0
DRY_RUN=0
PREFIX="${DEFAULT_PREFIX}"
REBOOT_NEEDED=0

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

Options:
  --profile <name>       Machine profile to install (required in online mode)
  --version <ver>        Pin bundle version (default: latest)
  --local <path>         Install from local bundle tarball or directory
  --with-telemetry       Also install telemetry-export service
  --with-observability   Also install observability stack (Docker Compose)
  --no-start             Install but don't start services
  --uninstall            Remove anolis installation
  --dry-run              Print what would happen without doing it
  --prefix <path>        Override install prefix (default: /opt/anolis)
  --help, -h             Show this help

Environment:
  GITHUB_TOKEN           GitHub API token (optional, avoids rate limits)

Examples:
  # Online install (Pi has internet):
  curl -fsSL https://github.com/anolishq/anolis/releases/latest/download/install.sh | \
    sudo bash -s -- --profile bioreactor-v1

  # Offline install from bundle:
  sudo ./install.sh --local ./anolis-bioreactor-v1-0.3.0-arm64.tar.gz
EOF
    exit 0
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --profile)   PROFILE="${2:-}"; shift 2 ;;
            --version)   VERSION="${2:-}"; shift 2 ;;
            --local)     LOCAL_PATH="${2:-}"; shift 2 ;;
            --with-telemetry)    WITH_TELEMETRY=1; shift ;;
            --with-observability) WITH_OBSERVABILITY=1; shift ;;
            --no-start)  NO_START=1; shift ;;
            --uninstall) UNINSTALL=1; shift ;;
            --dry-run)   DRY_RUN=1; shift ;;
            --prefix)    PREFIX="${2:-}"; shift 2 ;;
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
# Phase 3: Resolve bundle source
# =============================================================================

phase_resolve() {
    if [[ -n "${LOCAL_PATH}" ]]; then
        # Local mode — validate path
        if [[ ! -e "${LOCAL_PATH}" ]]; then
            die "Local path not found: ${LOCAL_PATH}"
        fi
        log_ok "resolve: local path ${LOCAL_PATH}"
        return
    fi

    # Online mode — need profile
    if [[ -z "${PROFILE}" ]]; then
        die "Either --profile or --local is required"
    fi

    # Query GitHub API for the release
    local api_url="${GITHUB_API}/repos/${PROJECTS_REPO}/releases"
    local auth_header=""
    if [[ -n "${GITHUB_TOKEN:-}" ]]; then
        auth_header="Authorization: Bearer ${GITHUB_TOKEN}"
    fi

    local releases
    if [[ -n "${auth_header}" ]]; then
        releases=$(curl -fsSL -H "Accept: application/vnd.github+json" -H "${auth_header}" "${api_url}" 2>/dev/null) || {
            log_err "GitHub API request failed"
            log_info "Download manually from: https://github.com/${PROJECTS_REPO}/releases"
            log_info "Then run: sudo $0 --local <downloaded-file>"
            exit 1
        }
    else
        releases=$(curl -fsSL -H "Accept: application/vnd.github+json" "${api_url}" 2>/dev/null) || {
            log_err "GitHub API request failed (rate limited or offline?)"
            log_info "Set GITHUB_TOKEN to avoid rate limits, or download manually:"
            log_info "  https://github.com/${PROJECTS_REPO}/releases"
            log_info "Then run: sudo $0 --local <downloaded-file>"
            exit 1
        }
    fi

    # Find the matching release
    local tag_prefix="${PROFILE}-"
    local target_tag=""

    if [[ -n "${VERSION}" ]]; then
        # Exact version requested
        target_tag="${PROFILE}-${VERSION}"
    fi

    # Parse releases JSON to find the right one
    # Uses a simple grep/sed approach to avoid jq dependency
    local found_tag=""
    local found_url=""

    if [[ -n "${target_tag}" ]]; then
        # Look for exact tag
        found_tag=$(printf '%s' "${releases}" | grep -o "\"tag_name\":\"${target_tag}\"" | head -1 | sed 's/.*"tag_name":"\(.*\)"/\1/' || true)
    else
        # Look for latest tag with our prefix
        found_tag=$(printf '%s' "${releases}" | grep -o "\"tag_name\":\"${tag_prefix}[^\"]*\"" | head -1 | sed 's/.*"tag_name":"\(.*\)"/\1/' || true)
    fi

    if [[ -z "${found_tag}" ]]; then
        if [[ -n "${VERSION}" ]]; then
            die "Release not found: ${target_tag}"
        else
            die "No releases found for profile: ${PROFILE}"
        fi
    fi

    # Find the asset URL for our architecture
    local asset_pattern="anolis-${PROFILE}-.*-${ARCH}\\.tar\\.gz"
    found_url=$(printf '%s' "${releases}" | grep -o "\"browser_download_url\":\"[^\"]*${asset_pattern}\"" | head -1 | sed 's/.*"browser_download_url":"\(.*\)"/\1/' || true)

    if [[ -z "${found_url}" ]]; then
        die "No ${ARCH} bundle asset found in release ${found_tag}"
    fi

    BUNDLE_URL="${found_url}"
    BUNDLE_TAG="${found_tag}"
    log_ok "resolve: ${found_tag} (${ARCH})"
}

# =============================================================================
# Phase 4: Download bundle
# =============================================================================

phase_download() {
    if [[ -n "${LOCAL_PATH}" ]]; then
        # Local mode — extract if tarball, use directly if directory
        if [[ -d "${LOCAL_PATH}" ]]; then
            BUNDLE_DIR="${LOCAL_PATH}"
        elif [[ -f "${LOCAL_PATH}" ]]; then
            BUNDLE_DIR=$(mktemp -d)
            trap 'rm -rf "${BUNDLE_DIR}"' EXIT
            tar -xzf "${LOCAL_PATH}" -C "${BUNDLE_DIR}" --strip-components=1
        else
            die "Local path is neither a file nor directory: ${LOCAL_PATH}"
        fi
        log_ok "download: using local bundle"
        return
    fi

    # Online mode — download
    BUNDLE_DIR=$(mktemp -d)
    trap 'rm -rf "${BUNDLE_DIR}"' EXIT

    local tmp_tarball="${BUNDLE_DIR}.tar.gz"
    local auth_args=()
    if [[ -n "${GITHUB_TOKEN:-}" ]]; then
        auth_args=(-H "Authorization: Bearer ${GITHUB_TOKEN}")
    fi

    curl -fsSL "${auth_args[@]}" -o "${tmp_tarball}" "${BUNDLE_URL}" || {
        rm -f "${tmp_tarball}"
        die "Failed to download bundle from ${BUNDLE_URL}"
    }

    tar -xzf "${tmp_tarball}" -C "${BUNDLE_DIR}" --strip-components=1
    rm -f "${tmp_tarball}"
    log_ok "download: bundle extracted"
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
# Main
# =============================================================================

main() {
    parse_args "$@"

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
        dry_run_phase "resolve bundle (profile=${PROFILE:-<from bundle>}, version=${VERSION:-latest})"
        dry_run_phase "download/extract bundle"
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
    phase_resolve
    phase_download
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

main "$@"
