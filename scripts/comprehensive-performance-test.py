#!/usr/bin/env python3
"""
Comprehensive Performance Test Suite
Tests multiple scenarios to evaluate system performance.

Real latency percentiles (p50/p95/p99/avg/min/max) are measured client-side by
message_counter.py (not estimated from RPS). A machine-readable JSON report is
written to scripts/perf-report.json for regression tracking.
"""

import subprocess
import time
import json
import os
from typing import List
from dataclasses import dataclass, asdict

REPORT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "perf-report.json")

@dataclass
class TestResult:
    name: str
    iterations: int
    concurrent: int
    rps: float
    avg_latency_ms: float
    p50_latency_ms: float
    p95_latency_ms: float
    p99_latency_ms: float
    min_latency_ms: float
    max_latency_ms: float
    success_rate: float
    errors: int

def parse_float(lines: List[str], prefix: str) -> float:
    value = 0.0
    for line in lines:
        if prefix in line:
            try:
                value = float(line.split(':')[-1].strip().split()[0])
            except (ValueError, IndexError):
                continue
    return value

def run_test(name: str, iterations: int, concurrent: int) -> TestResult:
    """Run a single test scenario"""
    print(f"\n{'='*60}")
    print(f"Test: {name}")
    print(f"   Iterations: {iterations}, Concurrent: {concurrent}")
    print(f"{'='*60}")

    cmd = [
        'python3', 'message_counter.py',
        '--url', 'http://localhost:7777',
        '--iterations', str(iterations),
        '--concurrent', str(concurrent)
    ]

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)

    # Parse output
    output = result.stdout + result.stderr
    lines = output.split('\n')

    rps = parse_float(lines, 'Requests per second:')
    success = int(parse_float(lines, 'Successful requests:'))
    failed = int(parse_float(lines, 'Failed requests:'))
    p50 = parse_float(lines, 'Latency p50:')
    p95 = parse_float(lines, 'Latency p95:')
    p99 = parse_float(lines, 'Latency p99:')
    avg_latency = parse_float(lines, 'Latency avg:')
    min_latency = parse_float(lines, 'Latency min:')
    max_latency = parse_float(lines, 'Latency max:')

    total = success + failed
    success_rate = (success / total * 100) if total > 0 else 0

    print(f"   RPS: {rps:.2f}")
    print(f"   Success: {success}/{total} ({success_rate:.1f}%)")
    print(f"   Avg latency: {avg_latency:.2f}ms  "
          f"p50: {p50:.2f}ms  p95: {p95:.2f}ms  p99: {p99:.2f}ms  "
          f"max: {max_latency:.2f}ms")

    return TestResult(
        name=name,
        iterations=iterations,
        concurrent=concurrent,
        rps=rps,
        avg_latency_ms=avg_latency,
        p50_latency_ms=p50,
        p95_latency_ms=p95,
        p99_latency_ms=p99,
        min_latency_ms=min_latency,
        max_latency_ms=max_latency,
        success_rate=success_rate,
        errors=failed
    )

def main():
    print("🚀 Comprehensive Performance Test Suite")
    print("="*60)

    tests = []

    # Test 1: Low load
    tests.append(run_test("Low Load", 20, 5))
    time.sleep(2)

    # Test 2: Medium load
    tests.append(run_test("Medium Load", 50, 10))
    time.sleep(2)

    # Test 3: High load
    tests.append(run_test("High Load", 100, 20))
    time.sleep(2)

    # Test 4: Stress test
    tests.append(run_test("Stress Test", 200, 50))
    time.sleep(2)

    # Print summary table
    print("\n" + "="*80)
    print("PERFORMANCE SUMMARY")
    print("="*80)
    print(f"{'Test':<20} {'RPS':>8} {'p50':>8} {'p95':>8} {'p99':>8} "
          f"{'Avg':>8} {'Max':>8} {'Success':>10} {'Errors':>8}")
    print("-"*80)

    for t in tests:
        print(f"{t.name:<20} {t.rps:>8.2f} {t.p50_latency_ms:>8.2f} "
              f"{t.p95_latency_ms:>8.2f} {t.p99_latency_ms:>8.2f} "
              f"{t.avg_latency_ms:>8.2f} {t.max_latency_ms:>8.2f} "
              f"{t.success_rate:>9.1f}% {t.errors:>8}")

    # Write machine-readable report for regression tracking
    report = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "results": [asdict(t) for t in tests],
    }
    with open(REPORT_PATH, "w") as f:
        json.dump(report, f, indent=2)
    print(f"\nReport saved to {REPORT_PATH}")

    # Recommendations
    print("\n" + "="*80)
    print("RECOMMENDATIONS")
    print("="*80)

    avg_rps = sum(t.rps for t in tests) / len(tests) if tests else 0
    max_rps = max(t.rps for t in tests) if tests else 0
    worst_p99 = max(t.p99_latency_ms for t in tests) if tests else 0

    print(f"\nOverall Performance:")
    print(f"  Average RPS: {avg_rps:.2f}")
    print(f"  Max RPS: {max_rps:.2f}")
    print(f"  Worst p99 latency: {worst_p99:.2f} ms")

    if avg_rps < 10:
        print("\n⚠️  Low throughput - consider:")
        print("  - Increase NUM_THREADS in docker-compose.yml")
        print("  - Optimize NATS connection pool size")
    elif avg_rps < 50:
        print("\n✅ Moderate throughput - can improve with:")
        print("  - Increase NATS_POOL_SIZE")
        print("  - Enable NATS request batching")
    else:
        print("\n🚀 High throughput - well optimized!")

    print("\nMonitoring:")
    print("  - Compare p99 against scripts/perf-report.json baseline")
    print("  - Watch l2_proxy_request_duration_seconds p99")

if __name__ == "__main__":
    main()
