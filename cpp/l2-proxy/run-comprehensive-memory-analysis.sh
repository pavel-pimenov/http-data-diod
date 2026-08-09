ize#!/bin/bash

# Comprehensive memory analysis script
# Runs multiple memory analysis tools and generates reports

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== Comprehensive Memory Analysis for l2-proxy ===${NC}"

# Check if we're in the right directory
if [ ! -f "CMakeLists.txt" ]; then
    echo -e "${RED}Error: CMakeLists.txt not found. Please run this script from the cpp/l2-proxy directory.${NC}"
    exit 1
fi

# Create analysis directory
ANALYSIS_DIR="./memory-analysis-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ANALYSIS_DIR"
echo -e "${GREEN}Created analysis directory: $ANALYSIS_DIR${NC}"

# Function to run Valgrind analysis
run_valgrind_analysis() {
    echo -e "${YELLOW}Running Valgrind memory analysis...${NC}"

    # Build debug version
    mkdir -p build-debug
    cd build-debug
    cmake -DCMAKE_BUILD_TYPE=Debug -DCPPHTTPLIB_OPENSSL_SUPPORT=ON ..
    make l2-proxy -j2
    cd ..

    # Run with Valgrind
    echo -e "${GREEN}Running application with Valgrind...${NC}"
    timeout 60s valgrind \
        --leak-check=full \
        --show-leak-kinds=all \
        --track-origins=yes \
        --verbose \
        --suppressions=valgrind.supp \
        --log-file="$ANALYSIS_DIR/valgrind-output.log" \
        ./build-debug/l2-proxy &

    VALGRIND_PID=$!

    # Wait for a bit then send SIGTERM
    sleep 30
    kill -TERM $VALGRIND_PID 2>/dev/null || true
    wait $VALGRIND_PID 2>/dev/null || true

    echo -e "${GREEN}Valgrind analysis completed. Output saved to $ANALYSIS_DIR/valgrind-output.log${NC}"
}

# Function to run AddressSanitizer analysis
run_asan_analysis() {
    echo -e "${YELLOW}Running AddressSanitizer analysis...${NC}"

    # Build with AddressSanitizer
    mkdir -p build-asan
    cd build-asan
    cmake -DCMAKE_BUILD_TYPE=Debug -DCPPHTTPLIB_OPENSSL_SUPPORT=ON ..
    make l2-proxy-debug -j2
    cd ..

    # Run with ASan
    echo -e "${GREEN}Running application with AddressSanitizer...${NC}"
    export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:log_path=$ANALYSIS_DIR/asan"

    timeout 60s ./build-asan/l2-proxy-debug &

    ASAN_PID=$!

    # Wait for a bit then send SIGTERM
    sleep 30
    kill -TERM $ASAN_PID 2>/dev/null || true
    wait $ASAN_PID 2>/dev/null || true

    echo -e "${GREEN}AddressSanitizer analysis completed. Output saved to $ANALYSIS_DIR/asan.*${NC}"
}

# Function to run heap profiling
run_heap_profiling() {
    echo -e "${YELLOW}Running heap profiling...${NC}"

    # Build release version with debug info
    mkdir -p build-profile
    cd build-profile
    cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCPPHTTPLIB_OPENSSL_SUPPORT=ON ..
    cd ..

    # Run with heap profiling
    echo -e "${GREEN}Running application with heap profiling...${NC}"
    export LD_PRELOAD="/usr/lib/x86_64-linux-gnu/libprofiler.so.0"
    export HEAPPROFILE="$ANALYSIS_DIR/l2-proxy.hprof"
    export HEAP_PROFILE_ALLOCATION_INTERVAL=1048576
    export HEAP_PROFILE_INUSE_INTERVAL=1048576

    timeout 60s ./build-profile/l2-proxy &

    HEAP_PID=$!

    # Wait for a bit then send SIGTERM
    sleep 30
    kill -TERM $HEAP_PID 2>/dev/null || true
    wait $HEAP_PID 2>/dev/null || true

    echo -e "${GREEN}Heap profiling completed. Output saved to $ANALYSIS_DIR/l2-proxy.hprof.*${NC}"
}

# Function to generate summary report
generate_summary() {
    echo -e "${YELLOW}Generating summary report...${NC}"

    {
        echo "=== Memory Analysis Summary Report ==="
        echo "Generated on: $(date)"
        echo ""

        echo "=== System Information ==="
        echo "OS: $(uname -s)"
        echo "Architecture: $(uname -m)"
        echo ""

        echo "=== Valgrind Summary ==="
        if [ -f "$ANALYSIS_DIR/valgrind-output.log" ]; then
            echo "Valgrind log file: $ANALYSIS_DIR/valgrind-output.log"
            echo "Errors detected: $(grep -c "ERROR SUMMARY" "$ANALYSIS_DIR/valgrind-output.log" 2>/dev/null || echo 0)"
            echo "Leaks detected: $(grep -c "definitely lost" "$ANALYSIS_DIR/valgrind-output.log" 2>/dev/null || echo 0)"
        else
            echo "No Valgrind output found"
        fi
        echo ""

        echo "=== AddressSanitizer Summary ==="
        if ls "$ANALYSIS_DIR"/asan.* 1> /dev/null 2>&1; then
            echo "ASan log files found: $(ls "$ANALYSIS_DIR"/asan.* | wc -l)"
            echo "Errors detected: $(grep -c "ERROR" "$ANALYSIS_DIR"/asan.* 2>/dev/null || echo 0)"
        else
            echo "No AddressSanitizer output found"
        fi
        echo ""

        echo "=== Heap Profiling Summary ==="
        if ls "$ANALYSIS_DIR"/l2-proxy.hprof.* 1> /dev/null 2>&1; then
            echo "Heap profile files found: $(ls "$ANALYSIS_DIR"/l2-proxy.hprof.* | wc -l)"
            echo "Largest profile: $(ls -la "$ANALYSIS_DIR"/l2-proxy.hprof.* | sort -k5 -n | tail -1 | awk '{print $5}')"
        else
            echo "No heap profiling output found"
        fi
        echo ""

        echo "=== Recommendations ==="
        echo "1. Review Valgrind output for memory leaks"
        echo "2. Check AddressSanitizer logs for memory errors"
        echo "3. Analyze heap profiles with: pprof ./build-profile/l2-proxy $ANALYSIS_DIR/l2-proxy.hprof.*"
        echo "4. For detailed Valgrind analysis: valgrind --tool=massif ./build-debug/l2-proxy"

    } > "$ANALYSIS_DIR/summary.txt"

    echo -e "${GREEN}Summary report generated: $ANALYSIS_DIR/summary.txt${NC}"
}

# Run all analyses
run_valgrind_analysis
run_asan_analysis
run_heap_profiling
generate_summary

echo -e "${BLUE}=== Memory analysis completed ===${NC}"
echo -e "${GREEN}Results saved to: $ANALYSIS_DIR${NC}"
echo -e "${YELLOW}To analyze heap profiles, run: pprof ./build-profile/l2-proxy $ANALYSIS_DIR/l2-proxy.hprof.*${NC}"