#!/bin/bash

set -euo pipefail

# On macOS the Docker daemon runs inside colima (a lightweight VM). Point the
# Docker CLI at the colima socket, so a single build script works on both
# Linux (daemon reachable directly) and macOS. No-op when the current context
# already reaches a daemon or colima is not installed.
setup_macos_docker() {
    if ! command -v docker >/dev/null 2>&1; then
        return 0
    fi
    if docker info >/dev/null 2>&1; then
        echo "Docker daemon reachable: $(docker context show)"
        return 0
    fi
    if [ "$(uname -s)" = "Darwin" ]; then
        for sock in "${HOME}/.colima/default/docker.sock" "${HOME}/.colima/docker.sock"; do
            if [ -S "$sock" ]; then
                echo "macOS: pointing Docker CLI to colima socket ${sock}"
                export DOCKER_HOST="unix://${sock}"
                if docker info >/dev/null 2>&1; then
                    echo "Docker daemon reachable via colima: $(docker context 2>/dev/null || echo default)"
                    return 0
                fi
            fi
        done
    fi
    echo "Docker daemon not reachable. Start it first:"
    echo "  Linux:  systemctl start docker"
    echo "  macOS:  colima start"
    exit 1
}

setup_macos_docker

# Track build time
BUILD_START_TIME=$(date +%s)

# VM name label for Grafana dashboards (vmagent adds it at scrape time).
# Defaults to the node hostname, can be overridden per VM:
#   VM_NAME=my-node ./rebuild-and-run.sh
export VM_NAME="${VM_NAME:-$(hostname)}"

ENABLE_ASAN=false
COMPOSE_ARGS=()

for arg in "$@"; do
    case "$arg" in
        --asan)
            ENABLE_ASAN=true
            ;;
        *)
            COMPOSE_ARGS+=("$arg")
            ;;
    esac
done

# Docker builds grow the build cache; if the disk is nearly full the
# apt/cmake steps fail with "not enough free space". Prune the cache
# automatically when free space drops below the threshold.
ensure_free_disk_space() {
    local threshold_mb=1024
    local avail_mb
    avail_mb=$(df -P / | awk 'NR==2 {print int($4/1024)}')
    if [ "$avail_mb" -lt "$threshold_mb" ]; then
        echo "⚠️  Low disk space: ${avail_mb} MB available (threshold ${threshold_mb} MB)."
        echo "Pruning Docker build cache and dangling images..."
        docker builder prune -a -f 2>/dev/null || true
        docker image prune -f 2>/dev/null || true
        avail_mb=$(df -P / | awk 'NR==2 {print int($4/1024)}')
        echo "Free space after prune: ${avail_mb} MB"
    else
        echo "Disk space OK: ${avail_mb} MB available"
    fi
}

ensure_free_disk_space

# Configure Docker registry mirrors if config exists
#if [ -f "docker-daemon.json" ]; then
#    echo "Configuring Docker registry mirrors..."
#    sudo mkdir -p /etc/docker
#    sudo cp docker-daemon.json /etc/docker/daemon.json
#    sudo systemctl restart docker
#    sleep 5  # Wait for Docker to restart
#fi

# Generate l2-proxy-version.h with current git SHA
mkdir -p cpp/l2-proxy/build
./cpp/l2-proxy/generate_version.sh > cpp/l2-proxy/build/l2-proxy-version.h
cp -a cpp/l2-proxy/build/l2-proxy-version.h cpp/l2-proxy/l2-proxy-version.h
rm -f cpp/l2-proxy/build/l2-proxy

# Ensure the CA bundle bind-mount source exists as a file.
# Docker creates an empty directory (or fails) when the source is missing.
if [ -f "ca-bundle.crt" ]; then
    echo "Using existing ca-bundle.crt"
elif [ -d "ca-bundle.crt" ]; then
    echo "⚠️  ca-bundle.crt is a directory; remove it manually (sudo rm -rf ca-bundle.crt)"
else
    : > ca-bundle.crt
    echo "Created empty ca-bundle.crt placeholder (mount source for optional CA bundle)"
fi

echo "Stopping existing containers..."
docker compose down --remove-orphans 2>/dev/null || true

# Clean up any conflicting containers (e.g., Grafana)
echo "Cleaning up conflicting containers..."
docker rm -f grafana 2>/dev/null || true
docker rm -f l2-proxy l2-worker l2-server nginx jaeger nginx-exporter 2>/dev/null || true
sleep 1

export DOCKER_BUILDKIT=1

if [ "$ENABLE_ASAN" = true ]; then
    export ENABLE_ASAN=true
    export L2_PROXY_DOCKER_TARGET=runtime-asan
    # The worker keeps the DB gateway (Instant Client) even under ASan: the
    # runtime-db stage layers on top of the same sanitized binary.
    export L2_WORKER_DOCKER_TARGET=runtime-db
    # ASan+LSan roughly triples RSS, so raise the per-container
    # memory limits above the release-mode defaults (1g each). Override via
    # the same env vars if the host has less/more RAM to spare.
    export L2_SERVER_MEM_LIMIT="${L2_SERVER_MEM_LIMIT:-3g}"
    export L2_PROXY_MEM_LIMIT="${L2_PROXY_MEM_LIMIT:-3g}"
    export L2_WORKER_MEM_LIMIT="${L2_WORKER_MEM_LIMIT:-3g}"
    mkdir -p docker-memory-analysis
    echo "=============================================="
    echo "Building in DEBUG mode with sanitizers:"
    echo "  - AddressSanitizer (ASan)"
    echo "  - LeakSanitizer (LSan, detect_leaks=1)"
    echo "  - UndefinedBehaviorSanitizer (UBSan)"
    echo "  CMake: -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON"
    echo "          (-fsanitize=address,undefined -g -O1)"
    echo "  ASAN logs: ./docker-memory-analysis/ (mount /memory-logs)"
    echo "  Memory limits: proxy=${L2_PROXY_MEM_LIMIT} worker=${L2_WORKER_MEM_LIMIT} server=${L2_SERVER_MEM_LIMIT}"
    echo "=============================================="
else
    # set to false (not unset): the `set -u` above trips on unbound vars
    export ENABLE_ASAN=false
    export L2_PROXY_DOCKER_TARGET=runtime
    # Default worker build skips the Oracle Instant Client download so local
    # and test runs work with PostgreSQL only. Production: set
    # L2_WORKER_DOCKER_TARGET=runtime-db (and the `oracle` profile +
    # DB_ORACLE_ENABLED=true) to embed the Oracle client for the DB gateway.
    export L2_WORKER_DOCKER_TARGET="${L2_WORKER_DOCKER_TARGET:-runtime}"
    echo "Building Docker images (RelWithDebInfo, optimized, no sanitizers)..."
fi

echo ""
echo "=== Clean build timing ==="
echo "  Use DOCKER_BUILDKIT=1 for parallel layer building"
echo "  ccache: $(which ccache 2>/dev/null && ccache --version 2>/dev/null | head -1 || echo 'not found')"
echo "  Docker Buildx: $(docker buildx version 2>/dev/null | head -1 || echo 'not found')"
echo ""

BUILD_STEP_START=$(date +%s)

if ! docker compose build --progress=plain ${COMPOSE_ARGS[@]+"${COMPOSE_ARGS[@]}"}; then
    # Remove failed containers, keep cached layers
    docker compose rm -f 2>/dev/null || true

    # Try building again without cache
    echo "Retrying build without cache..."
    if ! docker compose build --no-cache --progress=plain ${COMPOSE_ARGS[@]+"${COMPOSE_ARGS[@]}"}; then
        echo "❌ Build failed."
        echo ""
        echo "=========================================="
        echo "Manual steps to try:"
        echo "  1. docker system prune -a --volumes"
        echo "  2. Check Dockerfile for errors"
        echo "  3. Check disk space: df -h"
        echo "  4. Check Docker logs: docker system events"
        echo "=========================================="
        exit 1
    fi
fi

echo ""
echo "✅ Build successful! Starting containers..."

BUILD_STEP_END=$(date +%s)
BUILD_STEP_DURATION=$((BUILD_STEP_END - BUILD_STEP_START))
BUILD_STEP_MINUTES=$((BUILD_STEP_DURATION / 60))
BUILD_STEP_SECONDS=$((BUILD_STEP_DURATION % 60))
echo "⏱️  Docker image build: ${BUILD_STEP_MINUTES}m ${BUILD_STEP_SECONDS}s"
echo ""

# Start containers
CONTAINER_START=$(date +%s)
docker compose up -d --remove-orphans ${COMPOSE_ARGS[@]+"${COMPOSE_ARGS[@]}"}

# Wait for containers to start
echo ""
echo "Waiting 10 seconds for services to initialize..."
sleep 10

# Check container status
echo ""
echo "Container status:"
docker compose ps

# Check for unhealthy containers from Docker perspective
UNHEALTHY=$(docker compose ps --format json 2>/dev/null | grep -c '"Health":"unhealthy"' || true)
if [ "$UNHEALTHY" -gt 0 ]; then
    echo ""
    echo "⚠️  Warning: $UNHEALTHY container(s) marked as unhealthy by Docker"
fi

# Run application health checks
echo ""
echo "Running application health checks..."
if [ -f "./health-check.sh" ]; then
    HEALTH_OUTPUT=$(./health-check.sh all 2 2>&1)
    HEALTH_RC=$?
    echo "$HEALTH_OUTPUT"

    HEALTH_ERRORS=$(printf "%s\n" "$HEALTH_OUTPUT" | grep '^\[ERROR\]' || true)

    if [ "$HEALTH_RC" -ne 0 ] && [ -n "$HEALTH_ERRORS" ]; then
        echo ""
        echo "⚠️  Some required services failed health check!"
        echo ""
        echo "Attempting automatic restart of unhealthy services..."

        RESTART_OUTPUT=$(./health-check.sh all 1 2>&1 || true)
        echo "$RESTART_OUTPUT"

        echo ""
        echo "Final health check:"
        FINAL_OUTPUT=$(./health-check.sh all 1 2>&1 || true)
        echo "$FINAL_OUTPUT"

        FINAL_ERRORS=$(printf "%s\n" "$FINAL_OUTPUT" | grep '^\[ERROR\]' || true)

        if [ -n "$FINAL_ERRORS" ]; then
            echo ""
            echo "=========================================="
            echo "⚠️  WARNING: Some required services still unhealthy!"
            echo "Check logs: docker compose logs <service>"
            echo "Manual restart: docker compose restart <service>"
            echo "=========================================="
        else
            echo ""
            echo "✅ Required services are healthy."
        fi
    else
        echo ""
        echo "✅ Required health checks passed!"
    fi
else
    echo "⚠️  health-check.sh not found, skipping application health checks"
fi

echo ""
echo "=========================================="
echo "✅ Done! Services are running."
echo ""

# Calculate and display build time
BUILD_END_TIME=$(date +%s)
BUILD_DURATION=$((BUILD_END_TIME - BUILD_START_TIME))
BUILD_MINUTES=$((BUILD_DURATION / 60))
BUILD_SECONDS=$((BUILD_DURATION % 60))

CONTAINER_END=$(date +%s)
CONTAINER_DURATION=$((CONTAINER_END - CONTAINER_START))

echo ""
echo "=== Build phase breakdown ==="
echo "  Docker image build:  ${BUILD_STEP_MINUTES}m ${BUILD_STEP_SECONDS}s"
echo "  Container startup:   ${CONTAINER_DURATION}s"
echo "  Total:               ${BUILD_MINUTES}m ${BUILD_SECONDS}s"
echo ""

echo "Quick test:"
echo "  python3 message_counter.py --iterations 1 --concurrent 1"
echo ""
echo "Health check:"
echo "  ./health-check.sh all"
echo ""
echo "View logs:"
echo "  docker compose logs -f <service>"
if [ "${ENABLE_ASAN:-false}" = true ]; then
    echo ""
    echo "ASan/LSan leak logs:"
    echo "  ls -lah docker-memory-analysis/"
    echo "  grep -R \"SUMMARY: AddressSanitizer\\|LeakSanitizer\" docker-memory-analysis/"
fi
echo "=========================================="

# Update Grafana dashboards if Grafana is accessible
echo ""
if [ -f "scripts/generate-grafana-dashboards.py" ]; then
    # Check if GRAFANA_URL is set, otherwise try localhost:3000
    GRAFANA_URL="${GRAFANA_URL:-http://localhost:3000}"

    # Wait for Grafana to be fully ready (may take 10-15 seconds after container starts)
    echo "⏳ Waiting for Grafana to be fully ready..."
    GRAFANA_READY=false
    for i in $(seq 1 15); do
        if curl -s --connect-timeout 2 --max-time 5 "$GRAFANA_URL/api/health" > /dev/null 2>&1; then
            # Grafana responds, but may still be initializing
            sleep 3
            GRAFANA_READY=true
            break
        fi
        sleep 1
    done

    # Check if Grafana is running
    if [ "$GRAFANA_READY" = true ] && curl -s --connect-timeout 2 --max-time 5 "$GRAFANA_URL/api/health" > /dev/null 2>&1; then
        echo "📊 Grafana detected at $GRAFANA_URL"

        # Setup Prometheus datasource first
        if [ -f "scripts/setup-grafana-datasource.sh" ]; then
            echo "Setting up Prometheus datasource..."
            GRAFANA_URL="$GRAFANA_URL" \
            GRAFANA_USER="${GRAFANA_USER:-admin}" \
            GRAFANA_PASSWORD="${GRAFANA_PASSWORD:-admin}" \
            PROMETHEUS_URL="${PROMETHEUS_URL:-http://victoria-metrics:8428}" \
            bash scripts/setup-grafana-datasource.sh
        fi

        echo ""
        echo "Updating dashboards..."

        # Run dashboard generator with credentials from environment or defaults
        if [ -n "${GRAFANA_API_KEY:-}" ]; then
            python3 scripts/generate-grafana-dashboards.py --correct-dashboards
        elif [ -n "${GRAFANA_USER:-}" ] && [ -n "${GRAFANA_PASSWORD:-}" ]; then
            GRAFANA_URL="$GRAFANA_URL" GRAFANA_USER="$GRAFANA_USER" GRAFANA_PASSWORD="$GRAFANA_PASSWORD" \
                python3 scripts/generate-grafana-dashboards.py --correct-dashboards
        else
            # Try default credentials
            GRAFANA_URL="$GRAFANA_URL" GRAFANA_USER=admin GRAFANA_PASSWORD=admin \
                python3 scripts/generate-grafana-dashboards.py --correct-dashboards 2>&1 | head -30
        fi

        echo ""
        echo "✅ Dashboard update complete!"
        echo "View dashboards at: $GRAFANA_URL/dashboards"
        echo "   Login: admin/admin"
    else
        echo "ℹ️  Grafana not detected at $GRAFANA_URL"
        echo "   To update dashboards later, run:"
        echo "   python3 scripts/generate-grafana-dashboards.py --correct-dashboards"
    fi
fi

