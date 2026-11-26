#!/bin/bash

# Shared helpers for project shell scripts. Source it (`. "$SCRIPT_DIR/lib-common.sh"`)
# after setting SCRIPT_DIR (the dir containing the sourcing script), or call
# init_project_paths to derive both SCRIPT_DIR and PROJECT_ROOT.
#
# Provides:
#   init_project_paths    - sets SCRIPT_DIR/PROJECT_ROOT and cd's into the repo
#   log_info/log_warn/log_error - colorized console logging
#   ensure_version_header - regenerates src/l2-proxy-version.h when missing
#   ensure_builder_image  - builds the builder image (via contour-build-args.sh)
#                           if it is not present

# Resolve SCRIPT_DIR (dir of the sourcing script) and PROJECT_ROOT, then cd into
# the repo root so all relative paths below resolve from a known place.
init_project_paths() {
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[1]}")" && pwd)"
    PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
    cd "$PROJECT_ROOT"
}

# Colorized logging helpers. All three use the same GREEN/YELLOW/RED scheme.
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# Ensure src/l2-proxy-version.h exists. The header is build-generated and not
# tracked in git; host-side linting (clang-tidy/cppcheck) needs it for the
# includes to resolve on a fresh checkout.
ensure_version_header() {
    if [ ! -f "$L2_DIR/l2-proxy-version.h" ]; then
        log_info "Generating $L2_DIR/l2-proxy-version.h (not tracked in git)"
        "$L2_DIR/generate_version.sh" > "$L2_DIR/l2-proxy-version.h"
    fi
}

# Ensure the builder image exists, building it (with contour mirrors from .env)
# when it is missing. The build context defaults to PROJECT_ROOT/src and can be
# overridden via the optional first argument.
ensure_builder_image() {
    local src_dir="${1:-$PROJECT_ROOT/src}"
    if ! docker image inspect "$LINT_IMAGE" >/dev/null 2>&1; then
        log_info "Builder image not found, building ${LINT_IMAGE}..."
        local build_args=()
        contour_build_args build_args
        docker build --target builder -t "$LINT_IMAGE" "${build_args[@]}" "$src_dir"
    fi
}

# shellcheck source=contour-build-args.sh
_LIB_COMMON_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$_LIB_COMMON_DIR/contour-build-args.sh"
unset _LIB_COMMON_DIR
