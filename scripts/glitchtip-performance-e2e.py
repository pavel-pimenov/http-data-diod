#!/usr/bin/env python3
"""E2E check that spans delivered as Sentry transactions land in GlitchTip.

End-to-end through the real binaries: a normal message sequence
(proxy -> worker -> NATS) creates spans on l2-proxy/l2-worker, which are
delivered to the GlitchTip /envelope/ endpoint (when tracing is enabled and
SENTRY_DSN is set). This script asserts that new performance transaction
groups appear in the GlitchTip DB (ingest is async, so it polls).

Steps:
1. Baseline the per-transaction group counts (GlitchTip postgres).
2. Run message_counter.py to generate traffic and spans.
3. Poll until the group counts grow and the expected base transactions are
   present.
4. Exit 0 = check passed, 1 = check failed.

Usage:
  python3 scripts/glitchtip-performance-e2e.py
  python3 scripts/glitchtip-performance-e2e.py --iterations 3 --concurrent 2
"""

import argparse
import subprocess
import sys
import time

DB_CONTAINER = "glitchtip-db"
DB_PSQL = [
    "sh", "-c",
    "psql -U glitchtip -d glitchtip -tAc "
    "\"select transaction||'|'||op||'|'||count from performance_transactiongroup\"",
]

EXPECTED_BASE_TRANSACTIONS = {
    # (transaction, op) pairs that a basic message sequence must produce.
    # Names are prefixed with the runtime MODE so Transaction Groups stay
    # separated per product (proxy / worker / l2-server).
    "proxy: HTTP INCOMING /|http.server",
    "proxy: HTTP POST /|http.server",
    "worker: HTTP NATS_consume /nats|messaging",
    "worker: HTTP NATS_push /nats|messaging",
    "worker: HTTP NATS_poll /nats|messaging",
}


def psql_groups():
    proc = subprocess.run(
        ["docker", "exec", DB_CONTAINER] + DB_PSQL,
        capture_output=True, text=True, check=True)
    groups = {}
    for line in proc.stdout.splitlines():
        if not line.strip():
            continue
        key, _, count = line.rpartition("|")
        groups[key] = int(count)
    return groups


def run_message_counter(iterations, concurrent, timeout):
    subprocess.run(
        ["python3", "message_counter.py", "--iterations", str(iterations),
         "--concurrent", str(concurrent)],
        check=True, timeout=timeout)


MIN_BASE_GROWN = 3  # tolerance: not every base tx is hit in one run


def wait_groups(baseline, timeout):
    """Poll the GlitchTip DB until group counts grow past the baseline."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        groups = psql_groups()
        base_grown = sum(groups.get(k, 0) > baseline.get(k, 0)
                         for k in EXPECTED_BASE_TRANSACTIONS)
        total_grown = sum(v for v in groups.values()) > \
            sum(v for v in baseline.values())
        if base_grown >= MIN_BASE_GROWN and total_grown:
            return groups, time.time() - t0
        time.sleep(2)
    return None, time.time() - t0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iterations", type=int, default=1)
    parser.add_argument("--concurrent", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=60,
                        help="max seconds to wait for GlitchTip ingest")
    args = parser.parse_args()

    try:
        baseline = psql_groups()
    except subprocess.CalledProcessError:
        print("ERROR: can't reach GlitchTip postgres "
              "({}) — is the stack up?".format(DB_CONTAINER))
        return 1

    print("Baseline groups in GlitchTip: {}".format(len(baseline)))

    try:
        run_message_counter(args.iterations, args.concurrent,
                            timeout=120)
    except subprocess.CalledProcessError as exc:
        print("ERROR: message_counter failed: {}".format(exc))
        return 1

    groups, elapsed = wait_groups(baseline, args.timeout)
    if groups is None:
        print("FAIL: expected transaction groups did not grow after "
              "{}s".format(args.timeout))
        current = psql_groups()
        print("  baseline keys present now:")
        shown = set()
        for key in EXPECTED_BASE_TRANSACTIONS:
            if key in current:
                shown.add(key)
                print("    {} count={} (was {})".format(
                    key, current[key], baseline.get(key, 0)))
        missing = EXPECTED_BASE_TRANSACTIONS - shown
        print("  missing: {}".format(", ".join(sorted(missing))))
        return 1

    print("PASS: GlitchTip transaction groups grew from {} to {} rows "
          "({:.0f}s)".format(len(baseline), len(groups), elapsed))
    for key in sorted(EXPECTED_BASE_TRANSACTIONS):
        print("  {} count={} (was {})".format(key, groups.get(key, 0),
                                              baseline.get(key, 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
