#!/bin/bash

# PVS-Studio static analysis runner for l2-proxy project (local mode only)
#
# Usage:
#   ./run-pvs-studio.sh                    # Run analysis
#   ./run-pvs-studio.sh --clean            # Clean build dir and reconfigure
#
# Prerequisites:
#   - PVS-Studio installed locally with valid license
#
# Results:
#   - Reports are saved to ./reports/ directory
#   - Console output shows detected issues
#
# More info: https://pvs-studio.ru/ru/docs/manual/6591/

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPORTS_DIR="${SCRIPT_DIR}/reports"
BUILD_DIR="${SCRIPT_DIR}/cpp/l2-proxy/build-pvs"

# Parse arguments
CLEAN_BUILD=false
for arg in "$@"; do
    case "$arg" in
        --clean)
            CLEAN_BUILD=true
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --clean     Clean build directory and reconfigure"
            echo "  --help, -h  Show this help"
            echo ""
            echo "Prerequisites:"
            echo "  - PVS-Studio installed locally (pvs-studio-analyzer, plog-converter)"
            echo "  - Valid PVS-Studio license"
            exit 0
            ;;
    esac
done

# Check for pvs-studio-analyzer
if ! command -v pvs-studio-analyzer &> /dev/null; then
    echo "ERROR: pvs-studio-analyzer not found in PATH"
    echo "Install PVS-Studio: https://pvs-studio.ru/ru/pvs-studio/download/"
    exit 1
fi

# Check for plog-converter
if ! command -v plog-converter &> /dev/null; then
    echo "ERROR: plog-converter not found in PATH"
    echo "Install PVS-Studio: https://pvs-studio.ru/ru/pvs-studio/download/"
    exit 1
fi

echo "=== PVS-Studio analysis for l2-proxy ==="
echo ""

# Clean build directory if requested
if [ "$CLEAN_BUILD" = true ]; then
    echo "Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
fi

# Create build directory
mkdir -p "${BUILD_DIR}"

# Configure CMake with PVS-Studio enabled
echo "Configuring CMake..."
cd "${BUILD_DIR}"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_PVS_STUDIO=ON \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      ..

# Run PVS-Studio analysis
echo ""
echo "Running PVS-Studio analysis..."
cmake --build . --target l2-proxy.pvs

# Convert report
mkdir -p "${REPORTS_DIR}"
echo ""
echo "Converting analysis report..."
plog-converter \
    -r "${SCRIPT_DIR}/cpp/l2-proxy" \
    -t tasklist \
    -o "${REPORTS_DIR}/pvs-studio-results" \
    "${BUILD_DIR}/pvs-studio.log"

echo ""
echo "=== Analysis complete ==="
echo "Results: ${REPORTS_DIR}/pvs-studio-results.csv"
echo "Raw log: ${BUILD_DIR}/pvs-studio.log"
