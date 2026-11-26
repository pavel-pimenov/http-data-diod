#!/bin/bash

# Run shellcheck on shell scripts inside a container.
#
# The host usually has no shellcheck, so linting runs in the builder Docker
# image (http-data-diod:builder), which installs the shellcheck package.
#
# Behavior:
#   - no changed *.sh files -> exits 0 (nothing to lint)
#   - real errors in project scripts -> prints them and exits 1
#   - warnings/info/style in project scripts -> printed non-blocking (the
#     codebase has pre-existing style findings), mirroring run-clang-tidy.sh
#
# Usage:
#   ./scripts/run-shellcheck.sh        # lint changed *.sh files
#   ./scripts/run-shellcheck.sh --all  # lint all project *.sh files

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib-common.sh
. "$SCRIPT_DIR/lib-common.sh"

LINT_IMAGE="http-data-diod:builder"

changed_sh_files() {
    local f
    for f in $(git diff --cached --name-only; git diff --name-only); do
        case "$f" in
            *.sh) echo "$f" ;;
        esac
    done
}

all_sh_files() {
    git ls-files '*.sh'
}

format_findings() {
    # Parse shellcheck -f json1 output (one object per file: {"comments": [...]}).
    python3 -c "
import json
import sys
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        it = json.loads(line)
    except ValueError:
        continue
    for c in it.get('comments', []):
        f = c.get('file', '?').replace('/repo/', '', 1)
        msg = c.get('message', '').strip().replace(chr(10), ' ')
        print(f\"{f}:{c.get('line', '?')}:{c.get('column', '?')}: {c.get('level', '?')} [{c.get('code', '?')}] {msg}\")
"
}

main() {
    local mode="changed"
    if [ "$1" = "--all" ]; then
        mode="all"
    fi

    local files=()
    if [ "$mode" = "all" ]; then
        while IFS= read -r f; do files+=("$f"); done < <(all_sh_files)
        log_info "Full sweep over all project shell scripts (${#files[@]} files)"
    else
        while IFS= read -r f; do files+=("$f"); done < <(changed_sh_files)
    fi
    if [ "${#files[@]}" -eq 0 ]; then
        log_info "No shell scripts changed, skipping shellcheck"
        return 0
    fi

    log_info "Running shellcheck on ${#files[@]} shell scripts..."
    ensure_builder_image

    local container_files=()
    for f in "${files[@]}"; do
        container_files+=("/repo/$f")
    done

    local jobs="${SHELLCHECK_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}"
    local log
    log=$(mktemp)
    printf '%s\n' "${container_files[@]}" \
        | docker run --rm -i -v "$PROJECT_ROOT:/repo" -w /repo "$LINT_IMAGE" sh -c \
            "xargs -P $jobs -n1 shellcheck -f json1" \
        > "$log" 2>&1 || true

    local errors notes
    errors=$(grep -F '"level":"error"' "$log" || true)
    notes=$(grep -vF '"level":"error"' "$log" || true)

    if [ -n "$errors" ]; then
        log_error "shellcheck found errors in project scripts:"
        printf '%s\n' "$errors" | format_findings
        rm -f "$log"
        return 1
    fi

    if [ -n "$notes" ]; then
        log_warn "shellcheck warnings/info/style in project scripts (not blocking):"
        printf '%s\n' "$notes" | format_findings
    else
        log_info "✓ shellcheck: no findings in project scripts"
    fi
    rm -f "$log"
    return 0
}

main "$@"