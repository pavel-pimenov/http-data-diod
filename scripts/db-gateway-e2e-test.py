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
  scripts/db-gateway-e2e-test.py --base-url http://localhost:8888

Exit code 0 = all checks passed, 1 = any check failed.
"""

import argparse
import concurrent.futures
import json
import sys
import urllib.error
import urllib.request

REQUEST_TIMEOUT_SECONDS = 10
PROXY_BASE_URL = "http://localhost:8888"

# Positive checks return True on success; negative checks expect the given
# HTTP status and error code (or one of the accepted alternatives).


def http_get(url):
    request = urllib.request.Request(url)
    return _open(request)


def http_post(url, body):
    request = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    return _open(request)


def _open(request):
    response = urllib.request.urlopen(request, timeout=REQUEST_TIMEOUT_SECONDS)
    try:
        return response.status, json.loads(response.read().decode("utf-8"))
    finally:
        response.close()


def check_parallel(base, concurrency, queries_per_thread):
    """Runs `concurrency` concurrent queries with per-request unique markers
    and verifies each response carries the marker it asked for. Detects lost
    requests, cross-talk (1:1 NATS mapping) and pool saturation."""
    results = []

    def one(who):
        marker = who
        # SELECT <who> -- literal must round-trip unchanged through NATS.
        code, body = http_post(
            f"{base}/v1/sql/postgres/query",
            {"sql": f"SELECT {marker} AS marker"},
        )
        rows = body.get("rows")
        if (code != 200 or body.get("status") != "ok" or
                not rows or len(rows[0]) != 1 or rows[0][0] != marker):
            return (False, f"{who}: got status {code}, row {(rows or [])[:1]}")
        return (True, f"{who}")

    with concurrent.futures.ThreadPoolExecutor(
            max_workers=concurrency) as pool:
        futures = []
        for _ in range(concurrency * queries_per_thread):
            futures.append(pool.submit(one, len(futures) + 1))
        for future in concurrent.futures.as_completed(futures):
            results.append(future.result())
    total = len(results)
    bad = [r for ok, r in results if not ok]
    return total, bad


def http_error_json(error):
    """Parses the JSON body of an HTTPError (empty -> error body shape)."""
    try:
        return json.loads(error.read().decode("utf-8"))
    except Exception:  # noqa: BLE001
        return {}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default=PROXY_BASE_URL)
    parser.add_argument("--parallel", type=int, default=0,
                        help="run N concurrent marker queries (default: 0 = "
                             "sequential mode only)")
    args = parser.parse_args()
    base = args.base_url.rstrip("/")

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

    # 1. database list
    try:
        code, body = http_get(f"{base}/v1/sql")
        expect(code == 200 and isinstance(body.get("databases"), list),
               "GET /v1/sql returns the database list (200)")
        db_names = [d.get("name") for d in body.get("databases", [])]
        postgres = next(
            (d for d in body.get("databases", []) if d.get("name") == "postgres"),
            None,
        )
        expect(postgres is not None and postgres.get("enabled") is True,
               "postgres is registered and enabled")
    except urllib.error.HTTPError as e:
        expect(False, f"GET /v1/sql failed: HTTP {e.code}")
    except Exception as e:  # noqa: BLE001
        expect(False, f"GET /v1/sql failed: {e}")

    # 2. ping
    try:
        code, body = http_get(f"{base}/v1/sql/postgres/ping")
        expect(code == 200 and body.get("status") == "ok"
               and body.get("db") == "postgres",
               "GET /v1/sql/postgres/ping -> 200 ok")
    except urllib.error.HTTPError as e:
        expect(False, f"ping failed: HTTP {e.code} ({e.read()[:200]})")
    except Exception as e:  # noqa: BLE001
        expect(False, f"ping failed: {e}")

    # 3. query
    try:
        code, body = http_post(
            f"{base}/v1/sql/postgres/query",
            {"sql": "SELECT id FROM demo_messages ORDER BY id LIMIT 1"},
        )
        ok = (code == 200 and body.get("status") == "ok"
              and isinstance(body.get("columns"), list)
              and isinstance(body.get("rows"), list)
              and body.get("row_count", -1) >= 1)
        expect(ok, "POST /v1/sql/postgres/query -> 200 ok with rows")
    except urllib.error.HTTPError as e:
        expect(False, f"query failed: HTTP {e.code} ({e.read()[:200]})")
    except Exception as e:  # noqa: BLE001
        expect(False, f"query failed: {e}")

    # 4. read-only gate (non-SELECT is rejected)
    try:
        code, body = http_post(
            f"{base}/v1/sql/postgres/query",
            {"sql": "UPDATE demo_messages SET message = message"},
        )
        expect(False, "UPDATE via query unexpectedly accepted (status "
                      f"{code})")
    except urllib.error.HTTPError as e:
        error_body = http_error_json(e)
        expect(e.code == 400 and error_body.get("error", {}).get("code")
               == "BAD_REQUEST",
               "UPDATE via query -> 400 BAD_REQUEST (read-only gate)")
    except Exception as e:  # noqa: BLE001
        expect(False, f"read-only gate failed: {e}")

    # 5. method gate (query=POST only)
    try:
        code, body = http_post(f"{base}/v1/sql/postgres/ping", {})
        expect(False, "POST ping unexpectedly accepted (status "
                      f"{code})")
    except urllib.error.HTTPError as e:
        error_body = http_error_json(e)
        expect(e.code == 405 and error_body.get("error", {}).get("code")
               == "METHOD_NOT_ALLOWED",
               "POST /v1/sql/postgres/ping -> 405 METHOD_NOT_ALLOWED")
    except Exception as e:  # noqa: BLE001
        expect(False, f"method gate failed: {e}")

    # 6. oracle: must never hang. 404 (not registered) or 503 (unreachable)
    #    are both fine; a 504 timeout means the worker loop froze.
    try:
        code, body = http_get(f"{base}/v1/sql/oracle/ping")
        if is_504(code):
            expect(False, "GET /v1/sql/oracle/ping -> 504 TIMEOUT (worker "
                          "frozen)")
        elif code in (404, 503):
            expect(True, f"GET /v1/sql/oracle/ping -> {code} (oracle not "
                         "served)")
        else:
            expect(False, f"GET /v1/sql/oracle/ping -> unexpected HTTP "
                          f"{code}")
    except urllib.error.HTTPError as e:
        if e.code in (404, 503):
            expect(True, f"GET /v1/sql/oracle/ping -> HTTP {e.code} (oracle "
                         "not served)")
        else:
            expect(False, f"oracle ping failed: HTTP {e.code}")
    except Exception as e:  # noqa: BLE001
        expect(False, f"oracle ping failed: {e}")

    # 7. parallelism / cross-talk gate (optional)
    if args.parallel and args.parallel > 0:
        try:
            total, bad = check_parallel(base, args.parallel, 2)
            expect(not bad and total > 0,
                   f"{total} concurrent marker queries: all round-tripped "
                   f"uniquely" if not bad
                   else f"{total} concurrent queries -> {len(bad)} mismatches")
            if bad:
                for problem in bad[:5]:
                    print(f"    mismatch: {problem}")
        except Exception as e:  # noqa: BLE001
            expect(False, f"parallel gate failed: {e}")

    print(f"\nDB gateway E2E: {checks - len(failures)}/{checks} checks passed")
    if failures:
        print("FAILURES:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("All DB gateway checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
