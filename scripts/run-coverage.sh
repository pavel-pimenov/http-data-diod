#!/usr/bin/env bash
# Renders the unit-test coverage report for cpp/l2-proxy.
#
# The report is built inside the Docker image (coverage stage): the project
# is compiled with --coverage, the unit tests run, and gcovr renders
# coverage.html + details. No gcovr/lcov needed on the host.
#
# Usage:
#   scripts/run-coverage.sh [output-dir]
#
# Default output dir: <repo>/coverage-report (git-ignored).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

OUT_DIR="${1:-$REPO_ROOT/coverage-report}"
IMAGE_TAG="http-data-diod:coverage"

echo "=== Building the coverage image (gcovr) ==="
docker build --target coverage -t "$IMAGE_TAG" \
    -f cpp/l2-proxy/Dockerfile cpp/l2-proxy

CID="$(docker create "$IMAGE_TAG")"
trap 'docker rm -f "$CID" >/dev/null 2>&1 || true' EXIT

mkdir -p "$OUT_DIR"
docker cp "$CID:/app/out/coverage/." "$OUT_DIR/"

echo ""
echo "=== Coverage report: $OUT_DIR/coverage.html ==="
echo "Open in a browser (line/branch coverage per source file)."
echo "Note: the coverage image build FAILS (non-zero exit) if total line"
echo "coverage drops below the gate in cpp/l2-proxy/Dockerfile (--fail-under-line)."