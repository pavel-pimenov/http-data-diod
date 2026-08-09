#!/bin/bash
# Simple unit test runner for C++ components
# Runs all tests and reports results

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=========================================="
echo "HTTP Data DIOD Unit Tests"
echo "=========================================="
echo ""

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

PASSED=0
FAILED=0

# Parse command line arguments
TEST_FILTER=""
VERBOSE=0

while [[ $# -gt 0 ]]; do
    case $1 in
        -f|--filter)
            TEST_FILTER="$2"
            shift 2
            ;;
        -v|--verbose)
            VERBOSE=1
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  -f, --filter PATTERN   Run only tests matching PATTERN"
            echo "  -v, --verbose          Show verbose output"
            echo "  -h, --help             Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0                     # Run all tests"
            echo "  $0 -f circuit-breaker  # Run only circuit breaker tests"
            echo "  $0 -v                  # Run with verbose output"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use -h for help"
            exit 1
            ;;
    esac
done

# Build test binaries if needed
if [ ! -f "build_tests/test_components" ]; then
    echo "Building test binaries..."
    mkdir -p build_tests
    cd build_tests
    cmake .. -DBUILD_TESTS=ON
    make -j$(nproc) test_components
    cd ..
fi

echo ""
echo "Running tests..."
echo ""

# Run component tests
if [ -n "$TEST_FILTER" ]; then
    echo "Running tests matching filter: $TEST_FILTER"
    if [ $VERBOSE -eq 1 ]; then
        ./build_tests/test_components "[${TEST_FILTER}]" -s
    else
        ./build_tests/test_components "[${TEST_FILTER}]"
    fi
else
    if [ $VERBOSE -eq 1 ]; then
        ./build_tests/test_components -s
    else
        ./build_tests/test_components
    fi
fi

RESULT=$?

echo ""
echo "=========================================="
echo "Test Summary"
echo "=========================================="

if [ $RESULT -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed!${NC}"
    exit 1
fi
