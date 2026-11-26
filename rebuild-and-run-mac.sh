#!/bin/bash

# macOS build script for l2-proxy via colima Docker

set -euo pipefail

# Point Docker CLI to colima socket
export DOCKER_HOST=unix://${HOME}/.colima/docker.sock

# Track build time
BUILD_START_TIME=$(date +%s)

export USE_JAEGER=OFF

# Generate l2-proxy-version.h with current git SHA
mkdir -p src/build
./src/generate_version.sh > src/build/l2-proxy-version.h
cp -a src/build/l2-proxy-version.h src/l2-proxy-version.h
rm -f src/build/l2-proxy

echo "Stopping existing containers..."
docker compose down --remove-orphans 2>/dev/null || true

echo "Cleaning up conflicting containers..."
docker rm -f grafana 2>/dev/null || true
docker rm -f l2-proxy l2-worker l2-server nginx 2>/dev/null || true
sleep 1

export DOCKER_BUILDKIT=1

echo "Building Docker images..."
if ! docker compose --progress plain build; then
    docker compose rm -f 2>/dev/null || true
    echo "Retrying build without cache..."
    if ! docker compose --progress plain build --no-cache; then
        echo "Build failed."
        exit 1
    fi
fi

echo ""
echo "Build successful! Starting containers..."

docker compose up -d --remove-orphans

echo ""
echo "Waiting 15 seconds for services to initialize..."
sleep 15

echo ""
echo "Container status:"
docker compose ps

echo ""
echo "=========================================="
echo "Done! Services are running."

BUILD_END_TIME=$(date +%s)
BUILD_DURATION=$((BUILD_END_TIME - BUILD_START_TIME))
BUILD_MINUTES=$((BUILD_DURATION / 60))
BUILD_SECONDS=$((BUILD_DURATION % 60))
echo "Build time: ${BUILD_MINUTES}m ${BUILD_SECONDS}s"
echo ""
echo "Quick test:"
echo "  python3 message_counter.py --iterations 1 --concurrent 1"
echo ""
echo "Health check:"
echo "  ./health-check.sh all"
echo ""
echo "View logs:"
echo "  docker compose logs -f <service>"
echo "=========================================="
