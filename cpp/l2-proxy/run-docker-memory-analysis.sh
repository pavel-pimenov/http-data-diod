#!/bin/bash

# Script to run memory analysis on all services in Docker Compose environment

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== Docker Memory Analysis for l2-proxy Services ===${NC}"

# Find the project root directory
PROJECT_ROOT=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
DOCKER_COMPOSE_FILE="$PROJECT_ROOT/docker-compose.yml"

# Check if docker-compose.yml exists
if [ ! -f "$DOCKER_COMPOSE_FILE" ]; then
    echo -e "${RED}Error: docker-compose.yml not found at $DOCKER_COMPOSE_FILE${NC}"
    exit 1
fi

# Create analysis directory
ANALYSIS_DIR="$PROJECT_ROOT/docker-memory-analysis-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ANALYSIS_DIR"
echo -e "${GREEN}Created analysis directory: $ANALYSIS_DIR${NC}"

# Function to run Valgrind analysis on a service
run_service_valgrind() {
    local service_name=$1
    echo -e "${YELLOW}Running Valgrind analysis on $service_name...${NC}"

    # Run service with Valgrind
    timeout 120s docker-compose run --rm \
        -e MODE=${MODE:-proxy} \
        -e LOG_LEVEL=DEBUG \
        --entrypoint="valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --verbose --log-file=/tmp/valgrind-output.log --suppressions=/root/valgrind.supp" \
        $service_name &

    VALGRIND_PID=$!

    # Wait for a bit then send SIGTERM
    sleep 60
    kill -TERM $VALGRIND_PID 2>/dev/null || true
    wait $VALGRIND_PID 2>/dev/null || true

    # Copy Valgrind output from container
    # Note: This would need to be adapted based on how the service is actually run
    echo -e "${GREEN}Valgrind analysis for $service_name completed.${NC}"
}

# Function to run memory analysis on all services
run_all_services_analysis() {
    echo -e "${YELLOW}Running memory analysis on all services...${NC}"

    # Services to analyze
    SERVICES=("l2-proxy" "l2-worker" "l2-server")

    for service in "${SERVICES[@]}"; do
        echo -e "${BLUE}Analyzing $service...${NC}"

        # Create a temporary docker-compose override for memory analysis
        cat > "$ANALYSIS_DIR/docker-compose-override.yml" << EOF
version: '3.8'
services:
  $service:
    environment:
      - MEMORY_DEBUG=1
      - LOG_LEVEL=INFO
    volumes:
      - $PROJECT_ROOT/cpp/l2-proxy/valgrind.supp:/root/valgrind.supp:ro
      - $PROJECT_ROOT/$ANALYSIS_DIR:/memory-logs
EOF

        # Run the service with memory debugging enabled
        echo -e "${GREEN}Starting $service with memory debugging...${NC}"

        # Build the service with memory debugging
        docker-compose -f "$DOCKER_COMPOSE_FILE" -f "$ANALYSIS_DIR/docker-compose-override.yml" build $service

        # Run the service for a short time
        timeout 60s docker-compose -f "$DOCKER_COMPOSE_FILE" -f "$ANALYSIS_DIR/docker-compose-override.yml" run --rm \
            $service &

        SERVICE_PID=$!

        # Wait for a bit then send SIGTERM
        sleep 30
        kill -TERM $SERVICE_PID 2>/dev/null || true
        wait $SERVICE_PID 2>/dev/null || true

        echo -e "${GREEN}Memory analysis for $service completed.${NC}"
    done
}

# Function to run heap profiling in Docker
run_docker_heap_profiling() {
    echo -e "${YELLOW}Running heap profiling in Docker...${NC}"

    # Create override for heap profiling
    cat > "$ANALYSIS_DIR/docker-compose-heap-profile.yml" << EOF
version: '3.8'
services:
  l2-proxy:
    environment:
      - HEAPPROFILE=/memory-logs/proxy.hprof
      - HEAP_PROFILE_ALLOCATION_INTERVAL=1048576
    volumes:
      - $PROJECT_ROOT/$ANALYSIS_DIR:/memory-logs

  l2-worker:
    environment:
      - HEAPPROFILE=/memory-logs/worker.hprof
      - HEAP_PROFILE_ALLOCATION_INTERVAL=1048576
    volumes:
      - $PROJECT_ROOT/$ANALYSIS_DIR:/memory-logs

  l2-server:
    environment:
      - HEAPPROFILE=/memory-logs/server.hprof
      - HEAP_PROFILE_ALLOCATION_INTERVAL=1048576
    volumes:
      - $PROJECT_ROOT/$ANALYSIS_DIR:/memory-logs
EOF

    # Build services
    docker-compose -f "$DOCKER_COMPOSE_FILE" -f "$ANALYSIS_DIR/docker-compose-heap-profile.yml" build

    # Run services for profiling
    echo -e "${GREEN}Starting services with heap profiling...${NC}"
    timeout 60s docker-compose -f "$DOCKER_COMPOSE_FILE" -f "$ANALYSIS_DIR/docker-compose-heap-profile.yml" up -d

    # Wait for profiling
    sleep 30

    # Stop services
    docker-compose -f "$DOCKER_COMPOSE_FILE" -f "$ANALYSIS_DIR/docker-compose-heap-profile.yml" down

    echo -e "${GREEN}Heap profiling completed. Results saved to $ANALYSIS_DIR${NC}"
}

# Function to generate Docker memory analysis report
generate_docker_report() {
    echo -e "${YELLOW}Generating Docker memory analysis report...${NC}"

    {
        echo "=== Docker Memory Analysis Report ==="
        echo "Generated on: $(date)"
        echo ""

        echo "=== Services Analyzed ==="
        echo "1. l2-proxy"
        echo "2. l2-worker"
        echo "3. l2-server"
        echo ""

        echo "=== Memory Analysis Summary ==="
        echo "This analysis ran memory debugging tools on all services in the Docker environment."
        echo "The services were run with memory debugging enabled for a limited time."
        echo ""

        echo "=== Heap Profiling Results ==="
        if ls "$ANALYSIS_DIR"/*.hprof.* 1> /dev/null 2>&1; then
            echo "Heap profile files found: $(ls "$ANALYSIS_DIR"/*.hprof.* | wc -l)"
            echo "Largest profile: $(ls -la "$ANALYSIS_DIR"/*.hprof.* | sort -k5 -n | tail -1 | awk '{print $5}' 2>/dev/null || echo "N/A")"
        else
            echo "No heap profiling output found"
        fi
        echo ""

        echo "=== How to Analyze Results ==="
        echo "1. For detailed analysis, run the individual service analysis scripts:"
        echo "   - cd cpp/l2-proxy && ./run-with-valgrind.sh"
        echo "   - cd cpp/l2-proxy && ./run-with-heap-profiler.sh"
        echo ""
        echo "2. For heap profiles, use pprof:"
        echo "   pprof $PROJECT_ROOT/cpp/l2-proxy/build-profile/l2-proxy $ANALYSIS_DIR/*.hprof.*"

    } > "$ANALYSIS_DIR/docker-summary.txt"

    echo -e "${GREEN}Docker memory analysis report generated: $ANALYSIS_DIR/docker-summary.txt${NC}"
}

# Run all Docker memory analysis
run_all_services_analysis
run_docker_heap_profiling
generate_docker_report

echo -e "${BLUE}=== Docker memory analysis completed ===${NC}"
echo -e "${GREEN}Results saved to: $ANALYSIS_DIR${NC}"
echo -e "${YELLOW}To analyze heap profiles, run: pprof $PROJECT_ROOT/cpp/l2-proxy/build-profile/l2-proxy $ANALYSIS_DIR/*.hprof.*${NC}"