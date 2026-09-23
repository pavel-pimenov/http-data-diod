#!/usr/bin/env python3
"""E2E reconnect/dedup test for the NATS worker.

Verifies the worker-side resilience contract when the NATS server goes away:

  * downtime:  worker /health/ready flips to 503 ({"status":"not_ready"}),
               proxy rejects real requests with a 5xx JSON error body;
  * recovery:  after nats-server comes back the worker reconnects and serves
               requests again (health 200 + echo round-trip);
  * dedup:     a NATS request re-delivered with the same request_id (the proxy
               re-sends when it lost the reply) within the dedup TTL is answered
               from the cache without a second L2 call (worker logs
               "Duplicate NATS request detected").

Usage:
  scripts/e2e-worker-reconnect-test.py
  requires: docker, python3 with aiohttp + nats-py; the compose stack is up.

Exit code 0 = all phases passed, 1 = any failure.
"""

import argparse
import asyncio
import json
import os
import subprocess
import sys
import time
from pathlib import Path

import aiohttp

PROXY_URL = "http://localhost:8888"
WORKER_HEALTH = "http://localhost:19093/health/ready"
NATS_HOST = "localhost"
NATS_PORT = 4222
NATS_SUBJECT = "service.proxy"  # must match NATS_SUBJECT in docker-compose

NSERVER_CONTAINER = "nats-server"
WORKER_CONTAINER = "l2-worker"

PHASES = ["outage", "recovery", "dedup"]


def nats_url() -> str:
    """Authorized NATS URL; honors native nats:// scheme override."""
    env_url = os.environ.get("E2E_NATS_URL")
    if env_url:
        return env_url
    token = os.environ.get("E2E_NATS_TOKEN")
    if not token:
        env_path = Path(__file__).parent.parent / ".env"
        if env_path.exists():
            for line in env_path.read_text().splitlines():
                if line.startswith("NATS_TOKEN="):
                    token = line.split("=", 1)[1].strip()
                    break
    if not token or token in ("", "-"):
        # No auth configured (fresh clone / CI): plain URL.
        return f"nats://localhost:{NATS_PORT}"
    return f"nats://{token}@localhost:{NATS_PORT}"


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def docker(args: list[str], timeout: float = 40.0) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["docker", *args], capture_output=True, text=True, timeout=timeout
    )


def make_echo_payload(client_id: str, value: int) -> dict:
    return {
        "value": value,
        "client_id": client_id,
        "request_type": "echo",
        "version": "1.0.0",
        "timestamp": time.time(),
        "req_id": str(value),
    }


async def proxy_post(payload: dict, session: aiohttp.ClientSession, timeout: float = 30.0):
    headers = {
        "Content-Type": "application/json",
        "X-Correlation-Test": "1",
        "X-DataHub-Client-Id": payload["client_id"],
    }
    try:
        async with session.post(
            PROXY_URL, data=json.dumps(payload), headers=headers,
            timeout=aiohttp.ClientTimeout(total=timeout),
        ) as resp:
            return resp.status, await resp.text()
    except (aiohttp.ClientError, asyncio.TimeoutError):
        # Offline backend / hang: report as a -1 (client-side timeout) so callers
        # can treat "no successful response" uniformly.
        return -1, ""


async def wait_for(predicate, timeout_s: float, step_s: float = 0.5, desc: str = ""):
    deadline = time.monotonic() + timeout_s
    last = None
    while time.monotonic() < deadline:
        last = await predicate()
        if last:
            return last
        await asyncio.sleep(step_s)
    raise TimeoutError(f"timed out waiting for {desc} (last result: {last!r})")


async def worker_health_ok(session: aiohttp.ClientSession) -> int:
    try:
        async with session.get(
            WORKER_HEALTH, timeout=aiohttp.ClientTimeout(total=5)
        ) as resp:
            return resp.status
    except (aiohttp.ClientError, ConnectionError):
        return -1


async def nats_server_up() -> bool:
    """True when the NATS server accepts an authorized connection."""
    try:
        import nats
        nc = await nats.connect(nats_url(), connect_timeout=3)
        await nc.close()
        return True
    except Exception:
        return False


async def run_baseline(session: aiohttp.ClientSession) -> None:
    status, body = await proxy_post(make_echo_payload(f"e2e-reconnect-{int(time.time())}", 1), session)
    if status != 200:
        raise AssertionError(f"baseline POST failed: status={status} body={body[:200]}")
    parsed = json.loads(body)
    if parsed.get("value_return") != 1:
        raise AssertionError(f"baseline echo mismatch: {body[:200]}")
    log("baseline: echo round-trip OK")


async def phase_outage(session: aiohttp.ClientSession) -> None:
    log("stopping nats-server")
    docker(["stop", "-t", "5", NSERVER_CONTAINER])

    async def not_ready():
        return (await worker_health_ok(session)) == 503
    await wait_for(not_ready, timeout_s=45, desc="worker /health/ready -> 503")
    log("worker /health/ready -> 503 (not_ready)")

    status, body = await proxy_post(
        make_echo_payload(f"e2e-reconnect-{int(time.time())}", 2), session, timeout=45
    )
    # The proxy may reject quickly (500 queue_failed) or hang until its poll
    # timeout and only then surface a 504. Either way a request sent while NATS
    # is down must NOT be served: any 5xx or a client timeout is acceptable.
    if status == 200:
        raise AssertionError(
            f"downtime POST unexpectedly succeeded while NATS is down: {body[:200]}"
        )
    if status not in (-1,) and not (500 <= status <= 504):
        raise AssertionError(f"unexpected downtime status {status}: {body[:200]}")
    if status == -1:
        log("downtime: proxy request timed out while NATS is down (accepted, not 200)")
    else:
        try:
            parsed = json.loads(body)
            if "error" not in parsed:
                raise AssertionError(
                    f"downtime error body missing 'error' key: {body[:200]}"
                )
        except json.JSONDecodeError:
            raise AssertionError(f"downtime error body is not JSON: {body[:160]}") from None
        log(f"downtime: proxy returned {status} with JSON error contract (OK)")


async def phase_recovery(session: aiohttp.ClientSession) -> None:
    log("starting nats-server")
    docker(["start", NSERVER_CONTAINER])

    await wait_for(nats_server_up, timeout_s=60, desc="nats-server accepting connections")
    log("nats-server accepting connections")

    async def ready():
        return (await worker_health_ok(session)) == 200
    await wait_for(ready, timeout_s=60, desc="worker /health/ready -> 200")
    log("worker /health/ready -> 200 (reconnected)")

    status, body = await proxy_post(
        make_echo_payload(f"e2e-reconnect-{int(time.time())}", 3), session, timeout=60
    )
    if status != 200:
        raise AssertionError(f"recovery POST failed: status={status} body={body[:200]}")
    log("recovery: echo round-trip OK")


async def phase_dedup() -> None:
    import nats

    request_id = f"e2e-reconnect-dedup-{int(time.time())}"
    payload = make_echo_payload(f"e2e-dedup-{int(time.time())}", 7)
    request_json = json.dumps(
        {
            "request_id": request_id,
            "method": "POST",
            "path": "/",
            "body": json.dumps(payload),
            "query": "",
            "client_ip": "192.0.2.7",
            "proxy_ip": "192.0.2.8",
        }
    )

    nc = await nats.connect(nats_url())
    try:
        responses = []
        for round_trip in (1, 2):
            inbox = nc.new_inbox()
            future = asyncio.get_running_loop().create_future()

            async def on_msg(msg):
                if not future.done():
                    future.set_result(msg.data.decode())

            sub = await nc.subscribe(inbox, cb=on_msg)
            await asyncio.sleep(0.1)
            await nc.publish(NATS_SUBJECT, request_json.encode(), reply=inbox)
            try:
                data = await asyncio.wait_for(future, timeout=25)
            finally:
                await sub.unsubscribe()
            if not data:
                raise AssertionError(f"round-trip {round_trip}: no reply for {request_id}")
            responses.append(data)
            log(f"dedup round-trip {round_trip}: got reply ({len(data)} bytes)")

        if responses[1] != responses[0]:
            raise AssertionError("cached reply differs from the live reply")
        log("dedup: second delivery returned the identical cached reply")

        got = docker(["logs", "--since", "60s", WORKER_CONTAINER], timeout=30)
        marker = (
            "Duplicate NATS request detected, returning cached response: "
            f"request_id={request_id}"
        )
        if marker not in got.stdout:
            raise AssertionError(
                f"worker log does not contain dedup marker '{marker}'"
            )
        log("dedup: worker logged the duplicate-detection marker (no second L2 call)")
    finally:
        await nc.close()


async def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--phase", choices=PHASES, default="all",
        help="run a single phase ('outage' implies stopping nats-server)",
    )
    args = parser.parse_args()

    async with aiohttp.ClientSession() as session:
        if args.phase in ("all", "outage"):
            await wait_for(
                lambda: worker_health_ok(session), timeout_s=30, desc="worker healthy before test"
            )
            await run_baseline(session)
            await phase_outage(session)
        if args.phase in ("all", "recovery"):
            await phase_recovery(session)
        if args.phase in ("all", "dedup"):
            await phase_dedup()

    log("ALL PHASES PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except (TimeoutError, AssertionError) as e:
        log(f"FAILED: {e!r}")
        sys.exit(1)