#!/bin/bash

# Build-profile resolver shared by rebuild-and-run.sh and contour-build-args.sh.
#
#   public  (default) — Docker Hub images, distro Ubuntu sources, pypi.org.
#                       No .env required; stale mirror addresses in an old .env
#                       are deliberately ignored.
#   contour           — closed contour: external images and Ubuntu apt repos
#                       are pulled through mirrors from .env (dp.nlmk values
#                       below are only the documented example defaults).
#
# Callers load .env (if present) first, then call resolve_build_profile. The
# function exports the effective values so both compose interpolation and raw
# `docker build --build-arg` invocations see the same configuration.
# L2_BASE_IMAGE is the fully resolved BASE_IMAGE for build args/logging.

resolve_build_profile() {
    local profile="${BUILD_PROFILE:-public}"
    case "$profile" in
        public | internet)
            BUILD_PROFILE=public
            REGISTRY=""
            REGISTRY_MIRROR=""
            APT_MIRROR=""
            PIP_INDEX_URL="https://pypi.org/simple"
            BASE_IMAGE="ubuntu:26.04"
            ;;
        contour | closed)
            BUILD_PROFILE=contour
            REGISTRY="${REGISTRY:-docker-registry.dp.nlmk.com/}"
            REGISTRY_MIRROR="${REGISTRY_MIRROR:-${REGISTRY}}"
            APT_MIRROR="${APT_MIRROR:-https://repos.dp.nlmk.com/artifactory/archive-ubuntu-remote/}"
            PIP_INDEX_URL="${PIP_INDEX_URL:-https://repos.dp.nlmk.com/artifactory/api/pypi/pypi/simple}"
            BASE_IMAGE="${BASE_IMAGE:-${REGISTRY_MIRROR}ubuntu:26.04}"
            ;;
        *)
            echo "Unknown BUILD_PROFILE='$profile' (expected: public|contour)" >&2
            return 1
            ;;
    esac
    L2_BASE_IMAGE="${BASE_IMAGE}"
    export BUILD_PROFILE REGISTRY REGISTRY_MIRROR APT_MIRROR PIP_INDEX_URL BASE_IMAGE L2_BASE_IMAGE
}
