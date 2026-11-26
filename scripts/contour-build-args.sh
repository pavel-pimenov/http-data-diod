#!/bin/bash

# Shared helper for lint scripts (run-shellcheck.sh, run-clang-tidy.sh,
# run-cppcheck.sh): returns the docker build --build-arg list needed to build
# the builder image inside a closed contour.
#
# The contour mirrors (REGISTRY/APT_MIRROR) used to be hardcoded in each script
# separately; this function centralizes loading them from .env and emitting the
# three build args. Source it (`. "$SCRIPT_DIR/contour-build-args.sh"`) after
# setting PROJECT_ROOT, then:
#
#   local build_args=()
#   contour_build_args build_args
#   docker build --target builder -t "$LINT_IMAGE" "${build_args[@]}" "$SRC_DIR"
#
# Usage:
#   contour_build_args <array_name>   # fills <array_name> with --build-arg pairs

contour_build_args() {
    # nameref gives direct (safe) access to the caller's array, avoiding eval
    # with interpolated .env values (command injection via a malicious .env).
    local -n out="$1"
    out=()
    if [ -f "${PROJECT_ROOT}/.env" ]; then
        # shellcheck disable=SC1091
        set -a; source "${PROJECT_ROOT}/.env"; set +a
        out+=(--build-arg "REGISTRY_MIRROR=${REGISTRY_MIRROR:-${REGISTRY:-}}")
        out+=(--build-arg "APT_MIRROR=${APT_MIRROR:-}")
        out+=(--build-arg "BASE_IMAGE=${BASE_IMAGE:-${REGISTRY_MIRROR:-${REGISTRY:-}}ubuntu:26.04}")
    fi
}
