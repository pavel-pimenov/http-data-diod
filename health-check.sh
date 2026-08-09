#!/bin/bash

# Health check script for C++ L2 Proxy services
# Usage: ./health-check.sh [service] [max_retries] [interval]
#
# Services:
#   proxy  - L2 Service Proxy (port 8888)
#   worker - L2 Service Worker (port 19091)
#   server - L2 Server (port 8088 or 3333)
#   all    - Check all services (default)
#   watch  - Continuous monitoring mode
#
# Examples:
#   ./health-check.sh              # Check all services, 3 retries
#   ./health-check.sh proxy 5      # Check proxy, 5 retries
#   ./health-check.sh watch 30     # Watch mode, check every 30s
#   ./health-check.sh watch        # Watch mode, default 60s interval

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Default values
SERVICE="${1:-all}"
MAX_RETRIES="${2:-2}"
RETRY_DELAY=5

# Service ports
PROXY_PORT="${PROXY_PORT:-8888}"
WORKER_METRICS_PORT="${WORKER_METRICS_PORT:-19091}"
SERVER_PORT="${SERVER_PORT:-8088}"
PROXY_METRICS_PORT="${PROXY_METRICS_PORT:-19090}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check health endpoint for a service
check_health() {
    local name="$1"
    local port="$2"
    local endpoint="${3:-/metrics}"
    local retries=0

    while [ $retries -lt $MAX_RETRIES ]; do
        # Try to get metrics endpoint (lighter than health)
        if curl -sf --connect-timeout 5 --max-time 10 "http://localhost:${port}${endpoint}" > /dev/null 2>&1; then
            log_info "✓ $name metrics endpoint OK (port $port)"
            return 0
        fi

        retries=$((retries + 1))
        if [ $retries -lt $MAX_RETRIES ]; then
            log_warn "$name metrics check failed (attempt $retries/$MAX_RETRIES), retrying in ${RETRY_DELAY}s..."
            sleep $RETRY_DELAY
        fi
    done

    log_error "✗ $name metrics endpoint unavailable after $MAX_RETRIES attempts (port $port)"
    return 1
}

# Check metrics endpoint (fallback if no /health)
check_metrics() {
    local name="$1"
    local port="$2"
    local retries=0

    while [ $retries -lt $MAX_RETRIES ]; do
        if curl -sf --connect-timeout 5 --max-time 10 "http://localhost:${port}/metrics" > /dev/null 2>&1; then
            log_info "✓ $name metrics endpoint OK (port $port)"
            return 0
        fi

        retries=$((retries + 1))
        if [ $retries -lt $MAX_RETRIES ]; then
            log_warn "$name metrics check failed (attempt $retries/$MAX_RETRIES), retrying in ${RETRY_DELAY}s..."
            sleep $RETRY_DELAY
        fi
    done

    log_error "✗ $name metrics endpoint unavailable after $MAX_RETRIES attempts (port $port)"
    return 1
}

# Restart a service
restart_service() {
    local name="$1"

    log_warn "Attempting to restart $name..."

    # Check if we're in docker-compose context
    if [ -f "docker-compose.yml" ]; then
        if docker compose ps | grep -q "$name"; then
            docker compose restart "$name" 2>/dev/null || docker-compose restart "$name" 2>/dev/null || true
            log_info "Waiting for $name to restart (10 seconds)..."
            sleep 10
            return 0
        fi
    fi

    # Check if running as Docker container
    if docker ps | grep -q "$name"; then
        docker restart "$name" 2>/dev/null || true
        log_info "Waiting for $name to restart (10 seconds)..."
        sleep 10
        return 0
    fi

    log_warn "Could not find $name in Docker - manual restart may be required"
    return 1
}

# Check and restart if needed
check_and_restart() {
    local name="$1"
    local port="$2"
    local endpoint="${3:-/health}"

    if ! check_health "$name" "$port" "$endpoint"; then
        # Try metrics as fallback
        if ! check_metrics "$name" "$port"; then
            restart_service "$name"

            # Re-check after restart
            if ! check_health "$name" "$port" "$endpoint"; then
                log_error "Service $name failed to recover after restart!"
                return 1
            fi
        fi
    fi

    return 0
}

# Run health checks for all services (extracted for reuse in watch mode)
run_health_checks() {
    local exit_code=0
    local unhealthy_services=()

    # Check proxy
    if ! check_and_restart "l2-proxy" "$PROXY_PORT" "/health"; then
        unhealthy_services+=("l2-proxy")
        exit_code=1
    fi

    # Check proxy metrics
    if ! check_metrics "l2-proxy-metrics" "$PROXY_METRICS_PORT"; then
        unhealthy_services+=("l2-proxy-metrics")
    fi

    # Check worker
    if ! check_and_restart "l2-worker" "$WORKER_METRICS_PORT" "/metrics"; then
        unhealthy_services+=("l2-worker")
        exit_code=1
    fi

    # Check L2 Server (prometheus metrics port 19092)
    if ! check_and_restart "l2-server" "19092" "/metrics"; then
        unhealthy_services+=("l2-server")
        exit_code=1
    fi


    # Check Nginx
    if ! check_health "nginx" "7777" "/nginx-health"; then
        unhealthy_services+=("nginx")
        exit_code=1
    fi

    # Check Jaeger
    if ! check_metrics "jaeger" "16686"; then
        # Jaeger UI doesn't have /metrics, check UI
        if curl -sf --connect-timeout 5 --max-time 10 "http://localhost:16686" > /dev/null 2>&1; then
            log_info "✓ jaeger UI is accessible"
        else
            unhealthy_services+=("jaeger")
        fi
    fi

    echo ""
    echo "=========================================="
    if [ ${#unhealthy_services[@]} -eq 0 ]; then
        log_info "All health checks passed!"
    else
        log_error "Unhealthy services: ${unhealthy_services[*]}"
        echo ""
        echo "To view logs: docker compose logs <service>"
        echo "To restart: docker compose restart <service>"
    fi
    echo "=========================================="

    return $exit_code
}

# Main health check logic
main() {
    local exit_code=0
    local unhealthy_services=()

    log_info "Starting health check for: $SERVICE"
    log_info "Max retries: $MAX_RETRIES, Retry delay: ${RETRY_DELAY}s"
    echo ""

    case "$SERVICE" in
        proxy)
            if ! check_and_restart "l2-proxy" "$PROXY_PORT" "/health"; then
                unhealthy_services+=("l2-proxy")
                exit_code=1
            fi
            if ! check_metrics "l2-proxy-metrics" "$PROXY_METRICS_PORT"; then
                unhealthy_services+=("l2-proxy-metrics")
            fi
            ;;

        worker)
            if ! check_and_restart "l2-worker" "$WORKER_METRICS_PORT" "/metrics"; then
                unhealthy_services+=("l2-worker")
                exit_code=1
            fi
            ;;

        server)
            if ! check_and_restart "l2-server" "19092" "/metrics"; then
                unhealthy_services+=("l2-server")
                exit_code=1
            fi
            ;;

        l2-server)
            if ! check_and_restart "l2-server" "19092" "/metrics"; then
                unhealthy_services+=("l2-server")
                exit_code=1
            fi
            ;;

        watch)
            # Continuous monitoring mode
            local interval="${2:-60}"
            log_info "Starting continuous health monitoring (interval: ${interval}s)"
            log_info "Press Ctrl+C to stop"
            echo ""

            while true; do
                echo ""
                echo "=========================================="
                log_info "Health check at $(date '+%Y-%m-%d %H:%M:%S')"
                echo "=========================================="

                run_health_checks
                local check_result=$?

                if [ $check_result -ne 0 ]; then
                    log_warn "Health check failed - services restarted if needed"
                else
                    log_info "All services healthy"
                fi

                echo ""
                log_info "Next check in ${interval}s..."
                sleep "$interval"
            done
            ;;

        all|*)
            run_health_checks
            exit_code=$?
            ;;
    esac

    return $exit_code
}

# Run main function
main "$@"
