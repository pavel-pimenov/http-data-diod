#!/bin/bash

# Git pre-commit hook wrapper for HTTP-data-diod Proxy
# This script runs tests before allowing a commit
#
# Usage:
#   ./scripts/pre-commit.sh          # Run tests and commit if passed
#   ./scripts/pre-commit.sh "msg"    # Commit with message if tests pass
#
# Or use as git alias:
#   git config alias.safe-commit '!./scripts/pre-commit.sh'
#   git safe-commit "commit message"

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if containers are running
check_containers() {
    if ! docker compose ps 2>/dev/null | grep -q "Up"; then
        log_warn "Containers are not running. Starting services..."
        ./rebuild-and-run.sh
        return $?
    fi
    return 0
}

# Run quick health check
run_health_check() {
    log_info "Running health check..."
    if [ -f "./health-check.sh" ]; then
        if ! ./health-check.sh all 1 > /dev/null 2>&1; then
            log_error "Health check failed!"
            ./health-check.sh all 1
            return 1
        fi
        log_info "✓ Health check passed"
    else
        log_warn "health-check.sh not found, skipping"
    fi
    return 0
}

# Run message counter test
run_message_test() {
    log_info "Running message consistency test (1 iteration, 1 concurrent)..."

    if ! python3 message_counter.py --iterations 1 --concurrent 1 2>&1; then
        log_error "❌ Message counter test FAILED!"
        echo ""
        echo "=========================================="
        echo "Commit blocked due to test failure."
        echo ""
        echo "To debug:"
        echo "  1. Check service logs: docker compose logs"
        echo "  2. Run health check: ./health-check.sh"
        echo "  3. Rebuild services: ./rebuild-and-run.sh"
        echo "=========================================="
        return 1
    fi

    log_info "✓ Message counter test passed"
    return 0
}

# Run clang-tidy on changed C++ files (in the builder container)
run_clang_tidy() {
    log_info "Running clang-tidy on changed C++ files..."
    if ! ./scripts/run-clang-tidy.sh; then
        log_error "clang-tidy found errors in changed C++ files!"
        return 1
    fi
    return 0
}

# Main pre-commit logic
main() {
    local commit_message="$1"

    echo ""
    echo "=========================================="
    echo "🔒 Pre-commit Test Runner"
    echo "=========================================="
    echo ""

    # Check if we have changes to commit
    if [ -z "$(git status --porcelain)" ]; then
        log_warn "No changes to commit"
        exit 0
    fi

    # Check if containers are running
    if ! check_containers; then
        log_error "Failed to start containers"
        exit 1
    fi

    # Wait for services to be ready
    log_info "Waiting 5 seconds for services to stabilize..."
    sleep 5

    # Run health check
    if ! run_health_check; then
        log_error "Pre-commit checks failed!"
        exit 1
    fi

    # Run message test
    if ! run_message_test; then
        log_error "Pre-commit tests failed!"
        exit 1
    fi

    # Run clang-tidy on changed C++ files
    if ! run_clang_tidy; then
        log_error "Pre-commit clang-tidy failed!"
        exit 1
    fi

    echo ""
    echo "=========================================="
    log_info "✅ All pre-commit tests passed!"
    echo "=========================================="
    echo ""

    # If commit message provided, do the commit
    if [ -n "$commit_message" ]; then
        log_info "Committing changes..."
        git add -A
        git commit -m "$commit_message"

        if [ $? -eq 0 ]; then
            echo ""
            log_info "✅ Changes committed successfully!"
            echo ""
            echo "Next steps:"
            echo "  git push origin main"
            echo ""
        else
            log_error "Failed to commit changes"
            exit 1
        fi
    else
        log_info "Tests passed! You can now commit with:"
        echo "  git commit -m \"your message\""
        echo ""
        echo "Or use:"
        echo "  $0 \"your commit message\""
        echo ""
    fi

    return 0
}

# Run main function with all arguments
main "$*"
