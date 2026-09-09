#!/bin/bash

# Installs the project's pre-commit hook into .git/hooks.
#
# Git does NOT copy hooks from scripts/ on clone: the "auto-install on clone"
# claim in the old docs was false. Run this once per clone (or after a fresh
# `git clone`) to enable the safety net from AGENTS.md.
#
# Usage:
#   ./scripts/install-git-hooks.sh          # install
#   ./scripts/install-git-hooks.sh --uninstall
#
# The installed hook runs scripts/pre-commit.sh with no commit message, so it
# only executes the tests and lets git decide whether to proceed with the
# commit. SKIP_PRECOMMIT=1 git commit ... bypasses the tests (see AGENTS.md).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
HOOK_PATH="$(cd "$PROJECT_ROOT" && git rev-parse --git-dir)/hooks/pre-commit"

run_install() {
    local target
    target="$(cd "$PROJECT_ROOT" && git rev-parse --git-dir)/hooks/pre-commit"
    mkdir -p "$(dirname "$target")"

    # Attach the hook to pre-commit.sh by path so edits to the script are
    # picked up without reinstalling.
    cat > "$target" <<EOF
#!/bin/bash
exec "$SCRIPT_DIR/pre-commit.sh"
EOF
    chmod +x "$target"
    echo "Installed pre-commit hook: $target"
    echo "Next commit will run health checks + message_counter test."
    echo "Draft commits: SKIP_PRECOMMIT=1 git commit -m \"...\""
}

run_uninstall() {
    rm -f "$HOOK_PATH"
    echo "Removed pre-commit hook: $HOOK_PATH"
}

case "${1:-}" in
    --uninstall) run_uninstall ;;
    *) run_install ;;
esac