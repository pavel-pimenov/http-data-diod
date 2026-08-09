#!/bin/bash
# Run load test with memory leak detection
# Supports two modes:
#   1. Normal mode: just run the load test against running containers
#   2. ASan mode: rebuild with AddressSanitizer + LeakSanitizer, then run test
#
# Usage:
#   ./run-load-test.sh                    # Normal mode, 1 hour
#   ./run-load-test.sh --asan             # ASan mode, 1 hour
#   ./run-load-test.sh --asan --duration 1800  # ASan mode, 30 min
#   ./run-load-test.sh --duration 600 --concurrent 50  # 10 min, 50 workers

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

ENABLE_ASAN=false
DURATION=3600
CONCURRENT=20
PAYLOAD_SIZE=10
CONTAINERS="l2-proxy l2-worker"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --asan)
            ENABLE_ASAN=true
            shift
            ;;
        --duration)
            DURATION="$2"
            shift 2
            ;;
        --concurrent)
            CONCURRENT="$2"
            shift 2
            ;;
        --payload-size)
            PAYLOAD_SIZE="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

echo "============================================================"
echo "Load Test with Memory Leak Detection"
echo "============================================================"
echo ""
echo "Duration:    ${DURATION}s ($((DURATION / 60)) min)"
echo "Concurrent:  ${CONCURRENT} workers"
echo "Payload:     ${PAYLOAD_SIZE}KB"
echo "ASan mode:   ${ENABLE_ASAN}"
echo ""

# Step 1: Rebuild with ASan if requested
if [ "$ENABLE_ASAN" = true ]; then
    echo -e "${YELLOW}[1/3] Rebuilding with AddressSanitizer + LeakSanitizer...${NC}"
    ./rebuild-and-run.sh --asan
    echo ""
    echo -e "${GREEN}[1/3] ASan build complete${NC}"
    sleep 10
else
    echo -e "${YELLOW}[1/3] Checking if services are running...${NC}"
    if ! curl -sf --connect-timeout 3 "http://localhost:7777" > /dev/null 2>&1; then
        echo -e "${RED}Services not running. Starting...${NC}"
        ./rebuild-and-run.sh
        sleep 10
    fi
    echo -e "${GREEN}[1/3] Services are running${NC}"
fi

# Step 2: Install dependencies if needed
echo ""
echo -e "${YELLOW}[2/3] Checking Python dependencies...${NC}"
pip3 install aiohttp psutil --quiet 2>/dev/null || true
echo -e "${GREEN}[2/3] Dependencies ready${NC}"

# Step 3: Run load test
echo ""
echo -e "${YELLOW}[3/3] Starting load test...${NC}"
echo ""

python3 load_test_memory.py \
    --url "http://localhost:7777" \
    --duration "$DURATION" \
    --concurrent "$CONCURRENT" \
    --payload-size "$PAYLOAD_SIZE" \
    --memory-interval 30 \
    --containers $CONTAINERS \
    --report "load_test_report.json"

EXIT_CODE=$?

echo ""
if [ "$ENABLE_ASAN" = true ]; then
    echo "ASan/LSan logs:"
    echo "  ls -lah docker-memory-analysis/"
    echo "  grep -R 'SUMMARY: AddressSanitizer\|LeakSanitizer' docker-memory-analysis/"
fi

exit $EXIT_CODE
