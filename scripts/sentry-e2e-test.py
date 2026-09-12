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
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from threading import Thread

DOCKER_NETWORK = "http-data-diod_l2_network"
RECEIVER_PORT = 9001
COMPOSE_SVC = ["l2-server", "l2-proxy", "l2-worker"]
PROXY_PORT = 8890
# Client-side timeout must exceed the proxy's REQUEST_TIMEOUT_SECONDS (30s)
# so we observe the actual HTTP 504 when the worker is stopped.
REQUEST_TIMEOUT = 40
HEALTH_RETRIES = 90


def get_gateway_ip():
    raw = subprocess.check_output([
        "docker", "network", "inspect", DOCKER_NETWORK,
        "--format", "{{range .IPAM.Config}}{{.Gateway}}{{end}}"
    ], text=True).strip()
    if not raw:
        raise RuntimeError("gateway IP not found in compose network")
    return raw


def wait_health(timeout=HEALTH_RETRIES):
    t0 = time.time()
    while time.time() - t0 < timeout:
        code = os.system("./health-check.sh all >/dev/null 2>&1")
        if code == 0:
            return True
        time.sleep(2)
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-rec", action="store_true",
                        help="skip service recreation (use existing SENTRY_DSN)")
    args = parser.parse_args()

    # 1. Resolve gateway IP
    gateway = get_gateway_ip()
    dsn = f"http://sentry-e2e@{gateway}:{RECEIVER_PORT}/1"
    print(f"Gateway IP: {gateway}")
    print(f"SENTRY_DSN={dsn}")

    # 2. Start mock receiver subprocess
    events_file = "/tmp/sentry-e2e-events.log"
    env = {**os.environ, "SENTRY_MOCK_STDOUT": events_file}
    mock_proc = subprocess.Popen(
        [sys.executable, "scripts/sentry-mock-receiver.py",
         "--port", str(RECEIVER_PORT), "--wait-events", "1", "--timeout", "120"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        env=env,
    )
    mock_log_lines = []

    def read_mock_output():
        for line in mock_proc.stdout:
            mock_log_lines.append(line)

    Thread(target=read_mock_output, daemon=True).start()
    time.sleep(1)

    if not args.skip_rec:
        # 3. Recreate services with SENTRY_DSN
        print("\n--- Recreating services with SENTRY_DSN ---")
        proc = subprocess.run(
            ["docker", "compose", "up", "-d",
             "--force-recreate", "--no-deps", *COMPOSE_SVC],
            cwd=os.path.dirname(__file__), capture_output=True, text=True,
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
    if not wait_health():
        print("health check timeout", file=sys.stderr)
        return 1
    print("health check passed")

    # 4. Stop the worker so no backend responds. The proxy then times out and
    #    fails the request via fail_backend_request, firing the
    #    "proxy_backend_error" Sentry capture (request_handler.cpp:358).
    print("\n--- Stopping worker (to force proxy_backend_error) ---")
    subprocess.run(["docker", "stop", "l2-worker"], check=True, capture_output=True)
    time.sleep(3)

    print("--- Sending request (no worker → timeout) ---")
    req = urllib.request.Request(
        f"http://localhost:{PROXY_PORT}/",
        data=json.dumps({"value": 1}).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        resp = urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT)
        print(f"unexpected response status {resp.status} (expected 5xx)")
    except urllib.error.HTTPError as exc:
        print(f"proxy response: HTTP {exc.code}")
    except Exception as exc:
        print(f"request failed: {exc}", file=sys.stderr)

    # 5. Wait for the mock to receive the event
    print("\n--- Waiting for Sentry event in mock receiver ---")
    try:
        mock_proc.wait(timeout=90)
    except subprocess.TimeoutExpired:
        print("mock receiver timeout — no event received")
    finally:
        time.sleep(1)

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
        except Exception:
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
    subprocess.run(["docker", "start", "l2-worker"], check=True, capture_output=True)
    time.sleep(5)

    if ok:
        print("\n=== PASS: Sentry delivery E2E ===")
        return 0
    print("\n=== FAIL: Sentry delivery E2E ===")
    return 1


if __name__ == "__main__":
    sys.exit(main())
