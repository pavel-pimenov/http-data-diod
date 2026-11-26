#!/bin/bash

# Syntax-check the inline <script> of the registry-report UI with node.
#
# The UI serves a single in-page <script> embedded in registry-report-server.py.
# A syntax error there (e.g. a Python `# noqa` comment leaking into the JS, or a
# stray token) silently kills the whole page: the registry combo-box stays empty
# and the "Generate report" button never enables. `node --check` catches that.
#
# node is not installed on the host, so the check runs inside the
# registry-report-server container (nodejs is baked into its image).
#
# Usage:
#   ./scripts/check-registry-report-js.sh        # check the default file
#   ./scripts/check-registry-report-js.sh FILE   # check a given python file

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib-common.sh
. "$SCRIPT_DIR/lib-common.sh"
init_project_paths

SRC_FILE="${1:-$PROJECT_ROOT/scripts/registry-report-server.py}"
CONTAINER="${REGISTRY_REPORT_CONTAINER:-registry-report-server}"

if [ ! -f "$SRC_FILE" ]; then
    log_error "File not found: $SRC_FILE"
    exit 1
fi

# Pull every <script>...</script> block out of the python module (the module
# embeds the UI HTML as a plain string; the JS is the only <script> in it).
js_file=$(mktemp)
trap 'rm -f "$js_file"' EXIT
python3 - "$SRC_FILE" > "$js_file" <<'PY'
import re
import sys

path = sys.argv[1]
src = open(path, encoding="utf-8").read()

# All <script>...</script> bodies in the module's HTML strings.
blocks = re.findall(r"<script>(.*?)</script>", src, re.S)
if not blocks:
    print(f"warning: no <script> block found in {path}", file=sys.stderr)
    sys.exit(0)

# Concatenate with a separator; node --check validates the whole thing as one
# program. Each block ends in a newline so statements never join across blocks.
print("\n;\n".join(blocks))
PY

if [ ! -s "$js_file" ]; then
    log_warn "No <script> content to check in $SRC_FILE, skipping"
    exit 0
fi

if ! docker ps --filter "name=$CONTAINER" --format '{{.Names}}' | grep -q "$CONTAINER"; then
    log_error "Container $CONTAINER is not running (need nodejs for the JS check)"
    exit 1
fi

# Copy the extracted JS into the container and run node --check on it.
# -i is required so the piped JS actually reaches the container's stdin.
tmp_in="/tmp/registry-report-check.js"
if docker exec -i "$CONTAINER" sh -c \
        "cat > $tmp_in && node --check $tmp_in && rm -f $tmp_in" < "$js_file"; then
    log_info "✓ registry-report UI JavaScript syntax OK (node --check)"
    exit 0
else
    log_error "✗ registry-report UI JavaScript has a syntax error:"
    log_error "  (in $SRC_FILE — fix the <script> block, then re-run)"
    exit 1
fi
