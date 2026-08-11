#!/bin/bash

# Run clang-tidy on changed C++ files inside a container.
#
# The host has no C/C++ toolchain, so static analysis runs in the builder Docker
# image (http-data-diod:builder), which contains clang-tidy, all dependencies
# and the NATS library headers.
#
# Behavior:
#   - no changed C++ files  -> exits 0 (nothing to lint)
#   - real errors in project files -> prints them and exits 1
#   - warnings in project files    -> prints them and exits 0 (the codebase has
#     pre-existing style warnings, e.g. Prometheus metric members in snake_case)
#   - diagnostics from system headers (/usr/include, ...) and bundled third-party
#     code (httplib, base64, nats/src) are filtered out: fmt/spdlog consteval
#     false positives are not actionable in project code.
#
# Usage:
#   ./scripts/run-clang-tidy.sh        # lint changed C++ files
#   ./scripts/run-clang-tidy.sh --all  # lint all project C++ files (full sweep)
#
# Parallelism:
#   Files are linted by N parallel clang-tidy processes (N = CLANG_TIDY_JOBS,
#   defaults to the number of host CPUs) to avoid a multi-minute serial sweep.
#   Each TU still re-parses the bundled headers, so memory scales with N.

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
L2_DIR="cpp/l2-proxy"
BUILD_DIR="$L2_DIR/build-lint"
BUILD_DIR_CONTAINER="/app/build-lint"

# Diagnostics located outside project sources (system headers and bundled 3rd-party code)
IGNORE_PATH_RE='^(/usr/|/app/(nats/src|httplib|base64)/)'

changed_cpp_files() {
    local f
    for f in $(git diff --cached --name-only; git diff --name-only); do
        case "$f" in
            $L2_DIR/*.cpp|$L2_DIR/*.cc|$L2_DIR/*.h|$L2_DIR/*.hpp)
                case "$f" in
                    */httplib/*|*/base64/*) continue ;;
                esac
                echo "$f"
                ;;
        esac
    done
}

all_cpp_files() {
    # Full sweep lints translation units only: headers are covered via the
    # include graph of the .cpp files, so linting each header as its own TU
    # would be redundant and very slow (e.g. the bundled nats C headers).
    local f
    for f in $(git ls-files "$L2_DIR"); do
        case "$f" in
            $L2_DIR/*.cpp|$L2_DIR/*.cc)
                case "$f" in
                    */httplib/*|*/base64/*) continue ;;
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

ensure_compile_commands() {
    local compile_db="$BUILD_DIR/compile_commands.json"
    if [ -f "$compile_db" ] \
       && [ "$L2_DIR/CMakeLists.txt" -ot "$compile_db" ] \
       && [ "$L2_DIR/Dockerfile" -ot "$compile_db" ]; then
        return 0
    fi
    log_info "Generating per-file compile_commands.json (clang-tidy)..."
    mkdir -p "$BUILD_DIR"
    docker run --rm -v "$PROJECT_ROOT/$L2_DIR:/app" -w /app "$LINT_IMAGE" sh -c \
        "cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug \
               -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
               -DCMAKE_UNITY_BUILD=OFF \
               -S . -B build-lint >/dev/null"
}

main() {
    local mode="changed"
    if [ "$1" = "--all" ]; then
        mode="all"
    fi

    local files
    if [ "$mode" = "all" ]; then
        mapfile -t files < <(all_cpp_files)
        log_info "Full sweep over all project C++ files ($(printf '%s ' "${files[@]}"))"
    else
        mapfile -t files < <(changed_cpp_files)
    fi
    if [ "${#files[@]}" -eq 0 ]; then
        log_info "No C++ files to lint, skipping clang-tidy"
        return 0
    fi

    log_info "Running clang-tidy on $(printf '%s ' "${files[@]}")"
    ensure_builder_image
    ensure_compile_commands

    local container_files=()
    for f in "${files[@]}"; do
        container_files+=("/app/${f#$L2_DIR/}")
    done

    local jobs="${CLANG_TIDY_JOBS:-$(nproc)}"
    log_info "Linting ${#files[@]} files in parallel (jobs=${jobs})..."

    local log
    log=$(mktemp)
    printf '%s\n' "${container_files[@]}" \
        | docker run --rm -i -v "$PROJECT_ROOT/$L2_DIR:/app" -w /app "$LINT_IMAGE" sh -c \
            "xargs -P $jobs -n1 \
                clang-tidy -p $BUILD_DIR_CONTAINER \
                    --header-filter='.*\.(hpp|h)\$'" \
        > "$log" 2>&1 || true

    local real_errors real_warnings
    real_errors=$(grep -E '^/app/[^:]+:[0-9]+:[0-9]+: error:' "$log" | grep -vE "$IGNORE_PATH_RE" || true)
    real_warnings=$(grep -E '^/app/[^:]+:[0-9]+:[0-9]+: warning:' "$log" | grep -vE "$IGNORE_PATH_RE" || true)

    if [ -n "$real_errors" ]; then
        log_error "clang-tidy found errors in project files:"
        echo "$real_errors"
        rm -f "$log"
        return 1
    fi

    if [ -n "$real_warnings" ]; then
        log_warn "clang-tidy warnings in project files (not blocking):"
        echo "$real_warnings"
    else
        log_info "✓ clang-tidy: no errors or warnings in project files"
    fi
    rm -f "$log"
    return 0
}

main "$@"
