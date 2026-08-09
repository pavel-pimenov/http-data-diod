#!/usr/bin/env python3
"""
Integration tests for fault tolerance of the HTTP-data-diod stack.

Each test degrades one dependency (NATS server, L2 server, worker) via
`docker compose stop`, asserts the expected behaviour, then restores the
service with `docker compose start` and verifies recovery.

Scenarios:
  1. nats-server restart   - proxy/worker report not-ready, then recover;
                              message_counter.py passes after reconnect.
  2. l2-server down        - requests fail with 5xx in a bounded time (no
                              hang), then recover to 200.
  3. worker killed         - in-flight requests complete with 5xx (no hang),
                              then recover; message_counter.py passes.
  4. nats outage + dedup   - while requests are in-flight, nats-server is
                              stopped so in-flight replies are lost; after
                              recovery the proxy re-sends and the worker serves
                              them from its dedup cache without calling the L2
                              server again (l2_proxy_duplicate_requests_total
                              and l2_worker_duplicate_requests_total grow).

Every scenario restores the services it stopped, even on failure, so the
stack is left healthy at the end.

Requires a running stack (./rebuild-and-run.sh).

Usage:
  python3 fault_tolerance_test.py            # run all scenarios
  python3 fault_tolerance_test.py --skip nats --skip worker --skip dedup
"""

import argparse
import asyncio
import logging
import subprocess
import sys
import time
import urllib.request
from typing import List, Optional

import aiohttp

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[logging.StreamHandler(sys.stdout)]
)
logger = logging.getLogger(__name__)

PROXY_URL = 'http://localhost:8888'
PROXY_READY_URL = 'http://localhost:8888/health/ready'
WORKER_READY_URL = 'http://localhost:19093/health/ready'
PROXY_METRICS_URL = 'http://localhost:19090/metrics'
WORKER_METRICS_URL = 'http://localhost:19091/metrics'
SSL_VERIFY = False

# Client-side timeouts. Must exceed the proxy's REQUEST_TIMEOUT_SECONDS=30 so
# that a 504 (proxy deadline) is observed as a response, not a client timeout.
CLIENT_TIMEOUT = 50

# Wait budgets (seconds)
READY_TIMEOUT = 20          # initial stack readiness
NOT_READY_TIMEOUT = 60      # expected to become not-ready after a stop
RECOVERY_TIMEOUT = 120      # expected to become ready again after a start


class Colors:
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    NC = '\033[0m'


def run(cmd: list, timeout: int = 60) -> subprocess.CompletedProcess:
    logger.debug("Running: %s", " ".join(cmd))
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def compose_stop(service: str) -> None:
    r = run(["docker", "compose", "stop", service])
    if r.returncode != 0:
        raise RuntimeError(
            f"docker compose stop {service} failed: {r.stderr.strip()}")


def compose_start(service: str) -> None:
    r = run(["docker", "compose", "start", service])
    if r.returncode != 0:
        raise RuntimeError(
            f"docker compose start {service} failed: {r.stderr.strip()}")


async def http_get_status(url: str, timeout: float = 5.0) -> Optional[int]:
    try:
        timeout_obj = aiohttp.ClientTimeout(total=timeout)
        async with aiohttp.ClientSession(timeout=timeout_obj) as session:
            async with session.get(url, ssl=not SSL_VERIFY) as response:
                return response.status
    except Exception as e:
        logger.debug("GET %s failed: %s", url, e)
        return None


async def wait_until(url: str, expected: int, budget: float,
                     label: str) -> bool:
    start = time.time()
    last_status: Optional[int] = None
    while time.time() - start < budget:
        last_status = await http_get_status(url)
        if last_status == expected:
            logger.info("  OK: %s -> %s (%.1fs)", label, expected,
                        time.time() - start)
            return True
        await asyncio.sleep(2)
    logger.error("  FAIL: %s did not reach %s in %.0fs (last status: %s)",
                 label, expected, budget, last_status)
    return False


async def wait_ready(url: str, label: str) -> bool:
    return await wait_until(url, 200, READY_TIMEOUT, label)


async def wait_not_ready(url: str, label: str) -> bool:
    return await wait_until(url, 503, NOT_READY_TIMEOUT, label)


async def send_post(session: aiohttp.ClientSession,
                    timeout: float = CLIENT_TIMEOUT) -> dict:
    payload = {"value": 1, "body": "fault-tolerance-test"}
    start = time.time()
    try:
        timeout_obj = aiohttp.ClientTimeout(total=timeout)
        async with session.post(PROXY_URL, json=payload,
                                timeout=timeout_obj,
                                ssl=not SSL_VERIFY) as response:
            await response.read()
            return {"status": response.status,
                    "elapsed": time.time() - start,
                    "timed_out": False}
    except asyncio.TimeoutError:
        return {"status": None, "elapsed": time.time() - start,
                "timed_out": True}
    except aiohttp.ClientError as e:
        logger.warning("Request error: %s", e)
        return {"status": None, "elapsed": time.time() - start,
                "timed_out": True}


def fetch_metric(name: str, url: str) -> float:
    """Fetch the current value of a Prometheus counter from a /metrics endpoint."""
    try:
        text = urllib.request.urlopen(url, timeout=10).read().decode()
    except Exception as e:
        logger.warning("Failed to fetch %s from %s: %s", name, url, e)
        return 0.0
    for line in text.splitlines():
        if line.startswith(name):
            return float(line.split()[1])
    return 0.0


async def load_request(session: aiohttp.ClientSession,
                       timeout: float = CLIENT_TIMEOUT,
                       body: str = "fault-tolerance-test") -> dict:
    payload = {"value": 1, "body": body}
    start = time.time()
    try:
        timeout_obj = aiohttp.ClientTimeout(total=timeout)
        async with session.post(PROXY_URL, json=payload,
                                timeout=timeout_obj,
                                ssl=not SSL_VERIFY) as response:
            await response.read()
            return {"status": response.status,
                    "elapsed": time.time() - start,
                    "error": None}
    except asyncio.TimeoutError:
        return {"status": None, "elapsed": time.time() - start,
                "error": "timeout"}
    except aiohttp.ClientError as e:
        logger.debug("Load request error: %s", e)
        return {"status": None, "elapsed": time.time() - start,
                "error": "conn_error"}


async def run_duration_load(session: aiohttp.ClientSession, duration_s: float,
                            concurrency: int, body: str = "fault-tolerance-test") -> List[dict]:
    """Send POST requests continuously for duration_s seconds.

    A fixed number of worker coroutines each send requests until the duration
    elapses, so at most `concurrency` requests are in flight at once (no
    unbounded task queue behind the semaphore).
    """
    results: List[dict] = []
    end = time.monotonic() + duration_s

    async def worker():
        while time.monotonic() < end:
            results.append(await load_request(session, body=body))

    await asyncio.gather(*[worker() for _ in range(concurrency)])
    return results


async def wait_metric_stable(name: str, url: str, prev_value: float,
                             budget_s: float = 30.0) -> float:
    """Wait until the counter stops changing (no re-sends pending), max budget."""
    last = prev_value
    last_changed = time.time()
    start = time.time()
    while time.time() - start < budget_s:
        value = fetch_metric(name, url)
        if value != last:
            last = value
            last_changed = time.time()
        elif time.time() - last_changed >= 2:
            return last
        await asyncio.sleep(0.5)
    return last


def run_message_counter() -> bool:
    logger.info("Running message_counter.py (iterations=1, concurrent=1)...")
    r = run(["python3", "message_counter.py",
             "--iterations", "1", "--concurrent", "1"], timeout=120)
    if r.returncode == 0:
        print(f"{Colors.GREEN}  OK: message_counter.py passed{Colors.NC}")
        return True
    print(f"{Colors.RED}  FAIL: message_counter.py failed (exit "
          f"{r.returncode}){Colors.NC}")
    print(r.stdout[-2000:])
    print(r.stderr[-2000:])
    return False


async def check_initial_health() -> bool:
    logger.info("Checking initial stack health...")
    proxy_ok = await wait_ready(PROXY_READY_URL, "proxy /health/ready")
    worker_ok = await wait_ready(WORKER_READY_URL, "worker /health/ready")
    if not (proxy_ok and worker_ok):
        print(f"{Colors.RED}Initial stack is not healthy; run "
              f"./rebuild-and-run.sh first.{Colors.NC}")
        return False
    # Readiness endpoints don't cover l2-server; verify end-to-end message
    # flow so a stale leftover (e.g. l2-server down) fails fast here instead
    # of confusingly failing a scenario.
    if not run_message_counter():
        print(f"{Colors.RED}Initial stack message flow is broken; run "
              f"./rebuild-and-run.sh first.{Colors.NC}")
        return False
    return True


async def ensure_services_up(services: List[str]) -> None:
    """Best-effort restore: start services and wait for readiness."""
    for service in services:
        try:
            compose_start(service)
        except Exception as e:
            logger.error("Failed to start %s: %s", service, e)
    await wait_until(PROXY_READY_URL, 200, RECOVERY_TIMEOUT,
                     "proxy /health/ready (restore)")
    await wait_until(WORKER_READY_URL, 200, RECOVERY_TIMEOUT,
                     "worker /health/ready (restore)")


async def test_nats_reconnect() -> bool:
    print(f"\n{Colors.GREEN}{'=' * 60}\n[1/3] NATS server restart\n"
          f"{'=' * 60}{Colors.NC}")
    ok = True
    try:
        print("[1a] Stopping nats-server...")
        compose_stop("nats-server")

        print("[1b] Waiting for proxy/worker to report not-ready...")
        ok = await wait_not_ready(PROXY_READY_URL, "proxy /health/ready") and ok
        ok = await wait_not_ready(WORKER_READY_URL, "worker /health/ready") and ok

        print("[1c] Starting nats-server...")
        compose_start("nats-server")

        print("[1d] Waiting for proxy/worker to recover...")
        ok = await wait_until(PROXY_READY_URL, 200, RECOVERY_TIMEOUT,
                              "proxy /health/ready") and ok
        ok = await wait_until(WORKER_READY_URL, 200, RECOVERY_TIMEOUT,
                              "worker /health/ready") and ok
    except Exception as e:
        logger.error("NATS scenario raised: %s", e)
        ok = False
    finally:
        await ensure_services_up(["nats-server"])

    if ok:
        print("[1e] Verifying message consistency after reconnect...")
        ok = run_message_counter()
    return ok


async def test_l2_server_down() -> bool:
    print(f"\n{Colors.GREEN}{'=' * 60}\n[2/3] L2 server down\n"
          f"{'=' * 60}{Colors.NC}")
    ok = True
    try:
        print("[2a] Stopping l2-server...")
        compose_stop("l2-server")

        print("[2b] Sending requests while l2-server is down "
              "(expect 5xx in bounded time)...")
        connector = aiohttp.TCPConnector(limit=16, ssl=not SSL_VERIFY)
        async with aiohttp.ClientSession(connector=connector) as session:
            results = await asyncio.gather(
                *[send_post(session) for _ in range(5)])

        for i, res in enumerate(results, start=1):
            if res["timed_out"]:
                print(f"{Colors.RED}  FAIL: request {i} hung until client "
                      f"timeout ({res['elapsed']:.1f}s){Colors.NC}")
                ok = False
                continue
            if res["status"] < 500:
                print(f"{Colors.RED}  FAIL: request {i} returned "
                      f"{res['status']} (expected 5xx){Colors.NC}")
                ok = False
                continue
            logger.info("  request %d: status=%s elapsed=%.1fs",
                        i, res["status"], res["elapsed"])

        max_elapsed = max(r["elapsed"] for r in results)
        if ok:
            print(f"  OK: all requests returned 5xx without hanging "
                  f"(max {max_elapsed:.1f}s)")

        print("[2c] Starting l2-server...")
        compose_start("l2-server")

        print("[2d] Waiting for recovery (requests return 200)...")
        connector = aiohttp.TCPConnector(limit=4, ssl=not SSL_VERIFY)
        start = time.time()
        recovered = False
        async with aiohttp.ClientSession(connector=connector) as session:
            while time.time() - start < RECOVERY_TIMEOUT:
                res = await send_post(session)
                if res["status"] == 200:
                    recovered = True
                    break
                await asyncio.sleep(2)

        if not recovered:
            print(f"{Colors.RED}  FAIL: l2-server did not recover to 200 "
                  f"within {RECOVERY_TIMEOUT}s{Colors.NC}")
            ok = False
        else:
            print(f"  OK: request returned 200 after {time.time() - start:.1f}s")
    except Exception as e:
        logger.error("L2-server scenario raised: %s", e)
        ok = False
    finally:
        await ensure_services_up(["l2-server"])
    return ok


async def test_worker_kill() -> bool:
    print(f"\n{Colors.GREEN}{'=' * 60}\n[3/3] Worker killed (no hang)\n"
          f"{'=' * 60}{Colors.NC}")
    ok = True
    try:
        print("[3a] Stopping l2-worker...")
        compose_stop("l2-worker")

        print("[3b] Sending requests while worker is down "
              "(expect 5xx, no hang)...")
        connector = aiohttp.TCPConnector(limit=16, ssl=not SSL_VERIFY)
        async with aiohttp.ClientSession(connector=connector) as session:
            results = await asyncio.gather(
                *[send_post(session) for _ in range(3)])

        for i, res in enumerate(results, start=1):
            if res["timed_out"]:
                print(f"{Colors.RED}  FAIL: request {i} hung until client "
                      f"timeout ({res['elapsed']:.1f}s){Colors.NC}")
                ok = False
                continue
            if res["status"] < 500:
                print(f"{Colors.RED}  FAIL: request {i} returned "
                      f"{res['status']} (expected 5xx){Colors.NC}")
                ok = False
                continue
            logger.info("  request %d: status=%s elapsed=%.1fs",
                        i, res["status"], res["elapsed"])

        max_elapsed = max(r["elapsed"] for r in results)
        if ok:
            print(f"  OK: all requests completed with 5xx "
                  f"(max {max_elapsed:.1f}s, proxy did not hang)")

        print("[3c] Starting l2-worker...")
        compose_start("l2-worker")

        print("[3d] Waiting for worker recovery...")
        ok = await wait_until(WORKER_READY_URL, 200, RECOVERY_TIMEOUT,
                              "worker /health/ready") and ok
    except Exception as e:
        logger.error("Worker scenario raised: %s", e)
        ok = False
    finally:
        await ensure_services_up(["l2-worker"])

    if ok:
        print("[3e] Verifying message consistency after worker restart...")
        ok = run_message_counter()
    return ok


async def test_nats_dedup_resend() -> bool:
    print(f"\n{Colors.GREEN}{'=' * 60}\n[4/4] NATS outage: proxy re-send served "
          f"from dedup cache\n{'=' * 60}{Colors.NC}")
    ok = True
    load_task: Optional[asyncio.Task] = None
    statuses: List[dict] = []
    try:
        proxy_dup_before = fetch_metric("l2_proxy_duplicate_requests_total",
                                        PROXY_METRICS_URL)
        worker_dup_before = fetch_metric("l2_worker_duplicate_requests_total",
                                         WORKER_METRICS_URL)
        calls_before = fetch_metric("l2_worker_l2_calls_total",
                                    WORKER_METRICS_URL)
        processed_before = fetch_metric("l2_worker_requests_processed_total",
                                        WORKER_METRICS_URL)

        print("[4a] Starting continuous load while requests are in-flight...")
        # A large body keeps the worker busy long enough that requests already
        # delivered before the outage are still being processed (and their
        # replies lost) when nats-server goes down - the dedup cache then
        # answers the proxy's re-sends.
        big_body = "dedup-ft-" + "x" * (900 * 1024)
        connector = aiohttp.TCPConnector(limit=32, ssl=not SSL_VERIFY)
        async with aiohttp.ClientSession(connector=connector) as session:
            load_task = asyncio.create_task(
                run_duration_load(session, duration_s=6, concurrency=15,
                                  body=big_body))

            await asyncio.sleep(1.0)  # let the load ramp up

            print("[4b] Stopping nats-server (losing in-flight replies)...")
            compose_stop("nats-server")
            await asyncio.sleep(2)  # outage window

            print("[4c] Restarting nats-server...")
            compose_start("nats-server")
            await wait_until(PROXY_READY_URL, 200, RECOVERY_TIMEOUT,
                             "proxy /health/ready")
            await wait_until(WORKER_READY_URL, 200, RECOVERY_TIMEOUT,
                             "worker /health/ready")
    except Exception as e:
        logger.error("Dedup resend scenario raised: %s", e)
        ok = False
    finally:
        await ensure_services_up(["nats-server"])

    if load_task is not None:
        try:
            statuses = await asyncio.wait_for(load_task, timeout=60)
        except Exception as e:
            logger.error("Load generation failed: %s", e)
            ok = False

    ok200 = sum(1 for s in statuses if s["status"] == 200)
    hung = sum(1 for s in statuses if s["error"] == "timeout"
               and s["elapsed"] >= 40)
    conn_errors = sum(1 for s in statuses if s["error"] == "conn_error")
    print(f"  load results: 200={ok200}, connection errors={conn_errors}, "
          f"hung={hung}, total={len(statuses)}")
    if ok200 == 0:
        print(f"{Colors.RED}  FAIL: no request returned 200 after recovery{Colors.NC}")
        ok = False
    if hung > 0:
        print(f"{Colors.RED}  FAIL: {hung} requests genuinely hung until "
              f"client timeout{Colors.NC}")
        ok = False
    if conn_errors > 0:
        # During the outage the proxy drops in-flight client connections; the
        # requests themselves are re-sent after recovery (see metrics below).
        print(f"  INFO: {conn_errors} in-flight client connections were reset "
              f"during the outage (expected)")

    print("  Waiting for in-flight re-sends to settle...")
    proxy_dup_after = await wait_metric_stable(
        "l2_proxy_duplicate_requests_total", PROXY_METRICS_URL,
        proxy_dup_before)
    worker_dup_after = await wait_metric_stable(
        "l2_worker_duplicate_requests_total", WORKER_METRICS_URL,
        worker_dup_before)
    calls_after = fetch_metric("l2_worker_l2_calls_total", WORKER_METRICS_URL)
    processed_after = fetch_metric("l2_worker_requests_processed_total",
                                   WORKER_METRICS_URL)

    proxy_dup_delta = proxy_dup_after - proxy_dup_before
    worker_dup_delta = worker_dup_after - worker_dup_before
    calls_delta = calls_after - calls_before
    processed_delta = processed_after - processed_before
    print(f"  metrics: proxy re-sends delta={proxy_dup_delta}, "
          f"worker cache-hits delta={worker_dup_delta}")
    print(f"           worker l2_calls delta={calls_delta}, "
          f"requests_processed delta={processed_delta}")

    if proxy_dup_delta <= 0:
        print(f"{Colors.RED}  FAIL: proxy did not re-send any request "
              f"(delta={proxy_dup_delta}){Colors.NC}")
        ok = False
    if worker_dup_delta <= 0:
        print(f"{Colors.RED}  FAIL: worker did not serve any request from the "
              f"dedup cache (delta={worker_dup_delta}){Colors.NC}")
        ok = False
    if calls_delta > processed_delta:
        print(f"{Colors.RED}  FAIL: worker l2_calls delta ({calls_delta}) "
              f"exceeds requests_processed delta ({processed_delta}) — "
              f"anomalous L2 calls{Colors.NC}")
        ok = False

    if ok:
        print("[4d] Verifying message consistency after recovery...")
        ok = run_message_counter()
    return ok


def parse_args():
    parser = argparse.ArgumentParser(
        description="Fault tolerance integration tests for the HTTP-data-diod "
                    "stack")
    parser.add_argument("--skip", action="append", default=[],
                        choices=["nats", "server", "worker", "dedup"],
                        help="Skip a scenario (may be repeated)")
    return parser.parse_args()


async def main() -> int:
    args = parse_args()
    if not await check_initial_health():
        return 1

    results: List[tuple] = []
    scenarios = [
        ("nats", test_nats_reconnect),
        ("server", test_l2_server_down),
        ("worker", test_worker_kill),
        ("dedup", test_nats_dedup_resend),
    ]

    for name, coro in scenarios:
        if name in args.skip:
            print(f"\n[SKIP] scenario {name}")
            continue
        try:
            ok = await coro()
        except Exception as e:
            logger.error("Scenario %s raised: %s", name, e)
            ok = False
        results.append((name, ok))

    # Final safety: make sure the stack is left healthy.
    await ensure_services_up(["nats-server", "l2-server", "l2-worker"])

    print(f"\n{Colors.GREEN}{'=' * 60}\nSummary\n"
          f"{'=' * 60}{Colors.NC}")
    all_ok = True
    for name, ok in results:
        mark = "PASS" if ok else "FAIL"
        print(f"  [{mark}] {name}")
        all_ok = all_ok and ok

    if all_ok:
        print(f"\n{Colors.GREEN}All fault tolerance tests passed.{Colors.NC}")
    else:
        print(f"\n{Colors.RED}Some fault tolerance tests failed.{Colors.NC}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
