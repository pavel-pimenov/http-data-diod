#!/bin/bash

# Shared helper for lint/coverage scripts (run-shellcheck.sh, run-clang-tidy.sh,
# run-cppcheck.sh, ...): fills CONTOUR_BUILD_ARGS with the docker build args for
# the active profile (public by default, closed contour via BUILD_PROFILE=contour).
# Source it (`. "$SCRIPT_DIR/contour-build-args.sh"`) after setting
# PROJECT_ROOT, then:
#
#   contour_build_args
#   docker build --target builder -t "$LINT_IMAGE" "${CONTOUR_BUILD_ARGS[@]}" "$SRC_DIR"
#
# No namerefs: macOS ships bash 3.2, where `local -n` is a syntax error.

contour_build_args() {
    CONTOUR_BUILD_ARGS=()
    if [ -f "${PROJECT_ROOT}/.env" ]; then
        # Only simple VAR=value lines are needed here. Sourcing .env is unsafe:
        # it can contain JSON arrays (L2_SERVER_URLS=[...]) and arbitrary shell.
        while IFS='=' read -r _key _value; do
            case "$_key" in
                '' | \#* | *[[:space:]]*) continue ;;
            esac
            case "$_value" in
                *'['* | *[[:space:]]*) continue ;;
            esac
            export "$_key=$_value"
        done < "${PROJECT_ROOT}/.env"
    fi

    # shellcheck source=build-profile.sh
    . "${PROJECT_ROOT}/scripts/build-profile.sh"
    resolve_build_profile

    CONTOUR_BUILD_ARGS=(
        --build-arg "REGISTRY_MIRROR=${REGISTRY_MIRROR}"
        --build-arg "APT_MIRROR=${APT_MIRROR}"
        --build-arg "BASE_IMAGE=${L2_BASE_IMAGE}"
        --build-arg "PIP_INDEX_URL=${PIP_INDEX_URL}"
    )
}
