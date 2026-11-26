#!/usr/bin/env python3
"""E2E test of Sentry delivery via the mock receiver.

Proves that events captured in the l2-proxy/l2-worker actually reach a Sentry
ingest endpoint end-to-end (async queue + httplib POST + envelope parsing).

Trigger: a worker validation failure. A request that fails schema validation
in l2_worker.cpp::parse_request_data fires
  capture_message("Worker request validation failed: ...", request_id,
                  {"worker_validation_error", "schema"})
which is fully deterministic (no DB / postgres involvement needed).

Steps:
1. Start the mock Sentry receiver on port 9001.
2. Recreate l2-server / l2-proxy / l2-worker with SENTRY_DSN pointing at the
   host (via the compose network gateway IP so the container can reach it).
3. POST a schema-invalid request to the proxy (missing required field).
4. Verify the mock received a worker_validation_error event with the
   expected fingerprint and the request_id tag.

Usage:
  python3 scripts/sentry-e2e-test.py            # full run (default)
  python3 scripts/sentry-e2e-test.py --skip-rec # only with existing SENTRY_DSN

Exit code 0 = check passed, 1 = check failed.
"""

import argparse
import asyncio
import json
import os
import sys
import time
from pathlib import Path
from threading import Thread

import aiohttp

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import e2e_utils

DOCKER_NETWORK = "http-data-diod_l2_network"
RECEIVER_PORT = 9001
COMPOSE_SVC = ["l2-server", "l2-proxy", "l2-worker"]
PROXY_PORT = 29002
# Client-side timeout must exceed the proxy's REQUEST_TIMEOUT_SECONDS (30s)
# so we observe the actual HTTP 504 when the worker is stopped.
REQUEST_TIMEOUT = 40
HEALTH_RETRIES = 90


async def get_gateway_ip() -> str:
    result = await e2e_utils.docker(
        ["network", "inspect", DOCKER_NETWORK,
         "--format", "{{range .IPAM.Config}}{{.Gateway}}{{end}}"])
    raw = result.stdout.strip()
    if not raw:
        raise RuntimeError("gateway IP not found in compose network")
    return raw


async def wait_health(timeout: float = HEALTH_RETRIES) -> bool:
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        code = os.system("./health-check.sh all >/dev/null 2>&1")
        if code == 0:
            return True
        await asyncio.sleep(2)
    return False


async def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-rec", action="store_true",
                        help="skip service recreation (use existing SENTRY_DSN)")
    args = parser.parse_args()

    # 1. Resolve gateway IP
    gateway = await get_gateway_ip()
    dsn = f"http://sentry-e2e@{gateway}:{RECEIVER_PORT}/1"
    print(f"Gateway IP: {gateway}")
    print(f"SENTRY_DSN={dsn}")

    # 2. Start mock receiver subprocess
    events_file = "/tmp/sentry-e2e-events.log"
    env = {**os.environ, "SENTRY_MOCK_STDOUT": events_file}
    mock_proc = await asyncio.get_running_loop().run_in_executor(
        None,
        lambda: __import__("subprocess").Popen(
            [sys.executable, "scripts/sentry-mock-receiver.py",
             "--port", str(RECEIVER_PORT), "--wait-events", "1", "--timeout", "120"],
            stdout=__import__("subprocess").PIPE,
            stderr=__import__("subprocess").STDOUT, text=True,
            env=env,
        ),
    )
    mock_log_lines = []

    def read_mock_output():
        for line in mock_proc.stdout:
            mock_log_lines.append(line)

    Thread(target=read_mock_output, daemon=True).start()
    await asyncio.sleep(1)

    if not args.skip_rec:
        # 3. Recreate services with SENTRY_DSN (env must reach the compose
        #    build/up, so use subprocess directly — e2e_utils.docker does not
        #    forward env).
        print("\n--- Recreating services with SENTRY_DSN ---")
        import subprocess

        proc = subprocess.run(
            ["docker", "compose", "up", "-d",
             "--force-recreate", "--no-deps", *COMPOSE_SVC],
            cwd=str(REPO_ROOT), capture_output=True, text=True,
            env={**os.environ, "SENTRY_DSN": dsn},
        )
        print(proc.stdout)
        if proc.returncode != 0:
            print(f"compose up failed: {proc.stderr}", file=sys.stderr)
            return 1

        # nginx's `server l2-proxy:8888 resolve;` has no `resolver` directive,
        # so it caches the old proxy IP after recreation. Restarting nginx
        # forces a fresh DNS lookup (rebuild-and-run.sh avoids this by
        # `docker rm -f nginx` before `up`).
        print("\n--- Restarting nginx (re-resolve proxy IP) ---")
        subprocess.run(["docker", "restart", "nginx"],
                       check=True, capture_output=True)

    print("\n--- Waiting for health ---")
    if not await wait_health():
        print("health check timeout", file=sys.stderr)
        return 1
    print("health check passed")

    # 4. Stop the worker so no backend responds. The proxy then times out and
    #    fails the request via fail_backend_request, firing the
    #    "proxy_backend_error" Sentry capture (request_handler.cpp:358).
    print("\n--- Stopping worker (to force proxy_backend_error) ---")
    await e2e_utils.docker(["stop", "l2-worker"])
    await asyncio.sleep(3)

    print("--- Sending request (no worker → timeout) ---")
    async with aiohttp.ClientSession() as session:
        status, _ = await e2e_utils.http_post(
            f"http://localhost:{PROXY_PORT}/",
            {"value": 1},
            session,
            timeout=REQUEST_TIMEOUT,
        )
        print(f"proxy response: HTTP {status}")

    # 5. Wait for the mock to receive the event
    print("\n--- Waiting for Sentry event in mock receiver ---")
    try:
        await asyncio.get_running_loop().run_in_executor(
            None, mock_proc.wait, 90)
    except Exception as exc:
        print(f"mock receiver timeout — no event received: {exc}")
    finally:
        await asyncio.sleep(1)

    print("\n--- Mock log output ---")
    for line in mock_log_lines:
        print(line.rstrip())

    # 6. Parse the event
    event_found = False
    fingerprint_ok = False
    request_id_tag_ok = False

    for line in mock_log_lines:
        if not line.startswith("EVENT "):
            continue
        try:
            record = json.loads(line[6:])
        except Exception:  # noqa: BLE001
            continue
        payload = record.get("payload", {})
        tags = payload.get("tags", {})
        fp = payload.get("fingerprint", [])
        event_found = True
        print(f"  payload.level        = {payload.get('level')}")
        print(f"  payload.message      = {payload.get('message')}")
        print(f"  payload.fingerprint  = {fp}")
        print(f"  payload.tags         = {tags}")
        if "proxy_backend_error" in fp:
            fingerprint_ok = True
        if tags.get("service", "") == "proxy":
            request_id_tag_ok = True

    print(f"\nEvent found:        {event_found}")
    print(f"Fingerprint ok:     {fingerprint_ok}")
    print(f"Service tag ok:     {request_id_tag_ok}")

    ok = event_found and fingerprint_ok and request_id_tag_ok

    # 7. Restore the worker
    print("\n--- Restarting worker ---")
    await e2e_utils.docker(["start", "l2-worker"])
    await asyncio.sleep(5)

    if ok:
        print("\n=== PASS: Sentry delivery E2E ===")
        return 0
    print("\n=== FAIL: Sentry delivery E2E ===")
    return 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
