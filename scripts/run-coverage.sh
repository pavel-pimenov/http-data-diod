#!/bin/bash
# Renders the unit-test coverage report for src.
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
    -f src/Dockerfile src

CID="$(docker create "$IMAGE_TAG")"
trap 'docker rm -f "$CID" >/dev/null 2>&1 || true' EXIT

mkdir -p "$OUT_DIR"
docker cp "$CID:/app/out/coverage/." "$OUT_DIR/"

# Per-file regression gate: no production file may lose line coverage compared
# with the committed baseline (docs/coverage-baseline.json). Fails the script
# (and the CI coverage job) on a regression or on a new file that is not in the
# baseline yet -- accept it deliberately: scripts/coverage-regression-check.py --update
echo ""
echo "=== Coverage regression check (per-file vs docs/coverage-baseline.json) ==="
python3 "$REPO_ROOT/scripts/coverage-regression-check.py" --report "$OUT_DIR/cov.json"

echo ""
echo "=== Coverage report: $OUT_DIR/coverage.html ==="
echo "Open in a browser (line/branch coverage per source file)."
echo "Note: the coverage image build FAILS (non-zero exit) if total line"
echo "coverage drops below the gate in src/Dockerfile (--fail-under-line)."
