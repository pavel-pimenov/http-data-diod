#!/bin/bash
# Start the glitchtip profile (self-hosted Sentry-compatible server + its
# dedicated postgres) and point l2-proxy / l2-worker at it WITHOUT a full
# image rebuild. Unlike the old sentry-mock, glitchtip has no fixed DSN: you
# register the first user, create an organization + project in the UI
# (http://localhost:8000), copy the project DSN and pass it via SENTRY_DSN.
#
# Usage:
#   SENTRY_DSN="http://<public-key>@glitchtip:8000/<project-id>" \
#       ./scripts/run-glitchtip-stack.sh            # start glitchtip + reconfigure
#   ./scripts/run-glitchtip-stack.sh                # start glitchtip (keep current SENTRY_DSN)
#   ./scripts/run-glitchtip-stack.sh --stop         # tear down glitchtip profile
#
# After the script finishes:
#   docker logs -f glitchtip     # watch incoming events / migrations
#   python3 scripts/sentry-e2e-test.py   # run E2E validation (mock receiver)

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
# --stop: tear down glitchtip and restore SENTRY_DSN=''
# ---------------------------------------------------------------------------
if [[ "${1:-}" == "--stop" ]]; then
    log_info "Stopping glitchtip profile..."
    docker compose --profile glitchtip down glitchtip glitchtip-db

    log_info "Recreating l2-server + l2-proxy + l2-worker with SENTRY_DSN cleared..."
    SENTRY_DSN="" docker compose up -d --force-recreate l2-server l2-proxy l2-worker

    log_info "Waiting for services to recover..."
    sleep 10
    ./health-check.sh all
    exit 0
fi

# ---------------------------------------------------------------------------
# Start glitchtip server
# ---------------------------------------------------------------------------
log_info "Starting glitchtip + glitchtip-db (profile glitchtip)..."
docker compose --profile glitchtip up -d glitchtip glitchtip-db

# Wait for the UI to accept connections on port 8000
READY=0
for i in $(seq 1 90); do
    if curl -sf --max-time 3 http://localhost:8000/ >/dev/null 2>&1; then
        READY=1
        break
    fi
    sleep 1
done
if [[ $READY -eq 0 ]]; then
    log_error "glitchtip did not become ready in 90 s. Check: docker compose logs glitchtip"
    exit 1
fi
log_info "glitchtip UI is ready (http://localhost:8000)"

# ---------------------------------------------------------------------------
# Point l2-server / l2-proxy / l2-worker at glitchtip (all on l2_network,
# hostname 'glitchtip' resolves inside the compose network).
# ---------------------------------------------------------------------------
if [ -n "${SENTRY_DSN:-}" ]; then
    SENTRY_DSN="$SENTRY_DSN" \
        docker compose up -d --force-recreate l2-server l2-proxy l2-worker
else
    log_info "SENTRY_DSN is empty — keeping current DSN on the services."
fi

log_info "Waiting for l2-server + l2-proxy + l2-worker to come back up (max 60 s)..."
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

log_info "✅ Stack is up with glitchtip enabled"
log_info "   UI:          http://localhost:8000   (register first user, create org + project)"
log_info "   SENTRY_DSN:  ${SENTRY_DSN:-<unchanged>}  (inside docker: http://<key>@glitchtip:8000/<project-id>)"
log_info "   E2E test:    python3 scripts/sentry-e2e-test.py"
log_info "   Teardown:    $0 --stop"