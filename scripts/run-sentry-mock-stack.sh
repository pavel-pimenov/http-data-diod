#!/bin/bash
# Start the sentry-mock receiver profile and point l2-proxy / l2-worker at it
# WITHOUT a full image rebuild.
#
# Usage:
#   ./scripts/run-sentry-mock-stack.sh           # start mock + recreate services
#   ./scripts/run-sentry-mock-stack.sh --stop     # tear down sentry-mock profile
#
# After the script finishes:
#   docker logs -f sentry-mock     # watch incoming envelopes
#   python3 scripts/sentry-e2e-test.py   # run E2E validation

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC}  $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# ---------------------------------------------------------------------------
# --stop: tear down sentry-mock and restore SENTRY_DSN=''
# ---------------------------------------------------------------------------
if [[ "${1:-}" == "--stop" ]]; then
    log_info "Stopping sentry-mock profile..."
    docker compose --profile sentry-mock down sentry-mock

    log_info "Recreating l2-proxy + l2-worker with SENTRY_DSN cleared..."
    SENTRY_DSN="" docker compose up -d --force-recreate l2-proxy l2-worker

    log_info "Waiting for services to recover..."
    sleep 10
    ./health-check.sh all
    exit 0
fi

# ---------------------------------------------------------------------------
# Start sentry-mock receiver
# ---------------------------------------------------------------------------
log_info "Starting sentry-mock receiver (profile sentry-mock)..."
docker compose --profile sentry-mock up -d sentry-mock

# Wait for the receiver to accept connections on port 9001
READY=0
for i in $(seq 1 30); do
    if docker compose exec -T sentry-mock \
        python3 -c "import socket; s=socket.create_connection(('127.0.0.1',9001),2); s.close()" \
        >/dev/null 2>&1; then
        READY=1
        break
    fi
    sleep 1
done
if [[ $READY -eq 0 ]]; then
    log_error "sentry-mock receiver did not become healthy in 30 s"
    exit 1
fi
log_info "sentry-mock receiver is ready (port 9001)"

# ---------------------------------------------------------------------------
# Point l2-proxy and l2-worker at the mock (all on l2_network, hostname
# 'sentry-mock' resolves inside the compose network).
# ---------------------------------------------------------------------------
SENTRY_DSN="http://sentry-e2e@sentry-mock:9001/1" \
    docker compose up -d --force-recreate l2-proxy l2-worker

log_info "Waiting for l2-proxy + l2-worker to come back up (max 60 s)..."
HEALTHY=0
for i in $(seq 1 60); do
    if curl -sf http://localhost:8888/health/ready >/dev/null 2>&1 \
       && curl -sf http://localhost:19093/health/ready >/dev/null 2>&1; then
        HEALTHY=1
        break
    fi
    sleep 2
done
if [[ $HEALTHY -eq 0 ]]; then
    log_error "Services did not recover in 120 s. Check: docker compose logs l2-proxy l2-worker"
    exit 1
fi

log_info "✅ Stack is up with Sentry mock ingest enabled"
log_info "   SENTRY_DSN=http://sentry-e2e@sentry-mock:9001/1"
log_info "   Watch envelopes: docker logs -f sentry-mock"
log_info "   E2E test:       python3 scripts/sentry-e2e-test.py"
log_info "   Teardown:       $0 --stop"
