#!/usr/bin/env python3
"""
Load testing script for l2-proxy service.

Sends concurrent POST requests through nginx → proxy → worker → l2-server
and measures latency, throughput, and error rates.

Usage:
  python3 load_test.py --requests 1000 --concurrent 100
  python3 load_test.py --requests 5000 --concurrent 200 --url http://localhost:8888
"""

import asyncio
import aiohttp
import time
import argparse
import json
import statistics
from dataclasses import dataclass, field
from typing import List, Optional


@dataclass
class RequestResult:
    status: int
    latency_ms: float
    error: Optional[str] = None
    request_id: Optional[str] = None


@dataclass
class LoadTestReport:
    total_requests: int = 0
    successful: int = 0
    failed: int = 0
    status_codes: dict = field(default_factory=dict)
    latencies_ms: List[float] = field(default_factory=list)
    errors: List[str] = field(default_factory=list)
    total_time_s: float = 0.0
    rps: float = 0.0


async def send_request(
    session: aiohttp.ClientSession,
    url: str,
    semaphore: asyncio.Semaphore,
    req_id: int,
    payload: dict,
) -> RequestResult:
    async with semaphore:
        start = time.monotonic()
        try:
            async with session.post(
                url,
                json=payload,
                timeout=aiohttp.ClientTimeout(total=30),
                headers={"Content-Type": "application/json"},
            ) as resp:
                latency_ms = (time.monotonic() - start) * 1000
                body = await resp.text()
                request_id = None
                try:
                    data = json.loads(body)
                    request_id = data.get("request_id")
                except (json.JSONDecodeError, KeyError):
                    pass
                return RequestResult(
                    status=resp.status,
                    latency_ms=latency_ms,
                    request_id=request_id,
                )
        except asyncio.TimeoutError:
            latency_ms = (time.monotonic() - start) * 1000
            return RequestResult(status=0, latency_ms=latency_ms, error="timeout")
        except aiohttp.ClientError as e:
            latency_ms = (time.monotonic() - start) * 1000
            return RequestResult(
                status=0, latency_ms=latency_ms, error=f"client_error: {e}"
            )
        except Exception as e:
            latency_ms = (time.monotonic() - start) * 1000
            return RequestResult(
                status=0, latency_ms=latency_ms, error=f"unknown: {e}"
            )


def percentile(data: List[float], p: float) -> float:
    if not data:
        return 0.0
    sorted_data = sorted(data)
    k = (len(sorted_data) - 1) * (p / 100.0)
    f = int(k)
    c = f + 1
    if c >= len(sorted_data):
        return sorted_data[f]
    return sorted_data[f] + (k - f) * (sorted_data[c] - sorted_data[f])


def print_histogram(latencies: List[float], buckets_ms: List[float]) -> None:
    if not latencies:
        return
    print("\n  Latency Distribution:")
    print("  " + "-" * 60)
    prev = 0
    for bucket in buckets_ms:
        count = sum(1 for l in latencies if prev <= l < bucket)
        pct = count / len(latencies) * 100
        bar = "#" * int(pct / 2)
        print(f"  {prev:8.1f} - {bucket:8.1f} ms  | {count:5d} ({pct:5.1f}%) {bar}")
        prev = bucket
    overflow = sum(1 for l in latencies if l >= buckets_ms[-1])
    if overflow:
        pct = overflow / len(latencies) * 100
        bar = "#" * int(pct / 2)
        print(f"  {buckets_ms[-1]:8.1f}+         ms  | {overflow:5d} ({pct:5.1f}%)")


async def run_load_test(
    url: str,
    total_requests: int,
    concurrency: int,
    warmup: int = 0,
) -> LoadTestReport:
    report = LoadTestReport()
    payload = {
        "value": 1,
        "body": "load_test_payload",
        "proxy": {"mode": "load_test"},
        "worker": {"test": True},
        "server": {"type": "benchmark"},
        "timestamp": time.time(),
        "history": [1, 2, 3],
    }

    connector = aiohttp.TCPConnector(limit=concurrency, force_close=False)
    async with aiohttp.ClientSession(connector=connector) as session:
        # Warmup phase
        if warmup > 0:
            print(f"  Warmup: {warmup} requests...")
            warmup_sem = asyncio.Semaphore(concurrency)
            warmup_tasks = [
                send_request(session, url, warmup_sem, i, payload)
                for i in range(warmup)
            ]
            await asyncio.gather(*warmup_tasks)
            await asyncio.sleep(1)

        # Main load test
        print(
            f"  Sending {total_requests} requests with concurrency={concurrency}..."
        )
        sem = asyncio.Semaphore(concurrency)
        wall_start = time.monotonic()

        tasks = [
            send_request(session, url, sem, i, payload)
            for i in range(total_requests)
        ]
        results: List[RequestResult] = await asyncio.gather(*tasks)

        wall_time = time.monotonic() - wall_start

    # Build report
    report.total_requests = len(results)
    report.total_time_s = wall_time
    report.rps = report.total_requests / wall_time if wall_time > 0 else 0

    for r in results:
        report.latencies_ms.append(r.latency_ms)
        if r.error:
            report.failed += 1
            report.errors.append(r.error)
        elif r.status >= 200 and r.status < 400:
            report.successful += 1
        else:
            report.failed += 1
        code_key = str(r.status)
        report.status_codes[code_key] = report.status_codes.get(code_key, 0) + 1

    return report


def print_report(report: LoadTestReport) -> None:
    latencies = report.latencies_ms

    print("\n" + "=" * 70)
    print("  LOAD TEST REPORT")
    print("=" * 70)
    print(f"  Total requests:     {report.total_requests}")
    print(f"  Successful:         {report.successful}")
    print(f"  Failed:             {report.failed}")
    print(f"  Total time:         {report.total_time_s:.3f}s")
    print(f"  Throughput:         {report.rps:.1f} req/s")
    print(f"  Status codes:       {report.status_codes}")

    if latencies:
        print(f"\n  Latency (ms):")
        print(f"    Min:              {min(latencies):.1f}")
        print(f"    Max:              {max(latencies):.1f}")
        print(f"    Mean:             {statistics.mean(latencies):.1f}")
        print(f"    Median (p50):     {percentile(latencies, 50):.1f}")
        print(f"    p90:              {percentile(latencies, 90):.1f}")
        print(f"    p95:              {percentile(latencies, 95):.1f}")
        print(f"    p99:              {percentile(latencies, 99):.1f}")
        print(f"    p999:             {percentile(latencies, 99.9):.1f}")
        if len(latencies) > 1:
            print(f"    StdDev:           {statistics.stdev(latencies):.1f}")

        buckets = [1, 5, 10, 25, 50, 100, 250, 500, 1000, 5000]
        print_histogram(latencies, buckets)

    if report.errors:
        unique_errors = list(set(report.errors))[:10]
        print(f"\n  Top errors ({len(unique_errors)} of {len(report.errors)}):")
        for err in unique_errors:
            count = report.errors.count(err)
            print(f"    [{count}x] {err}")

    # Detect potential issues
    print(f"\n  Potential Issues:")
    issues_found = False

    if report.failed > 0:
        fail_rate = report.failed / report.total_requests * 100
        print(f"    !! {fail_rate:.1f}% failure rate ({report.failed}/{report.total_requests})")
        issues_found = True

    if latencies:
        p99 = percentile(latencies, 99)
        p999 = percentile(latencies, 99.9)
        if p99 > 1000:
            print(f"    !! p99 latency > 1s ({p99:.0f}ms) — possible bottleneck")
            issues_found = True
        if p999 > 5000:
            print(f"    !! p99.9 latency > 5s ({p999:.0f}ms) — possible timeout/stall")
            issues_found = True
        if max(latencies) > 10000:
            print(f"    !! Max latency > 10s ({max(latencies):.0f}ms) — possible dropped requests")
            issues_found = True
        mean = statistics.mean(latencies)
        if len(latencies) > 10:
            std = statistics.stdev(latencies)
            if std > mean:
                print(f"    !! High latency variance (stddev={std:.0f} > mean={mean:.0f})")
                issues_found = True

    if not issues_found:
        print(f"    No issues detected")

    print("=" * 70)


async def main():
    parser = argparse.ArgumentParser(description="l2-proxy load testing")
    parser.add_argument(
        "--requests", "-n", type=int, default=1000, help="Total requests (default: 1000)"
    )
    parser.add_argument(
        "--concurrent",
        "-c",
        type=int,
        default=100,
        help="Concurrent connections (default: 100)",
    )
    parser.add_argument(
        "--url",
        type=str,
        default="http://localhost:7777",
        help="Target URL (default: http://localhost:7777 via nginx)",
    )
    parser.add_argument(
        "--warmup", type=int, default=50, help="Warmup requests (default: 50)"
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=1,
        help="Number of test iterations (default: 1)",
    )
    args = parser.parse_args()

    print("=" * 70)
    print(f"  l2-proxy Load Test")
    print(f"  URL:          {args.url}")
    print(f"  Requests:     {args.requests}")
    print(f"  Concurrency:  {args.concurrent}")
    print(f"  Warmup:       {args.warmup}")
    print(f"  Iterations:   {args.iterations}")
    print("=" * 70)

    all_latencies = []
    all_rps = []

    for iteration in range(args.iterations):
        if args.iterations > 1:
            print(f"\n--- Iteration {iteration + 1}/{args.iterations} ---")

        report = await run_load_test(
            url=args.url,
            total_requests=args.requests,
            concurrency=args.concurrent,
            warmup=args.warmup,
        )
        print_report(report)
        all_latencies.extend(report.latencies_ms)
        all_rps.append(report.rps)

    if args.iterations > 1:
        print("\n" + "=" * 70)
        print("  AGGREGATE REPORT")
        print("=" * 70)
        print(f"  Total iterations:    {args.iterations}")
        print(f"  Total requests:      {args.iterations * args.requests}")
        print(f"  Avg RPS:             {statistics.mean(all_rps):.1f}")
        print(f"  Min RPS:             {min(all_rps):.1f}")
        print(f"  Max RPS:             {max(all_rps):.1f}")
        if all_latencies:
            print(f"  Overall p50:         {percentile(all_latencies, 50):.1f} ms")
            print(f"  Overall p99:         {percentile(all_latencies, 99):.1f} ms")
        print("=" * 70)


if __name__ == "__main__":
    asyncio.run(main())
