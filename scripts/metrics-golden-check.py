#!/usr/bin/env python3
"""Golden-set check for Prometheus metrics exported by the l2 stack.

Fetches the current metric-name set from VictoriaMetrics and asserts that every
family from the README catalog (section "Метрики Prometheus (полный каталог)")
is present. A family is considered present when either its bare name or any
histogram derivative (_bucket/_sum/_count) appears in the label values — this
guards against regressions were a metric silently disappears from the code
(such as the removed `skipped_transactions`).

For families that only appear after specific traffic (`--all`, e.g. the dynamic
per-client-id duplicate detectors) presence is checked additionally.

With `--traffic` the script also asserts that the core happy-path counters are
non-zero (>= 0 over the last TRAFFIC_WINDOW) — meant to be run after
`python3 message_counter.py --iterations 1 --concurrent 1`.

Exit code is 0 on success and 1 when any required metric is missing.
"""

import argparse
import json
import os
import sys
import time
import urllib.parse
import urllib.request

# Base family names from the README catalog (histogram families are described
# by their bare name here; the script checks the _bucket/_sum/_count triplets).
CATALOG = [
    # l2-proxy
    "l2_proxy_client_requests_total",
    "l2_proxy_nats_requests_total",
    "l2_proxy_client_request_errors_total",
    "l2_proxy_nats_errors_total",
    "l2_proxy_nats_connection_creates_total",
    "l2_proxy_nats_connection_errors_total",
    "l2_proxy_nats_request_duration_seconds",
    "l2_proxy_bytes_received_total",
    "l2_proxy_bytes_sent_total",
    "l2_proxy_request_duration_seconds",
    "l2_proxy_request_size_bytes",
    "l2_proxy_response_size_bytes",
    "l2_proxy_duplicate_requests_total",
    "l2_proxy_duplicate_posts_detected_total",
    "l2_proxy_responses_total",
    "l2_proxy_in_flight_requests",
    "l2_proxy_nats_connected",
    "l2_proxy_health_ready",
    # HTTP client pool (proxy registry)
    "l2_http_pool_active_clients",
    "l2_http_pool_available_clients",
    "l2_http_pool_client_acquisitions_total",
    "l2_http_pool_client_releases_total",
    "l2_http_pool_stale_evictions_total",
    # Rate limiter (proxy)
    "l2_rate_limiter_tokens",
    "l2_rate_limiter_rejected_total",
    "l2_per_ip_rate_limiter_rejected_total",
    "l2_proxy_per_ip_rate_limiter_ips_tracked",
    "l2_proxy_per_ip_requests_total",
    "l2_proxy_per_ip_rejected_total",
    "l2_proxy_per_client_id_requests_total",
    "l2_proxy_per_client_id_rejected_total",
    "l2_proxy_per_client_id_latency_seconds",
    # l2-worker
    "l2_worker_requests_processed_total",
    "l2_worker_l2_calls_total",
    "l2_worker_l2_errors_total",
    "l2_worker_bytes_received_total",
    "l2_worker_bytes_sent_total",
    "l2_worker_request_duration_seconds",
    "l2_worker_l2_call_duration_seconds",
    "l2_worker_processing_json_errors_total",
    "l2_worker_processing_validation_errors_total",
    "l2_worker_l2_response_size_bytes",
    "l2_worker_circuit_breaker_state",
    "l2_worker_duplicate_requests_total",
    "l2_worker_db_pool_connections",
    "l2_worker_db_gateway_ready",
    "l2_worker_responses_total",
    "l2_worker_in_flight_requests",
    "l2_worker_queue_size",
    "l2_worker_nats_connected",
    "l2_worker_health_ready",
    "l2_worker_sentry_events_sent_total",
    "l2_worker_sentry_events_failed_total",
    "l2_worker_sentry_queue_size",
    # l2-server
    "l2_server_requests_total",
    "l2_server_request_errors_total",
    "l2_server_bytes_received_total",
    "l2_server_bytes_sent_total",
    "l2_server_request_duration_seconds",
    "l2_server_responses_total",
    "l2_server_health_ready",
    # Distributed tracing (worker registry)
    "l2_tracing_spans_sent_total",
    "l2_tracing_spans_failed_total",
    "l2_tracing_queue_size",
    "l2_tracing_last_send_duration_seconds",
    "l2_tracing_send_latency_seconds",
    "l2_tracing_queue_time_seconds",
]

# Families emitted lazily by DynamicLabeledFamily: checked under --all only.
CONDITIONAL = [
    "l2_proxy_per_client_id_duplicate_requests_total",
    "l2_proxy_per_client_id_duplicate_rejected_total",
]

# Core happy-path counters asserted to be non-zero over the traffic window.
TRAFFIC_QUERIES = [
    "l2_proxy_client_requests_total",
    "l2_proxy_nats_requests_total",
    "l2_server_requests_total",
    "l2_server_responses_total",
    "l2_worker_requests_processed_total",
    "l2_worker_l2_calls_total",
    "l2_worker_responses_total",
    "l2_proxy_responses_total",
    "l2_tracing_spans_sent_total",
]

TRAFFIC_WINDOW = "5m"

TRAFFIC_TIMEOUT_S = 60
TRAFFIC_POLL_INTERVAL_S = 2


def fetch_json(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=15) as resp:
        return json.load(resp)


def label_values(vm_url: str) -> set:
    endpoint = urllib.parse.urljoin(vm_url.rstrip("/") + "/",
                                    "api/v1/label/__name__/values")
    data = fetch_json(endpoint)
    return set(data["data"])


def family_present(names: set, family: str) -> bool:
    if family in names:
        return True
    return any(names.intersection({family + s for s in ("_bucket", "_sum", "_count")}))


def query_samples(vm_url: str, name: str) -> bool:
    expr = f'last_over_time({name}[{TRAFFIC_WINDOW}])'
    url = vm_url.rstrip("/") + "/api/v1/query?" + urllib.parse.urlencode(
        {"query": expr})
    data = fetch_json(url)
    return bool(data.get("data", {}).get("result", []))


def check_traffic(vm_url: str) -> list:
    # Scrapes are timeseries-level async: a request just completed, but the
    # vmagent/VM scrape that records it may lag behind by one interval — poll
    # until every counter is observable (or the timeout expires) instead of
    # failing on the very first instant after message_counter.
    deadline = time.monotonic() + TRAFFIC_TIMEOUT_S
    while True:
        missing = [n for n in TRAFFIC_QUERIES if not query_samples(vm_url, n)]
        if not missing:
            return []
        if time.monotonic() >= deadline:
            return [f"{n}: no samples in the last {TRAFFIC_WINDOW}" for n in missing]
        time.sleep(TRAFFIC_POLL_INTERVAL_S)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default=os.environ.get("VICTORIA_METRICS_URL", "http://localhost:8428"),
                        help="VictoriaMetrics URL (default http://localhost:8428)")
    parser.add_argument("--traffic", action="store_true",
                        help="also require the core happy-path counters to be non-zero")
    parser.add_argument("--all", action="store_true",
                        help="also require lazily-emitted families (per-client-id duplicate detectors)")
    args = parser.parse_args()

    failures = []
    try:
        names = label_values(args.url)
    except Exception as exc:
        print(f"ERROR: cannot query VictoriaMetrics {args.url}: {exc}")
        return 1

    required = CATALOG + (CONDITIONAL if args.all else [])
    missing = [f for f in required if not family_present(names, f)]
    if missing:
        failures.append(f"missing families: {', '.join(sorted(missing))}")

    if args.traffic:
        failures.extend(check_traffic(args.url))

    per_service = {}
    for family in CATALOG:
        service = family.split("_")[1]
        per_service.setdefault(service, 0)
        per_service[service] += 1

    if failures:
        print("FAIL: golden metrics set incomplete")
        for item in failures:
            print("  -", item)
        return 1

    present = sum(1 for f in required if family_present(names, f))
    print(f"OK: golden metrics set complete "
          f"({present}/{len(required)} families, "
          + ", ".join(f"{k.upper()}={v}" for k, v in sorted(per_service.items()))
          + ")")
    if args.traffic:
        print("OK: core happy-path counters are non-zero "
              f"(last {TRAFFIC_WINDOW})")
    return 0


if __name__ == "__main__":
    sys.exit(main())