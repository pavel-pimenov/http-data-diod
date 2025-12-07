#!/bin/bash

# Benchmark script for load testing l2-proxy on port 8888 using Apache Bench (ab)

# Default values
DEFAULT_URL="http://localhost:8888/"
DEFAULT_REQUESTS=100
DEFAULT_CONCURRENT=100

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to print usage
usage() {
    echo "Usage: $0 [OPTIONS]"
    echo "Benchmark l2-proxy on port 8888 using Apache Bench"
    echo ""
    echo "Options:"
    echo "  -u, --url URL          Target URL (default: $DEFAULT_URL)"
    echo "  -n, --requests NUM     Number of requests (default: $DEFAULT_REQUESTS)"
    echo "  -c, --concurrent NUM   Number of concurrent requests (default: $DEFAULT_CONCURRENT)"
    echo "  -h, --help            Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0"
    echo "  $0 -n 500 -c 20"
    echo "  $0 --url http://localhost:8888/health --requests 100"
    exit 1
}

# Parse command line arguments
URL="$DEFAULT_URL"
REQUESTS="$DEFAULT_REQUESTS"
CONCURRENT="$DEFAULT_CONCURRENT"

while [[ $# -gt 0 ]]; do
    case $1 in
        -u|--url)
            URL="$2"
            shift 2
            ;;
        -n|--requests)
            REQUESTS="$2"
            shift 2
            ;;
        -c|--concurrent)
            CONCURRENT="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
done

# Check if ab is installed
if ! command -v ab &> /dev/null; then
    echo -e "${RED}Error: Apache Bench (ab) is not installed.${NC}"
    echo "Install it with: sudo apt-get install apache2-utils"
    exit 1
fi

# Print benchmark configuration
echo -e "${GREEN}Starting benchmark with Apache Bench...${NC}"
echo "URL: $URL"
echo "Total requests: $REQUESTS"
echo "Concurrent requests: $CONCURRENT"
echo "----------------------------------------"

if [ ! -f "test_payload.json" ]; then
    echo -e "${RED}Error: test_payload.json not found in current directory.${NC}"
    exit 1
fi

# Run Apache Bench
echo "Running: ab -n $REQUESTS -c $CONCURRENT -p test_payload.json -T application/json $URL"
echo ""

ab -n "$REQUESTS" -c "$CONCURRENT" -p test_payload.json -T application/json "$URL"

# Check the exit code
AB_EXIT_CODE=$?

if [ $AB_EXIT_CODE -eq 0 ]; then
    echo -e "\n${GREEN}Benchmark completed successfully!${NC}"

else
    echo -e "\n${RED}Benchmark failed with exit code $AB_EXIT_CODE${NC}"
    exit 1
fi

echo "----------------------------------------"
