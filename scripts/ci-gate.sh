#!/bin/bash

# Shared CI gate: the single source of truth for the repo's gate steps.
# pre-commit.sh and .github/workflows/ci.yml both call into this script so the
# offline and runtime checks cannot drift apart.
#
# Usage:
#   ./scripts/ci-gate.sh unit       Python unit tests (no containers)
#   ./scripts/ci-gate.sh offline    Offline gates: metric-name consistency,
#                                   env-var <-> compose, docker build-context
#   ./scripts/ci-gate.sh runtime    Runtime gates against a live stack:
#                                   message counter, DB gateway e2e, golden
#                                   metrics (--traffic --db), /metrics
#                                   consistency. Warns (rc=0) when the stack is
#                                   down or postgres is unavailable.
#   ./scripts/ci-gate.sh all        unit + offline + runtime
#
# SKIP_METRICS_CHECK=1 skips the metric-name gates (offline+runtime).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

SKIP_METRICS_CHECK="${SKIP_METRICS_CHECK:-0}"

g_unit() {
    log_info "Running Python unit tests (tests/)..."
    if ! python3 -m unittest discover -s tests 2>&1; then
        log_error "Python unit tests FAILED!"
        return 1
    fi
    log_info "✓ Python unit tests passed"
}

g_offline() {
    if [ "$SKIP_METRICS_CHECK" = "1" ]; then
        log_warn "SKIP_METRICS_CHECK=1 — metric-name gates skipped"
    else
        log_info "Running metric-name consistency check (offline)..."
        if ! python3 scripts/metrics-consistency-check.py --offline; then
            log_error "Metric names disagree between C++, dashboards, README and golden check!"
            return 1
        fi
        log_info "✓ Metric consistency (offline) passed"
    fi

    log_info "Running env-var <-> docker-compose consistency check..."
    if ! python3 scripts/env-consistency-check.py; then
        log_error "C++ env vars are not wired in docker-compose.yml!"
        return 1
    fi
    log_info "✓ Env-var consistency passed"

    log_info "Running docker build-context check..."
    if ! python3 scripts/docker-context-check.py; then
        log_error "Dockerfile references files excluded from the build context!"
        return 1
    fi
    log_info "✓ Docker build-context passed"
}

g_runtime() {
    if ! docker compose ps 2>/dev/null | grep -q "Up"; then
        log_warn "Stack is down — runtime gates skipped (run ./rebuild-and-run.sh)"
        return 0
    fi

    log_info "Running message consistency test (1 iteration, 1 concurrent)..."
    if ! python3 message_counter.py --iterations 1 --concurrent 1 --dup-check 2>&1; then
        log_error "Message counter test FAILED!"
        echo "To debug:"
        echo "  1. Check service logs: docker compose logs"
        echo "  2. Run health check: ./health-check.sh"
        echo "  3. Rebuild services: ./rebuild-and-run.sh"
        return 1
    fi
    log_info "✓ Message counter test passed"

    if docker compose ps postgres 2>/dev/null | grep -q "Up"; then
        log_info "Running DB gateway e2e test..."
        if ! python3 scripts/db-gateway-e2e-test.py 2>&1; then
            log_error "DB Gateway e2e test FAILED!"
            return 1
        fi
        log_info "✓ DB Gateway e2e passed"

        log_info "Running golden metrics check (--traffic --db)..."
        if ! python3 scripts/metrics-golden-check.py --traffic --db 2>&1; then
            log_error "Traffic/DB metric families incomplete or zero in VictoriaMetrics!"
            return 1
        fi
        log_info "✓ Golden metrics check passed"
    else
        log_warn "postgres is down — DB Gateway metric checks skipped"
    fi

    if [ "$SKIP_METRICS_CHECK" = "1" ]; then
        log_warn "SKIP_METRICS_CHECK=1 — runtime metric consistency skipped"
    else
        log_info "Running metric-name consistency check (runtime)..."
        if ! python3 scripts/metrics-consistency-check.py --runtime; then
            log_error "Exported metrics do not match the C++ registrations!"
            return 1
        fi
        log_info "✓ Metric consistency (runtime) passed"
    fi
}

g_all() {
    g_unit
    g_offline
    g_runtime
}

main() {
    local cmd="${1:?usage: $0 unit|offline|runtime|all}"
    shift
    case "$cmd" in
        unit)     g_unit ;;
        offline)  g_offline ;;
        runtime)  g_runtime ;;
        all)      g_all ;;
        *) log_error "unknown ci-gate command: $cmd"; return 1 ;;
    esac
}

main "$@"