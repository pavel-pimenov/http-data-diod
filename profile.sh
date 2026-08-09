#!/bin/bash
#
# Profiling script for L2 Proxy
# Uses perf + FlameGraph for CPU profiling
#
# Usage:
#   ./profile.sh [service] [duration_seconds]
#
# Examples:
#   ./profile.sh l2-proxy 30    # Profile proxy for 30 seconds
#   ./profile.sh l2-worker 30   # Profile worker for 30 seconds
#   ./profile.sh all 30                 # Profile all services
#
# Requirements:
#   - sudo apt-get install linux-perf binutils
#   - git clone https://github.com/brendangregg/FlameGraph (optional, for SVG generation)
#   - sudo sysctl -w kernel.perf_event_paranoid=1 (for full profiling)
#

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROFILE_DIR="${SCRIPT_DIR}/profiles"
FLAMEGRAPH_DIR="${PROFILE_DIR}/flamegraph"
COMPOSE_FILE="${SCRIPT_DIR}/docker-compose.yml"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values
DURATION=${2:-30}
SERVICES=${1:-l2-proxy}

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Create profile directory
mkdir -p "${PROFILE_DIR}"
mkdir -p "${FLAMEGRAPH_DIR}"

# Check if perf is available on host
check_perf() {
    if ! command -v perf &> /dev/null; then
        log_error "perf is not installed. Install with: sudo apt-get install linux-perf"
        exit 1
    fi
    log_info "perf found: $(which perf)"

    # Check for FlameGraph tools
    local has_flamegraph=false
    if command -v stackcollapse-perf.pl &> /dev/null; then
        log_info "FlameGraph found: $(which stackcollapse-perf.pl)"
        has_flamegraph=true
    elif command -v flamegraph &> /dev/null; then
        log_info "flamegraph binary found: $(which flamegraph)"
        has_flamegraph=true
    fi

    if [ "$has_flamegraph" = false ]; then
        log_warning "FlameGraph tools not found. Options:"
        log_warning "  1. Install from apt (if available): sudo apt-get install flamegraph"
        log_warning "  2. Use online generator: https://www.speedscope.app/"
        log_warning "  3. Use perf report: perf report -i profiles/*/perf.data"
    fi

    # Check perf_event_paranoid
    local paranoid_level=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "unknown")
    log_info "perf_event_paranoid level: ${paranoid_level}"

    if [ "${paranoid_level}" != "unknown" ] && [ "${paranoid_level}" -gt 1 ]; then
        log_warning "perf_event_paranoid > 1, profiling may be limited"
        log_warning "To enable full profiling, run: sudo sysctl -w kernel.perf_event_paranoid=1"
    fi
}

# Check Docker container status
check_container() {
    local service=$1
    local container_id=$(docker compose -f "${COMPOSE_FILE}" ps -q "${service}" 2>/dev/null)

    if [ -z "${container_id}" ]; then
        log_error "Container for service '${service}' not found. Is it running?"
        return 1
    fi

    echo "${container_id}"
}

# Profile a single service
profile_service() {
    local service=$1
    local duration=$2

    log_info "Profiling service: ${service} for ${duration} seconds..."

    # Get container ID
    local container_id=$(check_container "${service}")
    if [ $? -ne 0 ]; then
        return 1
    fi

    # Get container PID
    local container_pid=$(docker inspect --format '{{.State.Pid}}' "${container_id}" 2>/dev/null)
    if [ -z "${container_pid}" ] || [ "${container_pid}" == "0" ]; then
        log_error "Could not get PID for container ${container_id}"
        return 1
    fi

    log_info "Container PID: ${container_pid}"

    # Create output directory for this service
    local service_profile_dir="${PROFILE_DIR}/${service}_$(date +%Y%m%d_%H%M%S)"
    mkdir -p "${service_profile_dir}"

    # Record perf data using host perf with container PID
    log_info "Recording perf data for ${duration} seconds..."
    log_info "Using host perf with container namespace (PID: ${container_pid})"

    # Run perf on host targeting container PID
    sudo perf record -F 199 -p "${container_pid}" -g -- sleep "${duration}" || {
        log_error "perf record failed."
        log_error "Try: sudo sysctl -w kernel.perf_event_paranoid=1"
        return 1
    }

    # Move perf.data to profile directory
    if [ -f "./perf.data" ]; then
        mv ./perf.data "${service_profile_dir}/"
    fi

    # Generate report
    if [ -f "${service_profile_dir}/perf.data" ]; then
        log_success "Perf data saved to: ${service_profile_dir}/perf.data"

        # Generate text report
        log_info "Generating text report..."
        sudo perf report -i "${service_profile_dir}/perf.data" --stdio 2>/dev/null | head -100 > "${service_profile_dir}/report.txt" || true

        # Generate flamegraph if tools are available
        if command -v stackcollapse-perf.pl &> /dev/null && command -v flamegraph.pl &> /dev/null; then
            log_info "Generating FlameGraph SVG..."
            sudo perf script -i "${service_profile_dir}/perf.data" 2>/dev/null | \
                stackcollapse-perf.pl 2>/dev/null | \
                flamegraph.pl > "${service_profile_dir}/flamegraph.svg" 2>/dev/null || {
                    log_warning "Could not generate FlameGraph SVG"
                }
            log_success "FlameGraph SVG saved to: ${service_profile_dir}/flamegraph.svg"
        elif command -v perf &> /dev/null; then
            # Generate folded format for online viewers
            log_info "Generating folded format for online viewers..."
            sudo perf script -i "${service_profile_dir}/perf.data" 2>/dev/null | \
                awk 'BEGIN {p=0} /^[^ ]/ {if (p) print s; s=$0; p=1; next} {if (p) s=s";"$2; else s=$2; p=1} END {if (p) print s}' | \
                sort | uniq -c | awk '{print $2" "$1}' > "${service_profile_dir}/folded.txt" 2>/dev/null || true

            if [ -f "${service_profile_dir}/folded.txt" ]; then
                log_success "Folded format saved to: ${service_profile_dir}/folded.txt"
                log_info "Upload to https://www.speedscope.app/ for interactive visualization"
            fi
        fi

        # Show top 20 functions
        echo ""
        log_info "Top 20 functions by CPU time:"
        echo "========================================"
        sudo perf report -i "${service_profile_dir}/perf.data" --stdio --no-children 2>/dev/null | \
            grep -E "^\s+[0-9]+\.[0-9]+%" | head -20 || true
        echo "========================================"

    else
        log_error "Failed to generate perf.data"
        return 1
    fi

    log_success "Profiling complete for ${service}!"
    log_info "Results saved to: ${service_profile_dir}"

    return 0
}

# Main
main() {
    log_info "=== L2 Proxy Profiler ==="
    log_info "Duration: ${DURATION} seconds"
    log_info "Services: ${SERVICES}"
    echo ""

    check_perf

    if [ "${SERVICES}" == "all" ]; then
        # Profile all services
        profile_service "l2-proxy" "${DURATION}"
        echo ""
        profile_service "l2-worker" "${DURATION}"
        echo ""
        profile_service "l2-server" "${DURATION}"
    else
        # Profile specific service
        profile_service "${SERVICES}" "${DURATION}"
    fi

    echo ""
    log_success "=== Profiling Complete ==="
    log_info "Profile directory: ${PROFILE_DIR}"
    echo ""
    log_info "To view FlameGraph:"
    log_info "  firefox ${PROFILE_DIR}/*/flamegraph.svg"
    echo ""
    log_info "To analyze perf data:"
    log_info "  perf report -i ${PROFILE_DIR}/*/perf.data"
}

# Run main
main "$@"
