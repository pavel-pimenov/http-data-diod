#!/usr/bin/env python3
"""E2E graceful shutdown test for l2-proxy.

Loads the proxy with a continuous flood of POST requests, issues `docker stop`
mid-flood, then verifies:
  * the container exits gracefully (not SIGKILL);
  * the log shows "Received signal 15, exiting...", the in-flight drain
    completed ("All in-flight requests completed gracefully") and the server
    thread joined;
  * requests in-flight at stop time are drained (completed) rather than lost;
  * the proxy becomes healthy again after `docker start`.

Exit code 0 = pass, 1 = fail. Meant to run against the compose stack
(./rebuild-and-run.sh) with the l2-proxy container up.
"""

import asyncio
import json
import random
import subprocess
import sys
import time

import aiohttp

PROXY_URL = "http://localhost:8888"
CONTAINER = "l2-proxy"
CONCURRENCY = 64
STEADY_SECONDS = 5.0
STOP_TIMEOUT_SECONDS = 60
HEALTH_POLLS = 30


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
    def __init__(self, stats: Stats, stop_event: asyncio.Event):
        self.stats = stats
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
                    PROXY_URL, data=make_payload(req_id), headers=headers,
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


async def main() -> int:
    stats = Stats()
    stop_event = asyncio.Event()
    workers = [Worker(stats, stop_event) for _ in range(CONCURRENCY)]

    async with aiohttp.ClientSession(
            connector=aiohttp.TCPConnector(limit=200)) as session:
        tasks = [asyncio.create_task(w.run(session)) for w in workers]
        print(f"[flood] {CONCURRENCY} workers -> {PROXY_URL} "
              f"for {STEADY_SECONDS}s")
        await asyncio.sleep(STEADY_SECONDS)

        t0 = time.monotonic()
        print(f"[stop] docker stop -t {STOP_TIMEOUT_SECONDS} {CONTAINER}")
        proc = subprocess.run(
            ["docker", "stop", "-t", str(STOP_TIMEOUT_SECONDS), CONTAINER],
            capture_output=True, text=True)
        stats.stop_time = time.monotonic()
        stop_duration = stats.stop_time - t0
        stop_event.set()
        print(f"[stop] docker stop rc={proc.returncode} in {stop_duration:.2f}s")

        await asyncio.sleep(0.5)
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        stats.in_flight_at_stop = sum(len(w.in_flight) for w in workers)

        insp = subprocess.run(
            ["docker", "inspect", "--format", "{{.State.ExitCode}}", CONTAINER],
            capture_output=True, text=True)
        exit_code = insp.stdout.strip()
        logs = subprocess.run(
            ["docker", "logs", CONTAINER], capture_output=True, text=True).stdout

        print("\n=== RESULTS ===")
        print(f"docker stop rc: {proc.returncode} "
              f"({'graceful' if proc.returncode == 0 else 'ERROR'})")
        print(f"stop duration: {stop_duration:.2f}s "
              f"(timeout {STOP_TIMEOUT_SECONDS}s)")
        print(f"container ExitCode: {exit_code} "
              f"(143 = SIGTERM, 137 = SIGKILL)")
        print(f"sent: {stats.sent}")
        print(f"completed: {stats.completed}")
        print(f"conn refused (after stop): {stats.refused}")
        print(f"timed out: {stats.timed_out}")
        print(f"in-flight at stop: {stats.in_flight_at_stop}")
        print(f"of those completed after stop: "
              f"{stats.completed_in_flight_at_stop}")

        markers = [
            "Received signal 15",
            "All in-flight requests completed gracefully",
            "server thread joined",
        ]
        ok = proc.returncode == 0
        ok = ok and exit_code in ("0", "143")
        for m in markers:
            found = m in logs
            print(f"log '{m}': {'FOUND' if found else 'MISSING'}")
            ok = ok and found
        ok = ok and stats.in_flight_at_stop == 0
        if stats.in_flight_at_stop != 0:
            print("FAIL: requests were still in-flight when the process "
                  "exited (drain incomplete)")

    subprocess.run(["docker", "start", CONTAINER], check=True, capture_output=True)
    healthy = False
    for _ in range(HEALTH_POLLS):
        time.sleep(1)
        r = subprocess.run(
            ["curl", "-s", "-m", "2", "-o", "/dev/null", "-w", "%{http_code}",
             f"{PROXY_URL}/health"], capture_output=True, text=True)
        if r.stdout.strip() == "200":
            healthy = True
            break
    print(f"proxy healthy after restart: {'YES' if healthy else 'NO'}")
    ok = ok and healthy

    print("\n" + ("PASS: graceful shutdown verified" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
