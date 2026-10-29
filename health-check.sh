#!/bin/bash

# Health check script for C++ L2 Proxy services
# Usage: ./health-check.sh [service] [max_retries] [interval]
#
# Services:
#   proxy   - L2 Service Proxy (port 8890)
#   worker  - L2 Service Worker (health 19093, metrics 19091)
#   server  - L2 Server (port 8088 or 3333)
#   metrics - observability stack: VictoriaMetrics, vmagent, vmalert,
#             NATS, nats-exporter, Perses
#   disk    - free space on the Docker root filesystem
#   all     - Check all services (default)
#   watch   - Continuous monitoring mode
#
# Examples:
#   ./health-check.sh              # Check all services, 3 retries
#   ./health-check.sh proxy 5      # Check proxy, 5 retries
#   ./health-check.sh metrics      # Only the observability stack
#   ./health-check.sh disk         # Only free disk space
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
PROXY_PORT="${PROXY_PORT:-8890}"
WORKER_METRICS_PORT="${WORKER_METRICS_PORT:-19091}"
WORKER_HEALTH_PORT="${WORKER_HEALTH_PORT:-19093}"
SERVER_PORT="${SERVER_PORT:-8088}"
PROXY_METRICS_PORT="${PROXY_METRICS_PORT:-19090}"

# Observability stack (base compose; host ports differ from container ports
# because the FlylinkDC++ hub stack owns 8428/8080 on this host).
VM_PORT="${VM_PORT:-8430}"
VMAGENT_PORT="${VMAGENT_PORT:-8429}"
VMALERT_PORT="${VMALERT_PORT:-8880}"
NATS_HTTP_PORT="${NATS_HTTP_PORT:-8222}"
NATS_EXPORTER_PORT="${NATS_EXPORTER_PORT:-7778}"
PERSES_PORT="${PERSES_PORT:-8089}"

# Disk guard: the 2026-10-27 VictoriaMetrics crash was `no space left on
# device` at ~0.9 GB free, so the critical line is above that.
DISK_WARN_MB="${DISK_WARN_MB:-2048}"
DISK_CRIT_MB="${DISK_CRIT_MB:-1024}"
DISK_CHECK_PATH="${DISK_CHECK_PATH:-/}"

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

# Unified HTTP check — DRY для health/metrics, с ретраями
check_http() {
    local name="$1"
    local port="$2"
    local endpoint="${3:-/metrics}"
    local retries=0
    while [ $retries -lt $MAX_RETRIES ]; do
        # --noproxy '*' : на dev-машинах ALL_PROXY (socks5h) может перехватывать
        # localhost-запросы и валить чек, хотя сервис жив
        if curl -sf --noproxy '*' --connect-timeout 5 --max-time 10 "http://localhost:${port}${endpoint}" > /dev/null 2>&1; then
            log_info "✓ $name endpoint OK (port $port$endpoint)"
            return 0
        fi
        retries=$((retries + 1))
        if [ $retries -lt $MAX_RETRIES ]; then
            log_warn "$name $endpoint check failed (attempt $retries/$MAX_RETRIES), retrying in ${RETRY_DELAY}s..."
            sleep $RETRY_DELAY
        fi
    done
    log_error "✗ $name endpoint unavailable after $MAX_RETRIES attempts (port $port$endpoint)"
    return 1
}
check_health() { check_http "$1" "$2" "${3:-/metrics}"; }
check_metrics() { check_http "$1" "$2" "/metrics"; }

# Restart a service — точное совпадение имени, без подстрок (grep -w)
restart_service() {
    local name="$1"
    local container="${2:-$1}"
    log_warn "Attempting to restart $name..."
    if [ -f "docker-compose.yml" ]; then
        # `-a`: a stopped container is exactly the case we need to restart.
        if docker compose ps -a 2>/dev/null | grep -wq "$name"; then
            docker compose restart "$name" 2>/dev/null || docker-compose restart "$name" 2>/dev/null || true
            log_info "Waiting for $name to restart (10 seconds)..."
            sleep 10
            return 0
        fi
    fi
    if docker ps -a --format '{{.Names}}' 2>/dev/null | grep -wq "$container" || docker ps -a 2>/dev/null | grep -wq "$container"; then
        docker restart "$container" 2>/dev/null || true
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
    local container="${4:-$1}"

    if ! check_health "$name" "$port" "$endpoint"; then
        # Try metrics as fallback
        if ! check_metrics "$name" "$port"; then
            restart_service "$name" "$container"

            # Re-check after restart
            if ! check_health "$name" "$port" "$endpoint"; then
                log_error "Service $name failed to recover after restart!"
                return 1
            fi
        fi
    fi

    return 0
}

# Health-only variant for services without a /metrics endpoint (Perses, NATS).
check_and_restart_health() {
    local name="$1"
    local port="$2"
    local endpoint="$3"
    local container="${4:-$1}"

    if ! check_health "$name" "$port" "$endpoint"; then
        restart_service "$name" "$container"
        if ! check_health "$name" "$port" "$endpoint"; then
            log_error "Service $name failed to recover after restart!"
            return 1
        fi
    fi

    return 0
}

# Observability stack. VictoriaMetrics and vmagent are fatal: when either is
# down, dashboards and alerts silently freeze while every service still looks
# healthy (round 205: VictoriaMetrics was dead for ~43 h and nothing noticed).
# Perses (UI) and nats-exporter (extra NATS metrics) are reported but do not
# fail the check — traffic and metrics storage do not depend on them.
check_metrics_stack() {
    local failed=0

    if ! check_and_restart "victoria-metrics" "$VM_PORT" "/health" "l2-victoria-metrics"; then
        failed=1
    fi
    if ! check_and_restart "vmagent" "$VMAGENT_PORT" "/health"; then
        failed=1
    fi
    if ! check_and_restart "vmalert" "$VMALERT_PORT" "/health"; then
        failed=1
    fi
    if ! check_and_restart_health "nats-server" "$NATS_HTTP_PORT" "/healthz"; then
        failed=1
    fi

    if ! check_metrics "nats-exporter" "$NATS_EXPORTER_PORT"; then
        log_warn "nats-exporter is down (NATS metrics only, not fatal)"
    fi
    if ! check_and_restart_health "perses" "$PERSES_PORT" "/api/v1/projects" "l2-perses"; then
        log_warn "perses is down (dashboard UI only, not fatal)"
    fi

    return $failed
}

check_disk_space() {
    local avail_mb
    avail_mb=$(df -P "$DISK_CHECK_PATH" 2>/dev/null | awk 'NR==2 {print int($4/1024)}')
    if [ -z "$avail_mb" ]; then
        log_warn "Could not read free space for $DISK_CHECK_PATH (df failed)"
        return 0
    fi

    if [ "$avail_mb" -lt "$DISK_CRIT_MB" ]; then
        log_error "✗ Disk space CRITICAL: ${avail_mb} MB free on $DISK_CHECK_PATH (< ${DISK_CRIT_MB} MB)"
        return 1
    fi
    if [ "$avail_mb" -lt "$DISK_WARN_MB" ]; then
        log_warn "⚠ Disk space low: ${avail_mb} MB free on $DISK_CHECK_PATH (< ${DISK_WARN_MB} MB)"
        return 0
    fi
    log_info "✓ Disk space OK: ${avail_mb} MB free on $DISK_CHECK_PATH"
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

    # Check worker — health/ready (19093) is the source of truth; /metrics
    # (19091) stays up even when NATS is disconnected.
    if ! check_health "l2-worker" "$WORKER_HEALTH_PORT" "/health/ready"; then
        # fallback: at least metrics should respond
        if ! check_metrics "l2-worker" "$WORKER_METRICS_PORT"; then
            unhealthy_services+=("l2-worker")
            exit_code=1
        fi
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

    # Observability stack: metrics storage, collection and alerting. Without
    # this check a dead VictoriaMetrics is invisible (round 205).
    if ! check_metrics_stack; then
        unhealthy_services+=("observability")
        exit_code=1
    fi

    # Disk guard: a full filesystem killed VictoriaMetrics on 2026-10-27.
    if ! check_disk_space; then
        unhealthy_services+=("disk")
        exit_code=1
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
            if ! check_health "l2-worker" "$WORKER_HEALTH_PORT" "/health/ready"; then
                if ! check_and_restart "l2-worker" "$WORKER_METRICS_PORT" "/metrics"; then
                    unhealthy_services+=("l2-worker")
                    exit_code=1
                fi
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

        metrics|observability)
            if ! check_metrics_stack; then
                unhealthy_services+=("observability")
                exit_code=1
            fi
            ;;

        disk)
            if ! check_disk_space; then
                unhealthy_services+=("disk")
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

    if [ "$SERVICE" != "all" ] && [ "$SERVICE" != "watch" ]; then
        echo ""
        echo "=========================================="
        if [ ${#unhealthy_services[@]} -eq 0 ]; then
            log_info "Requested checks passed ($SERVICE)"
        else
            log_error "Unhealthy: ${unhealthy_services[*]}"
        fi
        echo "=========================================="
    fi

    return $exit_code
}

# Run main function
main "$@"
