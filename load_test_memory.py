#!/usr/bin/env python3
"""
Hour-long load test for l2-proxy with memory leak detection.

Sends concurrent POST requests for a specified duration and monitors
container memory usage (RSS) every 30 seconds. Detects linear memory
growth patterns that indicate potential leaks.

Usage:
    python3 load_test_memory.py --duration 3600 --concurrent 20
    python3 load_test_memory.py --duration 3600 --concurrent 50 --url http://localhost:7777

Requirements:
    pip install aiohttp psutil
"""

import asyncio
import aiohttp
import time
import argparse
import logging
import sys
import random
import json
import subprocess
from datetime import datetime
from typing import List, Dict, Optional
from dataclasses import dataclass, field

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.StreamHandler(sys.stdout),
        logging.FileHandler('load_test_memory.log')
    ]
)
logger = logging.getLogger(__name__)


@dataclass
class MemorySnapshot:
    timestamp: float
    rss_bytes: int
    requests_sent: int
    errors: int
    avg_latency_ms: float


@dataclass
class LoadTestStats:
    total_requests: int = 0
    successful_requests: int = 0
    failed_requests: int = 0
    warning_requests: int = 0
    total_bytes_sent: int = 0
    total_bytes_received: int = 0
    latencies: List[float] = field(default_factory=list)
    memory_snapshots: List[MemorySnapshot] = field(default_factory=list)
    start_time: float = 0.0
    error_messages: Dict[str, int] = field(default_factory=dict)
    warning_messages: Dict[str, int] = field(default_factory=dict)

    @property
    def duration(self) -> float:
        return time.time() - self.start_time

    @property
    def rps(self) -> float:
        d = self.duration
        return self.total_requests / d if d > 0 else 0

    @property
    def avg_latency(self) -> float:
        return sum(self.latencies) / len(self.latencies) if self.latencies else 0

    @property
    def p99_latency(self) -> float:
        if not self.latencies:
            return 0
        sorted_lat = sorted(self.latencies)
        idx = int(len(sorted_lat) * 0.99)
        return sorted_lat[min(idx, len(sorted_lat) - 1)]


def get_container_memory(container_name: str) -> Optional[int]:
    """Get RSS memory of a Docker container in bytes."""
    try:
        result = subprocess.run(
            ['docker', 'stats', '--no-stream', '--format',
             '{{.MemUsage}}', container_name],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            mem_str = result.stdout.strip()
            # Parse "123.4MiB / 8GiB" format
            parts = mem_str.split('/')
            if len(parts) >= 1:
                mem_part = parts[0].strip()
                if 'GiB' in mem_part:
                    return int(float(mem_part.replace('GiB', '').strip()) * 1024 * 1024 * 1024)
                elif 'MiB' in mem_part:
                    return int(float(mem_part.replace('MiB', '').strip()) * 1024 * 1024)
                elif 'KiB' in mem_part:
                    return int(float(mem_part.replace('KiB', '').strip()) * 1024)
                elif 'B' in mem_part:
                    return int(float(mem_part.replace('B', '').strip()))
    except Exception as e:
        logger.debug(f"Failed to get memory for {container_name}: {e}")
    return None


def get_process_memory(container_name: str) -> Optional[int]:
    """Get RSS memory of the main process inside a Docker container."""
    try:
        result = subprocess.run(
            ['docker', 'exec', container_name, 'cat', '/proc/1/status'],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            for line in result.stdout.split('\n'):
                if line.startswith('VmRSS:'):
                    # "VmRSS:   123456 kB"
                    parts = line.split()
                    if len(parts) >= 2:
                        return int(parts[1]) * 1024  # kB to bytes
    except Exception:
        pass
    return get_container_memory(container_name)


def generate_payload(size_kb: int = 10) -> str:
    """Generate JSON payload of approximately size_kb kilobytes."""
    metrics = {
        "proxy": {
            "client_requests": random.randint(1000, 100000),
            "bytes_received": random.randint(100000, 10000000),
            "bytes_sent": random.randint(100000, 10000000),
            "request_duration_p99": round(random.uniform(0.01, 2.0), 4),
        },
        "worker": {
            "requests_processed": random.randint(1000, 100000),
            "nats_operations": random.randint(1000, 100000),
            "l2_calls": random.randint(1000, 100000),
            "l2_call_duration_p99": round(random.uniform(0.01, 5.0), 4),
        },
        "server": {
            "requests": random.randint(1000, 100000),
            "request_errors": random.randint(0, 100),
            "bytes_received": random.randint(100000, 10000000),
            "bytes_sent": random.randint(100000, 10000000),
        },
        "value": 1,
        "timestamp": datetime.now().isoformat(),
        "host": f"load-test-worker-{random.randint(1, 100)}",
        "version": "load-test-1.0",
    }

    # Pad to approximate size
    padding_size = max(0, size_kb * 1024 - len(json.dumps(metrics)))
    if padding_size > 0:
        metrics["padding"] = "x" * padding_size

    return json.dumps(metrics)


async def send_request(session: aiohttp.ClientSession, url: str,
                       stats: LoadTestStats, stop_event: asyncio.Event,
                       size_kb: int = 10):
    """Send a single POST request and track stats."""
    while not stop_event.is_set():
        payload = generate_payload(size_kb)
        start = time.time()
        try:
            async with session.post(url, data=payload,
                                    headers={'Content-Type': 'application/json'},
                                    timeout=aiohttp.ClientTimeout(total=30)) as resp:
                await resp.read()
                latency = (time.time() - start) * 1000
                stats.latencies.append(latency)
                stats.total_requests += 1
                stats.total_bytes_sent += len(payload)
                stats.total_bytes_received += resp.content_length or 0
                if resp.status == 200:
                    stats.successful_requests += 1
                elif resp.status == 429:
                    stats.warning_requests += 1
                    stats.warning_messages["429 Too Many Requests"] = \
                        stats.warning_messages.get("429 Too Many Requests", 0) + 1
                else:
                    stats.failed_requests += 1
                    stats.error_messages[f"HTTP {resp.status}"] = \
                        stats.error_messages.get(f"HTTP {resp.status}", 0) + 1
        except asyncio.TimeoutError:
            stats.total_requests += 1
            stats.failed_requests += 1
            stats.error_messages["timeout"] = \
                stats.error_messages.get("timeout", 0) + 1
        except Exception as e:
            stats.total_requests += 1
            stats.failed_requests += 1
            err_type = type(e).__name__
            stats.error_messages[err_type] = \
                stats.error_messages.get(err_type, 0) + 1

        # Small random delay to avoid tight loop
        await asyncio.sleep(random.uniform(0.001, 0.01))


async def memory_monitor(container_names: List[str], stats: LoadTestStats,
                         stop_event: asyncio.Event, interval: int = 30):
    """Monitor container memory usage every interval seconds."""
    logger.info(f"Memory monitor started for containers: {container_names}")
    while not stop_event.is_set():
        await asyncio.sleep(interval)
        if stop_event.is_set():
            break

        for name in container_names:
            mem = get_process_memory(name)
            if mem is not None:
                snap = MemorySnapshot(
                    timestamp=time.time(),
                    rss_bytes=mem,
                    requests_sent=stats.total_requests,
                    errors=stats.failed_requests,
                    avg_latency_ms=stats.avg_latency
                )
                stats.memory_snapshots.append(snap)
                elapsed = stats.duration
                mem_mb = mem / (1024 * 1024)
                logger.info(f"[{elapsed:.0f}s] {name}: RSS={mem_mb:.1f}MB "
                            f"reqs={stats.total_requests} "
                            f"ok={stats.successful_requests} "
                            f"err={stats.failed_requests} "
                            f"warn={stats.warning_requests} "
                            f"rps={stats.rps:.1f} "
                            f"lat_avg={stats.avg_latency:.1f}ms "
                            f"lat_p99={stats.p99_latency:.1f}ms")


def analyze_memory_trend(snapshots: List[MemorySnapshot]) -> Dict:
    """Analyze memory snapshots for leak patterns."""
    if len(snapshots) < 3:
        return {"status": "INSUFFICIENT_DATA", "message": "Need at least 3 snapshots"}

    # Calculate memory growth rate (bytes per minute)
    first = snapshots[0]
    last = snapshots[-1]
    time_diff_min = (last.timestamp - first.timestamp) / 60.0
    if time_diff_min < 1:
        return {"status": "INSUFFICIENT_DATA", "message": "Test too short"}

    mem_diff_bytes = last.rss_bytes - first.rss_bytes
    growth_rate_mb_per_min = (mem_diff_bytes / (1024 * 1024)) / time_diff_min

    # Linear regression for trend
    n = len(snapshots)
    x_vals = [(s.timestamp - first.timestamp) / 60.0 for s in snapshots]
    y_vals = [s.rss_bytes / (1024 * 1024) for s in snapshots]
    x_mean = sum(x_vals) / n
    y_mean = sum(y_vals) / n
    numerator = sum((x - x_mean) * (y - y_mean) for x, y in zip(x_vals, y_vals))
    denominator = sum((x - x_mean) ** 2 for x in x_vals)
    slope = numerator / denominator if denominator > 0 else 0
    intercept = y_mean - slope * x_mean

    # R-squared
    ss_res = sum((y - (slope * x + intercept)) ** 2
                 for x, y in zip(x_vals, y_vals))
    ss_tot = sum((y - y_mean) ** 2 for y in y_vals)
    r_squared = 1 - (ss_res / ss_tot) if ss_tot > 0 else 0

    # Classify
    if slope > 1.0 and r_squared > 0.7:
        status = "LIKELY_LEAK"
        message = (f"Memory grows ~{slope:.2f} MB/min with R²={r_squared:.3f}. "
                   f"Total growth: {growth_rate_mb_per_min:.2f} MB/min")
    elif slope > 0.5 and r_squared > 0.5:
        status = "SUSPICIOUS"
        message = (f"Memory growth ~{slope:.2f} MB/min (R²={r_squared:.3f}). "
                   f"May be normal if caches are warming up.")
    elif abs(slope) < 0.5:
        status = "STABLE"
        message = f"Memory stable (~{slope:.2f} MB/min). No leak detected."
    else:
        status = "DECREASING"
        message = f"Memory decreasing (~{slope:.2f} MB/min). Possible GC."

    return {
        "status": status,
        "message": message,
        "slope_mb_per_min": slope,
        "r_squared": r_squared,
        "first_rss_mb": first.rss_bytes / (1024 * 1024),
        "last_rss_mb": last.rss_bytes / (1024 * 1024),
        "total_growth_mb": mem_diff_bytes / (1024 * 1024),
        "duration_min": time_diff_min,
        "snapshots_count": n,
    }


def print_report(stats: LoadTestStats, container_names: List[str]):
    """Print final test report."""
    print("\n" + "=" * 70)
    print("LOAD TEST REPORT — Memory Leak Detection")
    print("=" * 70)
    print(f"Duration:       {stats.duration:.0f}s ({stats.duration/60:.1f} min)")
    print(f"Total requests: {stats.total_requests}")
    print(f"Successful:     {stats.successful_requests}")
    print(f"Failed:         {stats.failed_requests}")
    print(f"Warnings:       {stats.warning_requests}")
    print(f"RPS:            {stats.rps:.1f}")
    print(f"Avg latency:    {stats.avg_latency:.1f}ms")
    print(f"P99 latency:    {stats.p99_latency:.1f}ms")
    print(f"Data sent:      {stats.total_bytes_sent / (1024*1024):.1f} MB")
    print(f"Data received:  {stats.total_bytes_received / (1024*1024):.1f} MB")

    if stats.warning_messages:
        print(f"\nWarnings:")
        for warn, count in sorted(stats.warning_messages.items(), key=lambda x: -x[1]):
            print(f"  {warn}: {count}")

    if stats.error_messages:
        print(f"\nErrors:")
        for err, count in sorted(stats.error_messages.items(), key=lambda x: -x[1]):
            print(f"  {err}: {count}")

    print(f"\nMemory Analysis ({len(stats.memory_snapshots)} snapshots):")
    print("-" * 70)

    for name in container_names:
        container_snaps = [s for s in stats.memory_snapshots if True]  # all snapshots
        if not container_snaps:
            print(f"  {name}: No memory data collected")
            continue

        trend = analyze_memory_trend(container_snaps)
        status_icon = {
            "STABLE": "✅",
            "LIKELY_LEAK": "❌",
            "SUSPICIOUS": "⚠️ ",
            "DECREASING": "📉",
            "INSUFFICIENT_DATA": "❓"
        }.get(trend["status"], "?")

        print(f"\n  {name}:")
        print(f"    Status:  {status_icon} {trend['status']}")
        print(f"    {trend['message']}")
        if "first_rss_mb" in trend:
            print(f"    RSS:     {trend['first_rss_mb']:.1f} MB → {trend['last_rss_mb']:.1f} MB "
                  f"(Δ {trend['total_growth_mb']:+.1f} MB over {trend['duration_min']:.1f} min)")

    # Memory timeline
    if stats.memory_snapshots:
        print(f"\nMemory Timeline:")
        print("-" * 70)
        for snap in stats.memory_snapshots:
            elapsed = snap.timestamp - stats.start_time
            mem_mb = snap.rss_bytes / (1024 * 1024)
            bar = "█" * int(mem_mb / 10)
            print(f"  {elapsed:6.0f}s | RSS: {mem_mb:8.1f}MB | "
                  f"reqs: {snap.requests_sent:8d} | {bar}")

    print("\n" + "=" * 70)

    # Final verdict
    any_leak = False
    for name in container_names:
        snaps = stats.memory_snapshots
        if snaps:
            trend = analyze_memory_trend(snaps)
            if trend["status"] == "LIKELY_LEAK":
                any_leak = True

    if any_leak:
        print("❌ VERDICT: Potential memory leak detected!")
        print("   Recommended: rebuild with ASan (./rebuild-and-run.sh --asan)")
        print("   and re-run this test to get detailed leak traces.")
    else:
        print("✅ VERDICT: No memory leak detected in this test run.")
    print("=" * 70)


async def main():
    parser = argparse.ArgumentParser(description="Load test with memory monitoring")
    parser.add_argument("--url", default="http://localhost:7777",
                        help="Target URL (default: http://localhost:7777)")
    parser.add_argument("--duration", type=int, default=3600,
                        help="Test duration in seconds (default: 3600 = 1 hour)")
    parser.add_argument("--concurrent", type=int, default=20,
                        help="Number of concurrent workers (default: 20)")
    parser.add_argument("--payload-size", type=int, default=10,
                        help="Payload size in KB (default: 10)")
    parser.add_argument("--memory-interval", type=int, default=30,
                        help="Memory check interval in seconds (default: 30)")
    parser.add_argument("--containers", nargs="+",
                        default=["l2-proxy", "l2-worker"],
                        help="Docker containers to monitor")
    parser.add_argument("--report", default="load_test_report.json",
                        help="Output report file (default: load_test_report.json)")
    args = parser.parse_args()

    logger.info(f"Starting load test: url={args.url} duration={args.duration}s "
                f"concurrent={args.concurrent} payload={args.payload_size}KB")
    logger.info(f"Monitoring containers: {args.containers}")

    stats = LoadTestStats(start_time=time.time())
    stop_event = asyncio.Event()

    connector = aiohttp.TCPConnector(limit=args.concurrent * 2,
                                     limit_per_host=args.concurrent * 2)
    async with aiohttp.ClientSession(connector=connector) as session:
        # Start memory monitor
        monitor_task = asyncio.create_task(
            memory_monitor(args.containers, stats, stop_event, args.memory_interval)
        )

        # Start worker tasks
        workers = []
        for i in range(args.concurrent):
            task = asyncio.create_task(
                send_request(session, args.url, stats, stop_event, args.payload_size)
            )
            workers.append(task)

        # Run for specified duration
        logger.info(f"Load test running for {args.duration} seconds...")
        await asyncio.sleep(args.duration)

        # Stop everything
        stop_event.set()
        monitor_task.cancel()
        for w in workers:
            w.cancel()

        # Wait for tasks to finish
        await asyncio.gather(*workers, return_exceptions=True)
        await asyncio.gather(monitor_task, return_exceptions=True)

    # Print report
    print_report(stats, args.containers)

    # Save JSON report
    report = {
        "timestamp": datetime.now().isoformat(),
        "duration_seconds": stats.duration,
        "total_requests": stats.total_requests,
        "successful_requests": stats.successful_requests,
        "failed_requests": stats.failed_requests,
        "warning_requests": stats.warning_requests,
        "rps": stats.rps,
        "avg_latency_ms": stats.avg_latency,
        "p99_latency_ms": stats.p99_latency,
        "containers": {},
    }
    for name in args.containers:
        snaps = stats.memory_snapshots
        if snaps:
            report["containers"][name] = analyze_memory_trend(snaps)
            report["containers"][name]["snapshots"] = [
                {"time": s.timestamp - stats.start_time,
                 "rss_mb": s.rss_bytes / (1024*1024),
                 "requests": s.requests_sent}
                for s in snaps
            ]

    with open(args.report, 'w') as f:
        json.dump(report, f, indent=2)
    logger.info(f"Report saved to {args.report}")

    return 1 if any(c.get("status") == "LIKELY_LEAK"
                     for c in report["containers"].values()) else 0


if __name__ == "__main__":
    exit_code = asyncio.run(main())
    sys.exit(exit_code)
