#!/bin/bash
# Memory leak test using Valgrind
# This script rebuilds the l2-proxy with debug symbols and runs valgrind

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/cpp/l2-proxy/build"
VALGRIND_LOG="${SCRIPT_DIR}/valgrind-output.log"

echo "============================================================"
echo "Memory Leak Test with Valgrind"
echo "============================================================"

# Check if valgrind is available
if ! command -v valgrind &> /dev/null; then
    echo "ERROR: valgrind not found. Install with: sudo apt install valgrind"
    exit 1
fi

# Rebuild with debug symbols
echo ""
echo "[1/4] Rebuilding l2-proxy with debug symbols..."
cd "${SCRIPT_DIR}/cpp/l2-proxy"

# Create build directory if it doesn't exist
mkdir -p build
cd build

# Build with debug info
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++" ..
make -j$(nproc)

echo "✓ Build complete"

# Run valgrind
echo ""
echo "[2/4] Running valgrind memory check..."
cd "${SCRIPT_DIR}"

# Set up environment similar to docker
export MODE=proxy
export PROXY_PORT=19999  # Use different port to avoid conflicts
export LOG_LEVEL=INFO
export LOG_FORMAT=json
export NUM_THREADS=4
export HTTP_POOL_SIZE=10
export REQUEST_TIMEOUT_SECONDS=5
export JAEGER_URL=http://localhost:9411/api/v2/spans
export ENABLE_TRACING=false

# Run valgrind with timeout (proxy will run for 10 seconds then exit)
timeout 15 valgrind \
    --leak-check=full \
    --show-leak-kinds=all \
    --track-origins=yes \
    --verbose \
    --log-file="${VALGRIND_LOG}" \
    --error-exitcode=42 \
    "${BUILD_DIR}/l2-proxy" 2>&1 | head -100 &

VALGRIND_PID=$!
echo "Valgrind running (PID: ${VALGRIND_PID})"

# Wait for valgrind to start
sleep 5

# Send some test requests to trigger memory allocations
echo ""
echo "[3/4] Sending test requests to trigger allocations..."
for i in $(seq 1 10); do
    curl -s -X POST http://localhost:19999/ \
        -H "Content-Type: application/json" \
        -d "{\"value\": $i, \"test\": \"memory_leak_test_$i\"}" > /dev/null 2>&1 || true
done

# Wait for valgrind to finish
wait $VALGRIND_PID 2>/dev/null || true

# Analyze results
echo ""
echo "[4/4] Analyzing valgrind results..."
echo ""

if [ -f "${VALGRIND_LOG}" ]; then
    # Extract summary
    echo "============================================================"
    echo "Valgrind Memory Check Results"
    echo "============================================================"
    echo ""

    # Show leak summary
    if grep -q "definitely lost" "${VALGRIND_LOG}"; then
        echo "❌ MEMORY LEAKS DETECTED!"
        echo ""
        grep -A 2 "LEAK SUMMARY" "${VALGRIND_LOG}" || true
        echo ""
        grep "definitely lost" "${VALGRIND_LOG}" || true
        grep "indirectly lost" "${VALGRIND_LOG}" || true
        grep "possibly lost" "${VALGRIND_LOG}" || true
        grep "still reachable" "${VALGRIND_LOG}" || true
        echo ""
        echo "Full log: ${VALGRIND_LOG}"
        echo ""
        echo "❌ FAILED: Memory leaks found"
        exit 1
    else
        echo "✅ No memory leaks detected!"
        echo ""
        if grep -q "All heap blocks were freed" "${VALGRIND_LOG}"; then
            echo "✓ All heap blocks were freed - no leaks are possible"
        elif grep -q "no leaks are possible" "${VALGRIND_LOG}"; then
            echo "✓ No leaks are possible"
        fi
        echo ""

        # Show allocation stats
        echo "Allocation statistics:"
        grep "total heap usage" "${VALGRIND_LOG}" || true
        echo ""
        echo "✅ PASSED: No memory leaks"
        exit 0
    fi
else
    echo "ERROR: Valgrind log file not found: ${VALGRIND_LOG}"
    exit 1
fi
