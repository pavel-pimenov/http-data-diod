#!/bin/bash

set -euo pipefail

# SHA is taken from the GIT_SHA env var first (CI/buildx passes it as a
# build-arg; the container has no .git), then falls back to git rev-parse when
# running on a host inside the repo, and finally to "unknown".
if [ -n "${GIT_SHA:-}" ]; then
    SHA="$GIT_SHA"
elif command -v git >/dev/null 2>&1; then
    SHA=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
else
    SHA="unknown"
fi

echo "#ifndef L2_PROXY_VERSION_H"
echo "#define L2_PROXY_VERSION_H"
echo "#define VERSION \"2.0.9-$SHA\""
echo "#endif"
