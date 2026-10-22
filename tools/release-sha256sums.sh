#!/usr/bin/env bash
# release-sha256sums.sh — write or check a release's SHA256SUMS (#446).
#
#   release-sha256sums.sh write <dir>   write <dir>/SHA256SUMS over the assets
#   release-sha256sums.sh check <dir>   check <dir>/SHA256SUMS against them
#
# The assets are the release's tarballs, install.sh and its manifest JSON
# files: the ones a consumer downloads and runs or verifies. metrics.json is
# informational and uploaded by a job the checksum job does not wait for, so it
# is not covered. `check` fails unless SHA256SUMS has exactly one line for
# install.sh and one for every asset in <dir>, and every line for a file in
# <dir> matches it.
set -euo pipefail

usage() {
    echo "usage: $0 write|check <dir>" >&2
    exit 64
}

[[ $# -eq 2 ]] || usage
mode="$1"
dir="$2"
[[ -d "${dir}" ]] || { echo "release-sha256sums: no such directory: ${dir}" >&2; exit 1; }
cd "${dir}"

# install.sh is a literal word, not a pattern, so nullglob never drops it:
# write needs the file and check needs its line even when it was not downloaded.
shopt -s nullglob
assets=(*.tar.gz install.sh manifest*.json *-manifest.json)
shopt -u nullglob
# A file can match two patterns; list each once, in a stable order.
mapfile -t assets < <(printf '%s\n' "${assets[@]}" | LC_ALL=C sort -u)

case "${mode}" in
    write)
        [[ -f install.sh ]] || { echo "release-sha256sums: install.sh is not in ${dir}" >&2; exit 1; }
        sha256sum "${assets[@]}" > SHA256SUMS
        ;;
    check)
        [[ -f SHA256SUMS ]] || { echo "release-sha256sums: no SHA256SUMS in ${dir}" >&2; exit 1; }
        status=0
        for name in "${assets[@]}"; do
            count=$(awk -v n="${name}" '$2 == n || $2 == "*" n { c++ } END { print c + 0 }' SHA256SUMS)
            if [[ "${count}" -ne 1 ]]; then
                echo "release-sha256sums: SHA256SUMS has ${count} lines for ${name}, expected 1" >&2
                status=1
            fi
        done
        if ! sha256sum -c --ignore-missing --quiet SHA256SUMS; then
            status=1
        fi
        exit "${status}"
        ;;
    *)
        usage
        ;;
esac
