#!/bin/bash
# Performance Regression Test Script
# Runs performance benchmarks and compares against baseline
#
# Usage:
#   ./scripts/performance-regression-test.sh [baseline_file]
#
# If baseline_file is provided, compares current results against it.
# Otherwise, creates a new baseline.

set -e

# Configuration
URL="${URL:-http://localhost:7777}"
ITERATIONS="${ITERATIONS:-100}"
CONCURRENT="${CONCURRENT:-10}"
BODY_SIZE="${BODY_SIZE:-0.01}"  # MB
LATENCY_THRESHOLD_P95="${LATENCY_THRESHOLD_P95:-500}"  # ms
LATENCY_THRESHOLD_P99="${LATENCY_THRESHOLD_P99:-1000}"  # ms
THROUGHPUT_MIN="${THROUGHPUT_MIN:-50}"  # requests per second

# Output file
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_DIR="${RESULTS_DIR:-./performance-results}"
mkdir -p "$RESULTS_DIR"
RESULTS_FILE="$RESULTS_DIR/performance_$TIMESTAMP.json"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=========================================="
echo "🚀 Performance Regression Test"
echo "=========================================="
echo "URL: $URL"
echo "Iterations: $ITERATIONS"
echo "Concurrent: $CONCURRENT"
echo "Body Size: ${BODY_SIZE}MB"
echo "=========================================="

# Run the performance test using message_counter.py
echo ""
echo "Running performance test..."
python3 message_counter.py \
    --url "$URL" \
    --iterations "$ITERATIONS" \
    --concurrent "$CONCURRENT" \
    --body-size "$BODY_SIZE" \
    --output-json "$RESULTS_FILE" 2>&1 | tee "$RESULTS_DIR/test_output_$TIMESTAMP.txt"

# Parse results
if [ ! -f "$RESULTS_FILE" ]; then
    echo -e "${RED}❌ Error: Results file not created${NC}"
    exit 1
fi

# Extract metrics using Python
python3 << EOF
import json
import sys

with open("$RESULTS_FILE", "r") as f:
    results = json.load(f)

# Extract key metrics
total_requests = results.get("total_requests", 0)
successful_requests = results.get("successful_requests", 0)
failed_requests = results.get("failed_requests", 0)
total_time_seconds = results.get("total_time_seconds", 1)
requests_per_second = results.get("requests_per_second", 0)

latency_p50_ms = results.get("latency_p50_ms", 0)
latency_p95_ms = results.get("latency_p95_ms", 0)
latency_p99_ms = results.get("latency_p99_ms", 0)

# Check thresholds
errors = []

if latency_p95_ms > $LATENCY_THRESHOLD_P95:
    errors.append(f"P95 latency {latency_p95_ms:.2f}ms exceeds threshold {$LATENCY_THRESHOLD_P95}ms")

if latency_p99_ms > $LATENCY_THRESHOLD_P99:
    errors.append(f"P99 latency {latency_p99_ms:.2f}ms exceeds threshold {$LATENCY_THRESHOLD_P99}ms")

if requests_per_second < $THROUGHPUT_MIN:
    errors.append(f"Throughput {requests_per_second:.2f} req/s below minimum {$THROUGHPUT_MIN} req/s")

if failed_requests > 0:
    error_rate = (failed_requests / total_requests) * 100 if total_requests > 0 else 0
    if error_rate > 1.0:  # Allow up to 1% errors
        errors.append(f"Error rate {error_rate:.2f}% exceeds 1% threshold")

# Print results
print("")
print("=" * 50)
print("📊 Performance Results")
print("=" * 50)
print(f"Total Requests:      {total_requests}")
print(f"Successful:          {successful_requests}")
print(f"Failed:              {failed_requests}")
print(f"Total Time:          {total_time_seconds:.2f}s")
print(f"Throughput:          {requests_per_second:.2f} req/s")
print(f"Latency P50:         {latency_p50_ms:.2f}ms")
print(f"Latency P95:         {latency_p95_ms:.2f}ms")
print(f"Latency P99:         {latency_p99_ms:.2f}ms")
print("=" * 50)

if errors:
    print("")
    print("❌ REGRESSION DETECTED:")
    for error in errors:
        print(f"  - {error}")
    sys.exit(1)
else:
    print("")
    print("✅ All performance thresholds passed!")
    sys.exit(0)
EOF

TEST_RESULT=$?

# Compare with baseline if provided
BASELINE_FILE="$1"
if [ -n "$BASELINE_FILE" ] && [ -f "$BASELINE_FILE" ]; then
    echo ""
    echo "=========================================="
    echo "📈 Comparing with baseline..."
    echo "=========================================="

    python3 << EOF
import json

with open("$RESULTS_FILE", "r") as f:
    current = json.load(f)

with open("$BASELINE_FILE", "r") as f:
    baseline = json.load(f)

def calc_change(current, baseline):
    if baseline == 0:
        return 0
    return ((current - baseline) / baseline) * 100

print(f"{'Metric':<20} {'Baseline':<15} {'Current':<15} {'Change':<15}")
print("-" * 65)

metrics = [
    ("Throughput (req/s)", current.get("requests_per_second", 0), baseline.get("requests_per_second", 0), True),
    ("Latency P50 (ms)", current.get("latency_p50_ms", 0), baseline.get("latency_p50_ms", 0), False),
    ("Latency P95 (ms)", current.get("latency_p95_ms", 0), baseline.get("latency_p95_ms", 0), False),
    ("Latency P99 (ms)", current.get("latency_p99_ms", 0), baseline.get("latency_p99_ms", 0), False),
]

for name, curr_val, base_val, higher_is_better in metrics:
    change = calc_change(curr_val, base_val)
    if higher_is_better:
        status = "✅" if change >= -5 else "⚠️"  # Allow 5% degradation
    else:
        status = "✅" if change <= 5 else "⚠️"  # Allow 5% increase

    change_str = f"{change:+.2f}%"
    print(f"{name:<20} {base_val:<15.2f} {curr_val:<15.2f} {status} {change_str:<15}")

EOF
fi

echo ""
echo "Results saved to: $RESULTS_FILE"
echo "=========================================="

if [ $TEST_RESULT -eq 0 ]; then
    echo -e "${GREEN}✅ Performance test PASSED${NC}"
else
    echo -e "${RED}❌ Performance test FAILED${NC}"
    exit 1
fi
