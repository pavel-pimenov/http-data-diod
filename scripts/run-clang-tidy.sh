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
#   - the lint compile DB is built with -DFMT_CONSTEVAL=: fmt guards its
#     compile-time format-string check with FMT_CONSTEVAL, and with consteval
#     enabled clang cannot constant-evaluate fmt's FMT_STRING for spdlog's
#     format helper. The translation unit then fails to compile and
#     clang-tidy aborts it with "Error while processing" -- not a single
#     project diagnostic is reported, so the sweep silently skipped every
#     file that includes the logger. Without consteval the check degrades to
#     a runtime one, which is irrelevant for a static analysis pass.
#
# Usage:
#   ./scripts/run-clang-tidy.sh        # lint changed C++ files
#   ./scripts/run-clang-tidy.sh --all  # lint all project C++ files (full sweep)
#
# Parallelism:
#   Files are linted by N parallel clang-tidy processes (N = CLANG_TIDY_JOBS,
#   defaults to min(host CPUs, 4)) to avoid a multi-minute serial sweep. Each  
#   TU still re-parses the bundled headers, so memory scales with N; the
#   default fan-out is capped because an oversized batch makes the container
#   OOM-kill workers (SIGKILL / exit 137) and their diagnostics would silently
#   vanish (a false clean sweep). Each TU writes its own log; workers that were
#   killed are retried serially before results are aggregated, so the sweep is
#   always complete regardless of the parallelism.

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
BUILD_DIR="$L2_DIR/build-lint"
BUILD_DIR_CONTAINER="/app/build-lint"

# Diagnostics located outside project sources (system headers and bundled 3rd-party code)
IGNORE_PATH_RE='^(/usr/|/app/(nats/src|httplib|base64|prometheus-cpp|odpi|json)/)'

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
    # Bundled third-party libraries (httplib, base64, prometheus-cpp/civetweb)
    # are skipped the same way as in the "changed" mode.
    local f
    for f in $(git ls-files "$L2_DIR"); do
        case "$f" in
            $L2_DIR/*.cpp|$L2_DIR/*.cc)
                case "$f" in
                    */httplib/*|*/base64/*|*/prometheus-cpp/*) continue ;;
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
    # l2-proxy-version.h is build-generated and not tracked in git. clang-tidy
    # lints the host sources (mounted into the container), so on a fresh
    # checkout (no cached header from rebuild-and-run.sh) the generated file
    # must be produced on the host, not only inside the Docker image.
    if [ ! -f "$L2_DIR/l2-proxy-version.h" ]; then
        log_info "Generating $L2_DIR/l2-proxy-version.h (not tracked in git)"
        "$L2_DIR/generate_version.sh" > "$L2_DIR/l2-proxy-version.h"
    fi
}

ensure_compile_commands() {
    local compile_db="$BUILD_DIR/compile_commands.json"
    if [ -f "$compile_db" ] \
       && [ "$L2_DIR/CMakeLists.txt" -ot "$compile_db" ] \
       && [ "$L2_DIR/Dockerfile" -ot "$compile_db" ] \
       && [ "${BASH_SOURCE[0]}" -ot "$compile_db" ]; then
        return 0
    fi
    log_info "Generating per-file compile_commands.json (clang-tidy)..."
    mkdir -p "$BUILD_DIR"
    docker run --rm -v "$PROJECT_ROOT/$L2_DIR:/app" -w /app "$LINT_IMAGE" sh -c \
        "cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug \
               -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
               -DCMAKE_UNITY_BUILD=OFF \
               -DL2_PROXY_TEST_PCH=OFF \
               -DL2_PROXY_APP_PCH=OFF \
               -DCMAKE_CXX_FLAGS=-DFMT_CONSTEVAL= \
               -S . -B build-lint >/dev/null"
}

main() {
    local mode="changed"
    if [ "$1" = "--all" ]; then
        mode="all"
    fi

    local files=()
    if [ "$mode" = "all" ]; then
        while IFS= read -r f; do files+=("$f"); done < <(all_cpp_files)
        log_info "Full sweep over all project C++ files ($(printf '%s ' "${files[@]}"))"
    else
        while IFS= read -r f; do files+=("$f"); done < <(changed_cpp_files)
    fi
    if [ "${#files[@]}" -eq 0 ]; then
        log_info "No C++ files to lint, skipping clang-tidy"
        return 0
    fi

    log_info "Running clang-tidy on $(printf '%s ' "${files[@]}")"
    ensure_builder_image
    ensure_version_header
    ensure_compile_commands

    local container_files=()
    for f in "${files[@]}"; do
        container_files+=("/app/${f#$L2_DIR/}")
    done

    # nproc is GNU-only: on macOS it is missing, $jobs becomes empty and
    # `xargs -P ''` inside the container lints nothing while the script still
    # reports success (silent false-green). Fall back to sysctl, then to 4.
    local jobs="${CLANG_TIDY_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
    # Cap the auto-detected fan-out: each TU re-parses the bundled headers and
    # can peak above 1 GB, so a large batch OOM-kills workers in the 8 GB
    # container (exit 137). CLANG_TIDY_JOBS still overrides the cap.
    if [ -z "${CLANG_TIDY_JOBS:-}" ] && [ "$jobs" -gt 4 ]; then
        jobs=4
    fi
    log_info "Linting ${#files[@]} files in parallel (jobs=${jobs})..."

    local log logdir f key rc
    log=$(mktemp)
    # The log dir must live on a path Docker Desktop shares with the container:
    # mktemp's default ($TMPDIR=/var/folders/...) is NOT shared, so the mount
    # would be silently empty. $BUILD_DIR is inside the repo and gitignored.
    mkdir -p "$PROJECT_ROOT/$BUILD_DIR"
    logdir=$(mktemp -d "$PROJECT_ROOT/$BUILD_DIR/tidy-logs.XXXXXX")
    # Per-TU helper: keep each clang-tidy process' output and exit code in its
    # own file so a killed worker cannot corrupt or swallow another one's
    # diagnostics when many processes share a single pipe.
    cat > "$logdir/run_one.sh" <<EOS
#!/bin/sh
f="\$1"
key=\$(printf '%s' "\$f" | tr '/' '_')
clang-tidy -p $BUILD_DIR_CONTAINER --header-filter='.*\\.(hpp|h)\$' "\$f" > "/logs/\$key.log" 2>&1
echo \$? > "/logs/\$key.rc"
EOS

    printf '%s\n' "${container_files[@]}" \
        | docker run --rm -i -v "$PROJECT_ROOT/$L2_DIR:/app" -w /app \
            -v "$logdir:/logs" "$LINT_IMAGE" sh -c \
            "xargs -P $jobs -n1 sh /logs/run_one.sh" || true

    # Retry TUs whose worker was killed (rc missing or >= 128, e.g. OOM SIGKILL
    # = 137) one at a time: serial execution stays well under the memory limit.
    for f in "${container_files[@]}"; do
        key=$(printf '%s' "$f" | tr '/' '_')
        rc=$(cat "$logdir/$key.rc" 2>/dev/null || echo killed)
        if [ "$rc" = "killed" ] || { [ "$rc" -ge 128 ] 2>/dev/null; }; then
            log_warn "clang-tidy worker for $f did not finish (rc=$rc); retrying serially"
            docker run --rm -v "$PROJECT_ROOT/$L2_DIR:/app" -w /app \
                -v "$logdir:/logs" "$LINT_IMAGE" sh /logs/run_one.sh "$f" || true
        fi
    done

    # Aggregate per-TU logs in the original file order for stable output.
    for f in "${container_files[@]}"; do
        key=$(printf '%s' "$f" | tr '/' '_')
        if [ -f "$logdir/$key.log" ]; then
            cat "$logdir/$key.log" >> "$log"
        fi
    done

    local real_errors real_warnings
    real_errors=$(grep -E '^/app/[^:]+:[0-9]+:[0-9]+: error:' "$log" | grep -vE "$IGNORE_PATH_RE" || true)
    real_warnings=$(grep -E '^/app/[^:]+:[0-9]+:[0-9]+: warning:' "$log" | grep -vE "$IGNORE_PATH_RE" || true)

    if [ -n "$real_errors" ]; then
        log_error "clang-tidy found errors in project files:"
        echo "$real_errors"
        rm -f "$log"; rm -rf "$logdir"
        return 1
    fi

    if [ -n "$real_warnings" ]; then
        log_warn "clang-tidy warnings in project files (not blocking):"
        echo "$real_warnings"
    else
        log_info "✓ clang-tidy: no errors or warnings in project files"
    fi
    rm -f "$log"; rm -rf "$logdir"
    return 0
}

main "$@"
