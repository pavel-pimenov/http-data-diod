#!/usr/bin/env python3
"""E2E graceful shutdown test for l2 services.

Runs `docker stop` on a service and verifies:
  * the container exits gracefully (ExitCode 0/143, not 137 = SIGKILL);
  * the service's own log shows its graceful-shutdown markers;
  * when a flood URL is given, requests in-flight at stop time are drained
    (completed) rather than lost;
  * the service becomes healthy again after `docker start`.

Usage:
  scripts/e2e-graceful-shutdown-test.py                    # all three services
  scripts/e2e-graceful-shutdown-test.py --container l2-worker
  scripts/e2e-graceful-shutdown-test.py --help

Exit code 0 = all tested services passed, 1 = any failed.
"""

import argparse
import asyncio
import json
import random
import subprocess
import sys
import time

import aiohttp

CONCURRENCY = 64
STEADY_SECONDS = 5.0
STOP_TIMEOUT_SECONDS = 60
HEALTH_POLLS = 30

# Container name -> (health URL, flood URL or None, log markers)
SERVICES = {
    "l2-proxy": (
        "http://localhost:8888/health",
        "http://localhost:8888",
        [
            "Received signal 15",
            "All in-flight requests completed gracefully",
            "server thread joined",
        ],
    ),
    "l2-worker": (
        "http://localhost:19093/health/ready",
        None,
        [
            "Shutting down gracefully",
            "Waiting for in-flight requests to complete",
            "Disconnected from NATS server",
        ],
    ),
    "l2-server": (
        "http://localhost:3333/health",
        "http://localhost:3333",
        [
            "Received signal 15",
            "server thread joined",
        ],
    ),
}


def make_payload(req_id: int) -> str:
    payload = {
        "value": 0,
        "client_id": f"e2e-shutdown-{random.randint(1, 100)}",
        "request_type": "echo",
        "version": "1.0.0",
        "timestamp": time.time(),
        "req_id": str(req_id),
    }
    return json.dumps(payload)


class Stats:
    def __init__(self):
        self.sent = 0
        self.completed = 0
        self.refused = 0
        self.timed_out = 0
        self.in_flight_at_stop = 0
        self.completed_in_flight_at_stop = 0
        self.stop_time = 0.0


class Worker:
    def __init__(self, stats: Stats, url: str, stop_event: asyncio.Event):
        self.stats = stats
        self.url = url
        self.stop_event = stop_event
        self.in_flight = {}
        self.next_id = 0

    def _complete(self, req_id, now: float) -> None:
        if self.in_flight.pop(req_id, None) is None:
            return
        if self.stats.stop_time > 0.0 and now <= self.stats.stop_time:
            self.stats.completed_in_flight_at_stop += 1
        self.stats.completed += 1

    async def run(self, session: aiohttp.ClientSession) -> None:
        headers = {
            "Content-Type": "application/json",
            "X-Correlation-Test": "1",
            "X-DataHub-Client-Id": "e2e-shutdown-client",
        }
        while not self.stop_event.is_set():
            req_id = self.next_id
            self.next_id += 1
            self.in_flight[req_id] = time.monotonic()
            self.stats.sent += 1
            try:
                async with session.post(
                    self.url, data=make_payload(req_id), headers=headers,
                    timeout=aiohttp.ClientTimeout(total=10),
                ) as resp:
                    await resp.read()
                    self._complete(req_id, time.monotonic())
            except (aiohttp.ClientConnectorError, aiohttp.ServerDisconnectedError):
                self.stats.refused += 1
                self._complete(req_id, time.monotonic())
            except asyncio.TimeoutError:
                self.stats.timed_out += 1
                self._complete(req_id, time.monotonic())


async def test_service(container: str, health_url: str, flood_url: str | None,
                       markers: list[str]) -> bool:
    print(f"\n{'=' * 70}\nTesting {container} (flood={flood_url or 'none'})\n"
          f"{'=' * 70}")
    stats = Stats()
    stop_event = asyncio.Event()

    async with aiohttp.ClientSession(
            connector=aiohttp.TCPConnector(limit=200)) as session:
        workers = [Worker(stats, flood_url, stop_event)
                   for _ in range(CONCURRENCY)] if flood_url else []
        tasks = [asyncio.create_task(w.run(session)) for w in workers]
        if workers:
            print(f"[flood] {CONCURRENCY} workers -> {flood_url} "
                  f"for {STEADY_SECONDS}s")
            await asyncio.sleep(STEADY_SECONDS)

        t0 = time.monotonic()
        print(f"[stop] docker stop -t {STOP_TIMEOUT_SECONDS} {container}")
        proc = subprocess.run(
            ["docker", "stop", "-t", str(STOP_TIMEOUT_SECONDS), container],
            capture_output=True, text=True)
        stats.stop_time = time.monotonic()
        stop_duration = stats.stop_time - t0
        stop_event.set()
        print(f"[stop] docker stop rc={proc.returncode} in {stop_duration:.2f}s")

        if workers:
            await asyncio.sleep(0.5)
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            stats.in_flight_at_stop = sum(len(w.in_flight) for w in workers)

        insp = subprocess.run(
            ["docker", "inspect", "--format", "{{.State.ExitCode}}", container],
            capture_output=True, text=True)
        exit_code = insp.stdout.strip()
        logs = subprocess.run(
            ["docker", "logs", container], capture_output=True, text=True).stdout

        print("=== RESULTS ===")
        print(f"docker stop rc: {proc.returncode} "
              f"({'graceful' if proc.returncode == 0 else 'ERROR'})")
        print(f"stop duration: {stop_duration:.2f}s "
              f"(timeout {STOP_TIMEOUT_SECONDS}s)")
        print(f"container ExitCode: {exit_code} "
              f"(0/143 = SIGTERM handled, 137 = SIGKILL)")
        if workers:
            print(f"sent: {stats.sent}")
            print(f"completed: {stats.completed}")
            print(f"conn refused (after stop): {stats.refused}")
            print(f"timed out: {stats.timed_out}")
            print(f"in-flight at stop: {stats.in_flight_at_stop}")

        ok = proc.returncode == 0 and exit_code in ("0", "143")
        for m in markers:
            found = m in logs
            print(f"log '{m}': {'FOUND' if found else 'MISSING'}")
            ok = ok and found
        if workers:
            if stats.in_flight_at_stop != 0:
                print("FAIL: requests still in-flight when process exited "
                      "(drain incomplete)")
            ok = ok and stats.in_flight_at_stop == 0

    subprocess.run(["docker", "start", container], check=True, capture_output=True)
    healthy = False
    for _ in range(HEALTH_POLLS):
        time.sleep(1)
        r = subprocess.run(
            ["curl", "-s", "-m", "2", "-o", "/dev/null", "-w", "%{http_code}",
             health_url], capture_output=True, text=True)
        if r.stdout.strip() == "200":
            healthy = True
            break
    print(f"healthy after restart: {'YES' if healthy else 'NO'}")
    return ok and healthy


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--container", default=None,
                        help="test a single container instead of all")
    args = parser.parse_args()

    ok = True
    for container, (health_url, flood_url, markers) in SERVICES.items():
        if args.container and container != args.container:
            continue
        ok = asyncio.run(
            test_service(container, health_url, flood_url, markers)) and ok
    print("\n" + ("PASS: all services shut down gracefully" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
