#!/usr/bin/env python3
"""E2E smoke test of the HTTP DB Gateway (postgres, default stack).

Runs the read-only SQL gateway checks against a running compose stack:
  * GET  /v1/sql                    -> database list contains postgres (enabled);
  * GET  /v1/sql/postgres/ping      -> 200 ok;
  * POST /v1/sql/postgres/query     -> 200 ok with columns/rows (read-only);
  * POST /v1/sql/postgres/query     -> 400 BAD_REQUEST for a non-SELECT stmt;
  * POST /v1/sql/postgres/ping      -> 405 METHOD_NOT_ALLOWED;
  * GET  /v1/sql/oracle/ping        -> error (404 when unregistered, 503 when
                                       enabled-but-unreachable). A 504 timeout
                                       fails the gate: it signals the worker
                                       froze (regression of the blocking init).

Usage:
  scripts/db-gateway-e2e-test.py
  scripts/db-gateway-e2e-test.py --base-url http://localhost:29002
  scripts/db-gateway-e2e-test.py --parallel 25
  scripts/db-gateway-e2e-test.py --nats-restart   # recovery scenario

Exit code 0 = all checks passed, 1 = any check failed.
"""

import argparse
import asyncio
import json
import sys
import time
from pathlib import Path

import aiohttp

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import e2e_utils

REQUEST_TIMEOUT_SECONDS = 10
PROXY_BASE_URL = "http://localhost:29002"


async def http_get(url: str, session: aiohttp.ClientSession) -> tuple:
    return await e2e_utils.http_get(url, session, timeout=REQUEST_TIMEOUT_SECONDS)


async def http_post(url: str, body: dict, session: aiohttp.ClientSession) -> tuple:
    return await e2e_utils.http_post(url, body, session, timeout=REQUEST_TIMEOUT_SECONDS)


async def check_parallel(base, concurrency, queries_per_thread):
    """Runs `concurrency` concurrent queries with per-request unique markers
    and verifies each response carries the marker it asked for. Detects lost
    requests, cross-talk (1:1 NATS mapping) and pool saturation."""
    async def one(who):
        marker = who
        # SELECT <who> -- literal must round-trip unchanged through NATS.
        async with aiohttp.ClientSession() as session:
            code, body = await http_post(
                f"{base}/v1/sql/postgres/query",
                {"sql": f"SELECT {marker} AS marker"},
                session,
            )
        rows = body.get("rows") if isinstance(body, dict) else None
        if (code != 200 or not isinstance(body, dict) or
                body.get("status") != "ok" or
                not rows or len(rows[0]) != 1 or rows[0][0] != marker):
            return (False, f"{who}: got status {code}, row {(rows or [])[:1]}")
        return (True, f"{who}")

    results = await asyncio.gather(
        *(one(i + 1) for i in range(concurrency * queries_per_thread)))
    total = len(results)
    bad = [r for ok, r in results if not ok]
    return total, bad


def http_error_json(body) -> dict:
    """Parses the JSON error body returned as `body` (an HTTP error text)."""
    if isinstance(body, dict):
        return body
    try:
        return json.loads(body) if body else {}
    except Exception:  # noqa: BLE001
        return {}


async def baseline_checks(base, expect, session):
    """List / ping / query checks shared by the sequential and scenario
    runs. Needs the `expect(ok, message)` callback from the caller."""
    # 1. database list
    code, body = await http_get(f"{base}/v1/sql", session)
    expect(code == 200 and isinstance(body, dict)
           and isinstance(body.get("databases"), list),
           "GET /v1/sql returns the database list (200)")
    postgres = next(
        (d for d in (body.get("databases") if isinstance(body, dict) else [])
         if isinstance(d, dict) and d.get("name") == "postgres"),
        None,
    )
    expect(postgres is not None and postgres.get("enabled") is True,
           "postgres is registered and enabled")

    # 2. ping
    code, body = await http_get(f"{base}/v1/sql/postgres/ping", session)
    expect(code == 200 and isinstance(body, dict)
           and body.get("status") == "ok"
           and body.get("db") == "postgres",
           "GET /v1/sql/postgres/ping -> 200 ok")

    # 3. query
    code, body = await http_post(
        f"{base}/v1/sql/postgres/query",
        {"sql": "SELECT id FROM demo_messages ORDER BY id LIMIT 1"},
        session,
    )
    ok = (code == 200 and isinstance(body, dict)
          and body.get("status") == "ok"
          and isinstance(body.get("columns"), list)
          and isinstance(body.get("rows"), list)
          and body.get("row_count", -1) >= 1)
    expect(ok, "POST /v1/sql/postgres/query -> 200 ok with rows")


async def ping_ok(base, session) -> bool:
    """True when the gateway answers a postgres ping with 200 ok."""
    code, body = await http_get(f"{base}/v1/sql/postgres/ping", session)
    return code == 200 and isinstance(body, dict) and body.get("status") == "ok"


async def wait_gateway_recovery(base, expect, max_seconds, session):
    """Polls the postgres ping until it succeeds again. During the window any
    non-200/error response is acceptable (503/504 while NATS is down); only a
    gateway that never comes back fails the gate."""
    deadline = time.monotonic() + max_seconds
    while time.monotonic() < deadline:
        if await ping_ok(base, session):
            expect(True, "gateway recovered after the NATS restart")
            return True
        await asyncio.sleep(1.0)
    expect(False, f"gateway did not recover within {max_seconds}s")
    return False


async def run_nats_restart_scenario(base, expect, compose_dir, session):
    await baseline_checks(base, expect, session)
    compose_file = compose_dir / "docker-compose.yml"
    print(f"[SCENARIO] restarting NATS ({compose_file})...")
    result = await e2e_utils.docker(
        ["compose", "-f", str(compose_file), "restart", "nats-server"], timeout=120)
    if result.returncode != 0:
        expect(False, f"docker compose restart nats-server failed: "
                      f"{result.stderr.strip()[:300]}")
        return
    await wait_gateway_recovery(base, expect, max_seconds=90, session=session)
    await baseline_checks(base, expect, session)


async def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default=PROXY_BASE_URL)
    parser.add_argument("--parallel", type=int, default=0,
                        help="run N concurrent marker queries (default: 0 = "
                             "sequential mode only)")
    parser.add_argument("--nats-restart", action="store_true",
                        help="run the NATS-restart recovery scenario "
                             "(baseline -> restart NATS -> recovery -> baseline)")
    parser.add_argument("--compose-dir", default=str(repo_root),
                        help="directory with docker-compose.yml (default: "
                             "repo root)")
    args = parser.parse_args()
    base = args.base_url.rstrip("/")
    compose_dir = Path(args.compose_dir)

    failures = []
    checks = 0

    def expect(ok, message):
        nonlocal checks, failures
        checks += 1
        marker = "PASS" if ok else "FAIL"
        print(f"[{marker}] {message}")
        if not ok:
            failures.append(message)

    def is_504(code):
        return code == 504

    async with aiohttp.ClientSession() as session:
        if args.nats_restart:
            await run_nats_restart_scenario(base, expect, compose_dir, session)
            print(f"\nDB gateway E2E (NATS restart): "
                  f"{checks - len(failures)}/{checks} checks passed")
            return 1 if failures else 0

        await baseline_checks(base, expect, session)

        # 4. read-only gate (non-SELECT is rejected)
        code, body = await http_post(
            f"{base}/v1/sql/postgres/query",
            {"sql": "UPDATE demo_messages SET message = message"},
            session,
        )
        error_body = http_error_json(body)
        expect(code == 400 and error_body.get("error", {}).get("code")
               == "BAD_REQUEST",
               "UPDATE via query -> 400 BAD_REQUEST (read-only gate)")

        # 5. method gate (query=POST only)
        code, body = await http_post(f"{base}/v1/sql/postgres/ping", {}, session)
        error_body = http_error_json(body)
        expect(code == 405 and error_body.get("error", {}).get("code")
               == "METHOD_NOT_ALLOWED",
               "POST /v1/sql/postgres/ping -> 405 METHOD_NOT_ALLOWED")

        # 6. oracle: must never hang. 404 (not registered) or 503 (unreachable)
        #    are both fine; a 504 timeout means the worker loop froze.
        code, body = await http_get(f"{base}/v1/sql/oracle/ping", session)
        if is_504(code):
            expect(False, "GET /v1/sql/oracle/ping -> 504 TIMEOUT (worker "
                          "frozen)")
        elif code in (404, 503):
            expect(True, f"GET /v1/sql/oracle/ping -> {code} (oracle not "
                         "served)")
        else:
            expect(False, f"GET /v1/sql/oracle/ping -> unexpected HTTP "
                          f"{code}")

        # 7. parallelism / cross-talk gate (optional)
        if args.parallel and args.parallel > 0:
            total, bad = await check_parallel(base, args.parallel, 2)
            expect(not bad and total > 0,
                   f"{total} concurrent marker queries: all round-tripped "
                   f"uniquely" if not bad
                   else f"{total} concurrent queries -> {len(bad)} mismatches")
            for problem in bad[:5]:
                print(f"    mismatch: {problem}")

    print(f"\nDB gateway E2E: {checks - len(failures)}/{checks} checks passed")
    if failures:
        print("FAILURES:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("All DB gateway checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
