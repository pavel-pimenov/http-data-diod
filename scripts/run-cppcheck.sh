#!/bin/bash

# Run cppcheck on project C++ files inside a container.
#
# The host has no C/C++ toolchain, so static analysis runs in the builder Docker
# image (http-data-diod:builder), which contains cppcheck, all dependencies and
# the bundled third-party headers.
#
# Behaviour:
#   - no matching files -> exits 0 (nothing to check)
#   - findings (error/warning/performance/portability) in project files -> prints
#     them and exits 1
#   - findings in bundled third-party code (httplib, base64, nats, odpi, json,
#     prometheus-cpp) are suppressed via src/cppcheck-suppressions.txt: cppcheck
#     parses those headers transitively and their diagnostics are not actionable
#     in project code (e.g. base64 endianness #error, json returnByReference).
#
# Only error/warning/performance/portability severities are gated. The `style`
# and `information` severities are intentionally excluded: cppcheck analyses
# each TU in isolation, so it reports `unusedFunction`/`unusedStructMember`
# false positives for symbols used from other TUs, and `missingInclude` noise.
#
# Usage:
#   ./scripts/run-cppcheck.sh        # check changed C++ files
#   ./scripts/run-cppcheck.sh --all  # check all project C++ files (full sweep)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

LINT_IMAGE="http-data-diod:builder"
L2_DIR="src"

changed_cpp_files() {
    local f
    for f in $(git diff --cached --name-only; git diff --name-only); do
        case "$f" in
            $L2_DIR/*.cpp|$L2_DIR/*.cc|$L2_DIR/*.h|$L2_DIR/*.hpp)
                case "$f" in
                    */httplib/*|*/base64/*|*/odpi/*|*/json/*|*/nats/*|*/prometheus-cpp/*) continue ;;
                esac
                echo "$f"
                ;;
        esac
    done
}

all_cpp_files() {
    local f
    for f in $(git ls-files "$L2_DIR"); do
        case "$f" in
            $L2_DIR/*.cpp|$L2_DIR/*.cc|$L2_DIR/*.h|$L2_DIR/*.hpp)
                case "$f" in
                    */httplib/*|*/base64/*|*/odpi/*|*/json/*|*/nats/*|*/prometheus-cpp/*) continue ;;
                esac
                echo "$f"
                ;;
        esac
    done
}

ensure_builder_image() {
    if ! docker image inspect "$LINT_IMAGE" >/dev/null 2>&1; then
        log_info "Builder image not found, building ${LINT_IMAGE}..."
        docker build --target builder -t "$LINT_IMAGE" "$L2_DIR"
    fi
}

ensure_version_header() {
    # l2-proxy-version.h is build-generated and not tracked in git. cppcheck
    # checks the host sources (mounted into the container), so on a fresh
    # checkout the generated header must exist for the includes to resolve.
    if [ ! -f "$L2_DIR/l2-proxy-version.h" ]; then
        log_info "Generating $L2_DIR/l2-proxy-version.h (not tracked in git)"
        "$L2_DIR/generate_version.sh" > "$L2_DIR/l2-proxy-version.h"
    fi
}

main() {
    local mode="changed"
    if [ "$1" = "--all" ]; then
        mode="all"
    fi

    local files=()
    if [ "$mode" = "all" ]; then
        while IFS= read -r f; do files+=("$f"); done < <(all_cpp_files)
        log_info "Full sweep over all project C++ files (${#files[@]} files)"
    else
        while IFS= read -r f; do files+=("$f"); done < <(changed_cpp_files)
    fi
    if [ "${#files[@]}" -eq 0 ]; then
        log_info "No C++ files to check, skipping cppcheck"
        return 0
    fi

    ensure_builder_image
    ensure_version_header

    local container_files=()
    for f in "${files[@]}"; do
        container_files+=("/app/${f#$L2_DIR/}")
    done

    # nproc is GNU-only (missing on macOS): fall back to sysctl, then 4.
    local jobs="${CPPCHECK_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
    if [ -z "${CPPCHECK_JOBS:-}" ] && [ "$jobs" -gt 4 ]; then
        jobs=4
    fi

    log_info "Running cppcheck on $(printf '%s ' "${files[@]}")"
    local out
    out=$(docker run --rm -v "$PROJECT_ROOT/$L2_DIR:/app" -w /app "$LINT_IMAGE" \
        cppcheck \
        --std=c++23 \
        --enable=warning,performance,portability \
        --check-level=exhaustive \
        --inline-suppr \
        --suppress=missingIncludeSystem \
        --suppressions-list=/app/cppcheck-suppressions.txt \
        -DL2_PROXY_BUILD_MODE="release" \
        -I/app -I/app/httplib -I/app/base64 -I/app/odpi/include -I/app/json -I/app/nats/src \
        -j "$jobs" \
        --template='{file}:{line}: {severity}: {id}: {message}' \
        "${container_files[@]}" 2>&1 || true)

    # Everything reported from a project file at a gated severity blocks the gate.
    local findings
    findings=$(printf '%s\n' "$out" \
        | grep -E '^/app/[^:]+:[0-9]+: (error|warning|performance|portability): ' || true)

    if [ -n "$findings" ]; then
        log_error "cppcheck found issues in project files:"
        printf '%s\n' "$findings"
        return 1
    fi

    log_info "✓ cppcheck: no errors/warnings/performance/portability issues in project files"
    return 0
}

main "$@"
