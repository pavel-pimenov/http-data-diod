#!/usr/bin/env python3
# Requires Python 3.7+; /usr/bin/python may point to Python 2 on some systems.
"""
Script to send POST requests to HTTP proxy and verify response-to-request correlation.

POST test: Each request sends {"value": <unique_req_id>, ...} and expects
{"value_return": <unique_req_id>} in the response. Because the value is unique
per request, a client can tell whether it received ITS response or a response
intended for a different client (crossed response detection).

The JSON body is intentionally small and flat (default ~10 KB, matching the
10-30 KB response size seen in production).

GET test: Requests /favicon.ico and verifies binary data integrity (Content-Type: image/x-icon).
"""

import sys

if sys.version_info < (3, 7):
    sys.stderr.write(
        "message_counter.py requires Python 3.7+.\n"
        "Detected: {}.{}.{}\n"
        "Run it with python3, for example:\n"
        "  python3 message_counter.py --iterations 1 --concurrent 1\n".format(
            sys.version_info[0], sys.version_info[1], sys.version_info[2]
        )
    )
    sys.exit(1)

import asyncio
import aiohttp
import time
import argparse
import logging
import random
import json
import hashlib
import signal
import os
from urllib.parse import urlparse
from typing import Optional, Tuple, Any, List, Dict

# Set up logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.StreamHandler(sys.stdout)
    ]
)
logger = logging.getLogger(__name__)
# ============================================================================
# Configuration
# ============================================================================

# Logging progress interval (number of requests between log messages)
LOG_PROGRESS_INTERVAL = 10

# Default body size for generated JSON (in megabytes)
DEFAULT_BODY_SIZE_MB = 0.01

# Request timeout in seconds (overridable via --timeout)
REQUEST_TIMEOUT = 30

# SSL verification (set to False for self-signed certificates)
SSL_VERIFY = False

# Graceful shutdown flag set by SIGINT/SIGTERM
g_shutdown = False

# JSON size limits (in megabytes)
MAX_JSON_SIZE_MB = 1.0      # Warning threshold
CRITICAL_JSON_SIZE_MB = 10.0  # Critical threshold - will reduce body size

# Path for GET test (favicon)
FAVICON_PATH = '/favicon.ico'

# Default command line argument values (should match parse_args defaults)
DEFAULT_URL = os.environ.get('TEST_BASE_URL', 'http://nginx' if os.path.exists('/.dockerenv') else 'http://localhost:7777')
DEFAULT_ITERATIONS = 100
DEFAULT_CONCURRENT = 20

# HTTP client connection pool settings
MAX_CONNECTIONS = 100
CONNECTOR_LIMIT = 100

# ============================================================================


def should_use_ssl(url: str) -> bool:
    return urlparse(url).scheme.lower() == 'https' and SSL_VERIFY


def normalize_runtime_url(url: str) -> str:
    parsed_url = urlparse(url)
    if parsed_url.hostname != 'localhost':
        return url

    netloc = parsed_url.netloc
    if parsed_url.port is not None:
        netloc = f"127.0.0.1:{parsed_url.port}"
    else:
        netloc = '127.0.0.1'
    return parsed_url._replace(netloc=netloc).geturl()


async def resolve_runtime_url(url: str) -> str:
    parsed_url = urlparse(url)
    if parsed_url.scheme.lower() != 'http' or parsed_url.hostname != 'localhost' or parsed_url.port != 7777:
        return normalize_runtime_url(url)

    try:
        reader, writer = await asyncio.wait_for(asyncio.open_connection('127.0.0.1', 7777), timeout=1.0)
        writer.write(b"GET /nginx-health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        await writer.drain()
        response = await asyncio.wait_for(reader.read(64), timeout=1.0)
        writer.close()
        await writer.wait_closed()
        if response.startswith(b"HTTP/1.1 200") or response.startswith(b"HTTP/1.0 200"):
            return normalize_runtime_url(url)
    except Exception:
        pass

    if os.path.exists('/.dockerenv'):
        fallback_url = 'http://nginx'
    else:
        fallback_url = 'http://127.0.0.1:8888'
    logger.warning(
        "localhost:7777 is not responding over HTTP; using l2-proxy fallback %s",
        fallback_url)
    return fallback_url


def generate_random_body(max_size_mb: float = 0.01) -> str:
    """
    Generate a simple, flat JSON echo body of approximately the requested size.

    The structure is intentionally minimal — the test verifies response-to-request
    correlation, not metric realism. The default ~10 KB matches the 10-30 KB
    response size observed in production.

    Args:
        max_size_mb: Maximum size in megabytes (default: 0.01 for ~10KB)

    Returns:
        JSON string with a simple flat structure
    """
    max_bytes = max(int(max_size_mb * 1024 * 1024), 256)
    min_size = 256  # Keep small bodies simple and fast
    target_size = random.randint(min_size, max_bytes)

    payload = {
        "value": 0,
        "client_id": f"client-{random.randint(1, 10000)}",
        "request_type": "echo",
        "version": "1.0.0",
        "timestamp": time.time(),
    }

    # Pad to the target size with a flat array of integers (no deep nesting)
    base_size = len(json.dumps(payload))
    if base_size < target_size:
        sample_count = (target_size - base_size) // 6  # ~6 bytes per integer
        payload["samples"] = [random.randint(0, 1000000) for _ in range(sample_count)]

    json_str = json.dumps(payload)
    logger.debug(f"Generated JSON echo body of size: {len(json_str.encode('utf-8')) / 1024:.2f} KB")
    return json_str


class Colors:
    """ANSI color codes for terminal output."""
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    NC = '\033[0m'  # No Color


async def make_request(session: aiohttp.ClientSession, url: str,
                       payload: dict) -> Dict[str, Any]:
    """
    Make a single POST request to the specified URL with the given payload.

    The payload carries a unique ``value`` and ``req_id`` per request; the
    backend echoes them back as ``value_return``/``req_id`` together with
    ``req_hash`` (SHA-256 of the exact request body). The response is accepted
    only when all three match the sent values — otherwise the client received
    a response intended for a different request (crossed/corrupted response).

    The ``X-Correlation-Test: 1`` header opts the request into the
    correlation-test echo on the l2-server (req_id + req_hash). Regular
    production clients don't send it, so the backend stays a plain value echo
    for them.

    Args:
        session: aiohttp client session
        url: Target URL for the request
        payload: JSON payload to send

    Returns:
        dict with keys:
            success: bool — request succeeded and response matched
            value_return: Optional[int] — echoed value on success
            error: Optional[str] — error description
            mismatch: bool — response belonged to a different request
    """
    try:
        use_ssl = should_use_ssl(url)
        headers = {'Content-Type': 'application/json',
                   'X-Correlation-Test': '1',
                   'X-DataHub-Client-Id': str(payload.get("client_id", "test-client"))}
        # Serialize once and send as raw bytes so the server hashes exactly
        # what we hash client-side.
        payload_json = json.dumps(payload)
        payload_size_kb = len(payload_json.encode('utf-8')) / 1024

        logger.debug(f"Sending request with payload size: {payload_size_kb:.1f} KB")

        expected_value = payload.get("value")
        expected_req_id = str(payload.get("req_id"))
        expected_hash = hashlib.sha256(payload_json.encode('utf-8')).hexdigest()

        async with session.post(url, data=payload_json, headers=headers, timeout=aiohttp.ClientTimeout(total=REQUEST_TIMEOUT), ssl=use_ssl) as response:
            if response.status == 200:
                response_data = await response.json()
                value_return = response_data.get("value_return")
                resp_req_id = response_data.get("req_id")
                resp_hash = response_data.get("req_hash")
                if (value_return == expected_value
                        and resp_req_id == expected_req_id
                        and resp_hash == expected_hash):
                    return {"success": True, "value_return": value_return,
                            "error": None, "mismatch": False}
                details = []
                if value_return != expected_value:
                    details.append(f"value_return={value_return} != {expected_value}")
                if resp_req_id != expected_req_id:
                    details.append(f"req_id={resp_req_id} != {expected_req_id}")
                if resp_hash != expected_hash:
                    details.append(f"req_hash mismatch ({resp_hash} != {expected_hash})")
                return {"success": False, "value_return": None,
                        "error": "response mismatch: " + "; ".join(details),
                        "mismatch": True}
            else:
                # Read error response for better diagnostics
                try:
                    error_text = await response.text()
                    error_msg = f"HTTP {response.status}: {error_text[:200]}"
                    logger.warning(f"Received status code {response.status}: {error_text[:200]}...")
                except Exception:
                    error_msg = f"HTTP {response.status}"
                    logger.warning(f"Received status code {response.status}")
                return {"success": False, "value_return": None,
                        "error": error_msg, "mismatch": False}
    except asyncio.TimeoutError:
        logger.error("Request timed out")
        return {"success": False, "value_return": None, "error": "Timeout",
                "mismatch": False}
    except aiohttp.ClientError as e:
        logger.error(f"Client error: {e}")
        return {"success": False, "value_return": None, "error": str(e),
                "mismatch": False}
    except Exception as e:
        logger.error(f"Unexpected error: {e}")
        return {"success": False, "value_return": None, "error": str(e),
                "mismatch": False}


async def limited_request(session: aiohttp.ClientSession, semaphore: asyncio.Semaphore,
                          url: str, base_payload: dict,
                          req_id: int,
                          body_sizes_kb: Optional[List[int]] = None,
                          client_id: Optional[str] = None,
                          hot_clients: int = 0,
                          hot_share: float = 0.8) -> Tuple[int, Dict[str, Any], float]:
    """
    Make a request with concurrency limiting.

    Each request gets a unique ``value``/``req_id`` injected so the response
    can be attributed to the exact request that produced it. When
    ``body_sizes_kb`` is given, a fresh body of a random size from the list is
    generated per request (production-like mix of small/large payloads).

    Args:
        session: aiohttp client session
        semaphore: Concurrency limiter
        url: Target URL
        base_payload: Base JSON payload (cloned per request when no body_sizes_kb)
        req_id: Request identifier
        body_sizes_kb: Optional list of body sizes in KB to mix per request
        client_id: Optional pinned client id (sets payload client_id and the
            X-DataHub-Client-Id header); overrides the random per-request id
        hot_clients: Number of "hot" client ids (hot-client-1..N) that get the
            lion's share of requests, emulating a few heavy consumers behind
            one IP in Grafana (0 disables the emulation)
        hot_share: Fraction of requests assigned to the hot clients
            (default 0.8)

    Returns:
        Tuple of (request_id, result, latency_ms)
    """
    async with semaphore:
        if body_sizes_kb:
            size_kb = random.choice(body_sizes_kb)
            payload = json.loads(generate_random_body(size_kb / 1024.0))
        else:
            payload = dict(base_payload)
        resolved_client_id = client_id
        if resolved_client_id is None and hot_clients > 0:
            if random.random() < hot_share:
                resolved_client_id = f"hot-client-{random.randint(1, hot_clients)}"
            else:
                resolved_client_id = f"client-{random.randint(1, 10000)}"
        if resolved_client_id is not None:
            payload["client_id"] = resolved_client_id
        payload["value"] = req_id
        payload["req_id"] = str(req_id)
        t0 = time.monotonic()
        result = await make_request(session, url, payload)
        latency_ms = (time.monotonic() - t0) * 1000.0
        return req_id, result, latency_ms


def process_result(result: Dict[str, Any], req_id: int) -> Tuple[int, int, int, int]:
    """
    Process a request result and return statistics.

    Args:
        result: Result from make_request
        req_id: Request identifier

    Returns:
        Tuple of (value_to_add, successful_requests, failed_requests, mismatched_requests)
    """
    if result.get("success"):
        return result.get("value_return", 0), 1, 0, 0
    elif result.get("mismatch"):
        logger.warning(f"Request {req_id}: {result.get('error')}")
        return 0, 0, 1, 1
    else:
        logger.warning(f"Request {req_id}: Failed: {result.get('error')}")
        return 0, 0, 1, 0


def compute_latency_stats(latencies: List[float]) -> Dict[str, float]:
    """Compute p50/p95/p99/avg/min/max latency statistics from raw latencies (ms)."""
    stats = {"p50_ms": 0.0, "p95_ms": 0.0, "p99_ms": 0.0,
             "avg_ms": 0.0, "min_ms": 0.0, "max_ms": 0.0}
    if not latencies:
        return stats
    sorted_lat = sorted(latencies)
    n = len(sorted_lat)

    def pct(p: float) -> float:
        return sorted_lat[min(n - 1, int(p * n))]

    stats["p50_ms"] = pct(0.50)
    stats["p95_ms"] = pct(0.95)
    stats["p99_ms"] = pct(0.99)
    stats["avg_ms"] = sum(latencies) / n
    stats["min_ms"] = sorted_lat[0]
    stats["max_ms"] = sorted_lat[-1]
    return stats


def _install_signal_handlers() -> None:
    def _handler(signum, frame):
        global g_shutdown
        if not g_shutdown:
            g_shutdown = True
            logger.warning(f"Received signal {signum}, draining in-flight requests...")
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            signal.signal(sig, _handler)
        except ValueError:
            pass  # not in main thread (e.g. pytest)

async def run_test(url: str, iterations: int, concurrent: int, base_payload: dict,
                   body_sizes_kb: Optional[List[int]] = None,
                   client_id: Optional[str] = None,
                   hot_clients: int = 0,
                   hot_share: float = 0.8,
                   duration_seconds: Optional[float] = None) -> dict:
    """
    Run the response-to-request correlation test.

    Every request carries a unique ``value``/``req_id`` and a SHA-256 ``req_hash``
    of its body. The backend echoes them back, so a successful test proves each
    client received exactly the response intended for it — a crossed or corrupted
    response would surface as a mismatch.

    Requests are kept in flight continuously: at most ``concurrent`` are in
    flight at once, and a new one is launched as soon as one completes. The test
    stops either after ``iterations`` requests (fixed mode) or after
    ``duration_seconds`` elapsed (duration mode), whichever applies.

    Args:
        url: Target URL for requests
        iterations: Number of requests to make (fixed mode; capped as a safety
            limit only in duration mode when ``duration_seconds`` is given)
        concurrent: Maximum concurrent requests
        base_payload: Base JSON payload (unique value/req_id injected per request)
        body_sizes_kb: Optional list of body sizes in KB to mix per request
        client_id: Optional pinned client id (see limited_request)
        hot_clients: Number of hot client ids to emulate (see limited_request)
        hot_share: Fraction of requests assigned to the hot clients
        duration_seconds: When set (>0), keep sending requests until this many
            seconds elapsed, ignoring ``iterations`` (duration mode)

    Returns:
        Dictionary with test results
    """
    deadline = None
    if duration_seconds and duration_seconds > 0:
        deadline = time.monotonic() + duration_seconds
        max_requests: Optional[int] = None
        logger.info(f"Starting duration-based POST test to {url}: "
                    f"{duration_seconds:.0f}s at concurrency {concurrent}")
    else:
        max_requests = iterations
        logger.info(f"Starting {iterations} POST requests to {url} with concurrency {concurrent}")

    start_time = time.time()
    semaphore = asyncio.Semaphore(concurrent)

    connector = aiohttp.TCPConnector(limit=CONNECTOR_LIMIT, ssl=False)
    async with aiohttp.ClientSession(connector=connector) as session:
        total_sum = 0
        successful_requests = 0
        failed_requests = 0
        mismatched_requests = 0
        completed = 0
        latencies: List[float] = []
        total_sent = 0
        stop_launching = False
        tasks: set = set()
        last_progress_log = 0.0

        # Dynamic submission loop: keep up to `concurrent` requests in flight.
        # In fixed mode stop launching once `iterations` are sent; in duration
        # mode once the deadline passes (then drain the in-flight ones).
        # SIGTERM/SIGINT sets g_shutdown — stop launching and drain.
        while True:
            if g_shutdown:
                stop_launching = True
            if not stop_launching:
                if deadline is not None and time.monotonic() >= deadline:
                    stop_launching = True
                else:
                    while len(tasks) < concurrent and (max_requests is None or total_sent < max_requests) and not g_shutdown:
                        total_sent += 1
                        tasks.add(asyncio.create_task(
                            limited_request(session, semaphore, url, base_payload,
                                            total_sent, body_sizes_kb, client_id,
                                            hot_clients, hot_share)))
                        if deadline is not None and time.monotonic() >= deadline:
                            stop_launching = True
                            break

            if not tasks:
                break

            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for coro in done:
                tasks.discard(coro)
                req_id, result, latency_ms = await coro
                completed += 1
                latencies.append(latency_ms)

                # Process the result
                value_to_add, success_count, fail_count, mismatch_count = process_result(result, req_id)
                total_sum += value_to_add
                successful_requests += success_count
                failed_requests += fail_count
                mismatched_requests += mismatch_count

            # Log progress (fixed mode: every LOG_PROGRESS_INTERVAL; duration
            # mode: also at least once per ~10s so the run is observable).
            now = time.time()
            if (completed % LOG_PROGRESS_INTERVAL == 0
                    or (deadline is not None and now - last_progress_log >= 10.0)):
                last_progress_log = now
                elapsed_time = now - start_time
                requests_per_second = completed / elapsed_time if elapsed_time > 0 else 0
                if deadline is None:
                    logger.info(f"Completed {completed}/{iterations} requests. "
                              f"Current sum: {total_sum}, "
                              f"Requests per second: {requests_per_second:.2f}")
                else:
                    logger.info(f"Completed {completed} requests ({elapsed_time:.0f}s, "
                              f"{requests_per_second:.2f} rps). "
                              f"Current sum: {total_sum}")

        end_time = time.time()
        total_time = end_time - start_time
        requests_per_second = completed / total_time if total_time > 0 else 0

        latency_stats = compute_latency_stats(latencies)

        expected_sum = total_sent * (total_sent + 1) // 2

        return {
            'total_sum': total_sum,
            'expected_sum': expected_sum,
            'successful_requests': successful_requests,
            'failed_requests': failed_requests,
            'mismatched_requests': mismatched_requests,
            'total_time': total_time,
            'requests_per_second': requests_per_second,
            'iterations': total_sent,
            'latency_p50_ms': latency_stats["p50_ms"],
            'latency_p95_ms': latency_stats["p95_ms"],
            'latency_p99_ms': latency_stats["p99_ms"],
            'latency_avg_ms': latency_stats["avg_ms"],
            'latency_min_ms': latency_stats["min_ms"],
            'latency_max_ms': latency_stats["max_ms"],
        }


def print_results(results: dict) -> None:
    """
    Print test results in a formatted way.

    Args:
        results: Dictionary with test results
    """
    print(f"\n{Colors.GREEN}Results:{Colors.NC}")
    print(f"Expected sum: {results['expected_sum']}")
    print(f"Actual sum: {results['total_sum']}")
    print(f"Successful requests: {results['successful_requests']}")
    print(f"Failed requests: {results['failed_requests']}")
    print(f"Mismatched responses (crossed requests): {results['mismatched_requests']}")
    print(f"Total time: {results['total_time']:.2f} seconds")
    print(f"Requests per second: {results['requests_per_second']:.2f}")
    if results.get('latency_p50_ms') is not None:
        print(f"Latency p50: {results['latency_p50_ms']:.2f} ms")
        print(f"Latency p95: {results['latency_p95_ms']:.2f} ms")
        print(f"Latency p99: {results['latency_p99_ms']:.2f} ms")
        print(f"Latency avg: {results['latency_avg_ms']:.2f} ms")
        print(f"Latency min: {results['latency_min_ms']:.2f} ms")
        print(f"Latency max: {results['latency_max_ms']:.2f} ms")

    if (results['total_sum'] == results['expected_sum']
            and results['failed_requests'] == 0
            and results['mismatched_requests'] == 0):
        print(f"{Colors.GREEN}✅ Success: No message loss or crossed responses detected. "
              f"Every client received the response intended for it.{Colors.NC}")
        return True
    elif results['mismatched_requests'] > 0:
        print(f"{Colors.RED}❌ Error: {results['mismatched_requests']} responses were "
              f"delivered to the wrong client (value_return did not match the "
              f"sent value).{Colors.NC}")
        return False
    else:
        print(f"{Colors.RED}❌ Error: Sum mismatch or failures detected. "
              f"Expected {results['expected_sum']}, got {results['total_sum']}. "
              f"Failures: {results['failed_requests']}{Colors.NC}")
        print(f"{Colors.RED}Possible message loss or data corruption detected.{Colors.NC}")
        return False

def parse_args():
    """
    Parse command line arguments.

    Returns:
        Parsed arguments
    """
    parser = argparse.ArgumentParser(description="HTTP Proxy Message Consistency Test")
    parser.add_argument("--url", default=DEFAULT_URL,
                        help="Target URL for requests (default: http://localhost:7777)")
    parser.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS,
                        help="Number of requests to send (default: {})".format(DEFAULT_ITERATIONS))
    parser.add_argument("--duration", type=float, default=0.0,
                        help="Run the POST test continuously for this many "
                             "seconds instead of a fixed number of requests "
                             "(default: 0, disabled)")
    parser.add_argument("--concurrent", type=int, default=DEFAULT_CONCURRENT,
                        help="Maximum concurrent requests (default: {})".format(DEFAULT_CONCURRENT))
    parser.add_argument("--body-size", type=float, default=DEFAULT_BODY_SIZE_MB,
                        help="Maximum body size in megabytes (default: {} MB)".format(DEFAULT_BODY_SIZE_MB))
    parser.add_argument("--body-sizes", type=str, default=None,
                        help="Comma-separated body sizes in KB to mix per request "
                             "(e.g. 1,10,30) — production-like mix of small/large payloads")
    parser.add_argument("--log-level", default="INFO",
                        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
                        help="Logging level (default: INFO)")
    parser.add_argument("--test", default="all", choices=["post", "get", "all"],
                        help="Test type: post (JSON), get (binary favicon), or all (default: all)")
    parser.add_argument("--output-json", type=str, default=None,
                        help="Output results to JSON file")
    parser.add_argument("--client-id", type=str, default=None,
                        help="Pin the X-DataHub-Client-Id header and payload "
                             "client_id to this value for deterministic per-client "
                             "Grafana verification "
                             "(default: random per request)")
    parser.add_argument("--hot-clients", type=int, default=0,
                        help="Emulate hot clients: N fixed client ids "
                             "(hot-client-1..N) get the lion's share of requests, "
                             "so Grafana shows a few heavy consumers vs many small "
                             "ones (default: 0, disabled)")
    parser.add_argument("--hot-share", type=float, default=0.8,
                        help="Fraction of requests assigned to the hot clients "
                             "(default: 0.8)")
    parser.add_argument("--timeout", type=float, default=REQUEST_TIMEOUT,
                        help="HTTP request timeout in seconds (default: %(default)s)")
    parser.add_argument("--seed", type=int, default=None,
                        help="Random seed for reproducible bodies/client-ids (default: random)")
    return parser.parse_args()


# ============================================================================
# GET Request Test - Binary Data (favicon.ico)
# ============================================================================

async def make_get_request(session: aiohttp.ClientSession, url: str, req_id: int,
                           client_id: Optional[str] = None) -> Tuple[int, bool, Optional[str]]:
    """
    Make a single GET request for favicon.ico and verify binary data integrity.

    Sends the X-DataHub-Client-Id header so the favicon probe is attributed to
    a client in Grafana instead of polluting the "unknown" bucket. When no
    pinned client id is given, a random per-request id is used so the favicon
    probes don't form one dominant series in the hot-client panel.

    Args:
        session: aiohttp client session
        url: Target URL for the request
        req_id: Request identifier
        client_id: Optional pinned client id for the header (default: random)

    Returns:
        Tuple of (request_id, success, error_message)
    """
    headers = {'X-DataHub-Client-Id': client_id or f"client-{random.randint(1, 10000)}"}
    try:
        async with session.get(url, headers=headers, timeout=aiohttp.ClientTimeout(total=REQUEST_TIMEOUT), ssl=should_use_ssl(url)) as response:
            if response.status == 200:
                # Check content type
                content_type = response.headers.get('Content-Type', '')
                if 'image/x-icon' not in content_type and 'image/vnd.microsoft.icon' not in content_type:
                    logger.warning(f"Request {req_id}: Unexpected content type: {content_type}")
                    return req_id, False, f"Unexpected content type: {content_type}"

                # Read binary data
                binary_data = await response.read()

                # Verify ICO header (must start with 00 00 01 00)
                if len(binary_data) < 4 or binary_data[:4] != b'\x00\x00\x01\x00':
                    logger.warning(f"Request {req_id}: Invalid ICO header")
                    return req_id, False, "Invalid ICO header"

                # Calculate hash for integrity check
                data_hash = hashlib.sha256(binary_data).hexdigest()[:16]
                logger.debug(f"Request {req_id}: Received {len(binary_data)} bytes, hash={data_hash}")

                return req_id, True, None
            else:
                error_text = await response.text()
                logger.warning(f"Request {req_id}: Received status code {response.status}: {error_text[:200]}...")
                return req_id, False, f"Status {response.status}"
    except asyncio.TimeoutError:
        logger.error(f"Request {req_id}: Timed out")
        return req_id, False, "Timeout"
    except aiohttp.ClientError as e:
        logger.error(f"Request {req_id}: Client error: {e}")
        return req_id, False, str(e)
    except Exception as e:
        logger.error(f"Request {req_id}: Unexpected error: {e}")
        return req_id, False, str(e)


async def limited_get_request(session: aiohttp.ClientSession, semaphore: asyncio.Semaphore,
                              url: str, req_id: int,
                              client_id: Optional[str] = None) -> Tuple[int, bool, Optional[str]]:
    """
    Make a GET request with concurrency limiting.

    Args:
        session: aiohttp client session
        semaphore: Concurrency limiter
        url: Target URL
        req_id: Request identifier
        client_id: Optional pinned client id for the X-DataHub-Client-Id header

    Returns:
        Tuple of (request_id, success, error_message)
    """
    async with semaphore:
        return await make_get_request(session, url, req_id, client_id)


async def run_get_test(url: str, iterations: int, concurrent: int,
                       client_id: Optional[str] = None) -> dict:
    """
    Run the GET request binary data test.

    Args:
        url: Target URL for requests
        iterations: Number of requests to make
        concurrent: Maximum concurrent requests
        client_id: Optional pinned client id for the X-DataHub-Client-Id header

    Returns:
        Dictionary with test results
    """
    logger.info(f"Starting {iterations} GET requests to {url} (favicon.ico) with concurrency {concurrent}")

    start_time = time.time()
    semaphore = asyncio.Semaphore(concurrent)

    connector = aiohttp.TCPConnector(limit=CONNECTOR_LIMIT, ssl=False)
    async with aiohttp.ClientSession(connector=connector) as session:
        tasks = [
            asyncio.create_task(limited_get_request(session, semaphore, url, i+1, client_id))
            for i in range(iterations)
        ]

        successful_requests = 0
        failed_requests = 0
        errors: List[str] = []
        completed = 0

        # Process results as they complete
        for coro in asyncio.as_completed(tasks):
            req_id, success, error = await coro
            completed += 1

            if success:
                successful_requests += 1
            else:
                failed_requests += 1
                if error:
                    errors.append(f"Request {req_id}: {error}")

            # Log progress
            if completed % LOG_PROGRESS_INTERVAL == 0 or completed == iterations:
                elapsed_time = time.time() - start_time
                requests_per_second = completed / elapsed_time if elapsed_time > 0 else 0
                logger.info(f"Completed {completed}/{iterations} requests. "
                          f"Success: {successful_requests}, Failed: {failed_requests}, "
                          f"Requests per second: {requests_per_second:.2f}")

        end_time = time.time()
        total_time = end_time - start_time
        requests_per_second = iterations / total_time if total_time > 0 else 0

        return {
            'successful_requests': successful_requests,
            'failed_requests': failed_requests,
            'errors': errors,
            'total_time': total_time,
            'requests_per_second': requests_per_second,
            'iterations': iterations
        }


def print_get_results(results: dict) -> bool:
    """
    Print GET test results in a formatted way.

    Args:
        results: Dictionary with test results

    Returns:
        True if all tests passed, False otherwise
    """
    print(f"\n{Colors.GREEN}GET Test Results:{Colors.NC}")
    print(f"Successful requests: {results['successful_requests']}")
    print(f"Failed requests: {results['failed_requests']}")
    print(f"Total time: {results['total_time']:.2f} seconds")
    print(f"Requests per second: {results['requests_per_second']:.2f}")

    if results['failed_requests'] > 0:
        print(f"\n{Colors.YELLOW}Errors:{Colors.NC}")
        for error in results['errors'][:10]:  # Show first 10 errors
            print(f"  - {error}")
        if len(results['errors']) > 10:
            print(f"  ... and {len(results['errors']) - 10} more errors")

    if results['failed_requests'] == 0:
        print(f"{Colors.GREEN}✅ Success: All GET requests returned valid binary favicon data.{Colors.NC}")
        return True
    else:
        print(f"{Colors.RED}❌ Error: {results['failed_requests']} GET requests failed.{Colors.NC}")
        return False


async def main():
    """Main function to run the message consistency test."""
    args = parse_args()

    # Reproducibility
    if args.seed is not None:
        random.seed(args.seed)
        logger.info(f"Random seed: {args.seed}")

    # Override global timeout before any request
    global REQUEST_TIMEOUT
    REQUEST_TIMEOUT = args.timeout

    _install_signal_handlers()

    # Set logging level
    logging.getLogger().setLevel(getattr(logging, args.log_level))

    args.url = await resolve_runtime_url(args.url)

    all_success = True

    # Run POST test (JSON response-to-request correlation)
    if args.test in ["post", "all"]:
        body_size = args.body_size

        # Optional per-request body size mix (KB). When set, a fresh body of a
        # random size from the list is generated per request.
        body_sizes_kb = None
        if args.body_sizes:
            body_sizes_kb = [int(x) for x in args.body_sizes.split(",") if x.strip()]
            if not body_sizes_kb:
                logger.error(f"Invalid --body-sizes value: {args.body_sizes}")
                return 1
            logger.info(f"Body size mix per request (KB): {body_sizes_kb}")

        # Generate a simple, flat JSON echo body (default ~10 KB, matching
        # the 10-30 KB response size seen in production). The unique
        # value/req_id is injected per request in run_test.
        try:
            payload = json.loads(generate_random_body(body_size))
        except json.JSONDecodeError:
            payload = {"value": 0, "request_type": "echo", "version": "1.0.0"}

        logger.info("Using simplified flat JSON echo body")

        # Hot-client emulation: a few fixed client ids absorb most of the load,
        # so the Grafana "hot clients" panel shows them as red bars while the
        # long tail of normal clients stays green.
        if args.hot_clients > 0:
            if not (0.0 < args.hot_share <= 1.0):
                logger.error(f"Invalid --hot-share value: {args.hot_share} (must be in (0, 1])")
                return 1
            logger.info(f"Hot client emulation: {args.hot_clients} hot client(s), "
                        f"share {args.hot_share:.0%} of requests")

        # Check JSON size to avoid issues
        payload_json = json.dumps(payload)
        payload_size_mb = len(payload_json.encode('utf-8')) / 1024 / 1024

        if payload_size_mb > MAX_JSON_SIZE_MB:  # Keep JSON under threshold
            logger.warning(f"JSON payload size ({payload_size_mb:.2f} MB) is large and may cause issues")
            if payload_size_mb > CRITICAL_JSON_SIZE_MB:
                logger.error(f"JSON payload too large ({payload_size_mb:.2f} MB). Reducing body size.")
                new_body_size = min(args.body_size, body_size)
                payload = json.loads(generate_random_body(new_body_size))
                payload_json = json.dumps(payload)
                payload_size_mb = len(payload_json.encode('utf-8')) / 1024 / 1024
                logger.info(f"Reduced body size to {new_body_size} MB. New JSON size: {payload_size_mb:.2f} MB")

        logger.info(f"Starting POST response-to-request correlation test")
        logger.info(f"URL: {args.url}")
        if args.duration > 0:
            logger.info(f"Duration: {args.duration:.0f} seconds (continuous load)")
        else:
            logger.info(f"Iterations: {args.iterations}")
        logger.info(f"Concurrent: {args.concurrent}")
        logger.info(f"Body size: {body_size} MB")
        logger.info(f"Message size configuration: {body_size} MB per request")
        logger.info(f"Total JSON payload size: {payload_size_mb:.2f} MB")
        logger.info(f"Payload keys: {list(payload.keys())}")

        # Run the POST test
        results = await run_test(args.url, args.iterations, args.concurrent,
                                 payload, body_sizes_kb, args.client_id,
                                 args.hot_clients, args.hot_share,
                                 args.duration if args.duration > 0 else None)

        # Print results
        post_success = print_results(results)
        all_success = all_success and post_success

        # Save results to JSON if requested
        if args.output_json:
            output_data = {
                "test_type": "post",
                "url": args.url,
                "iterations": args.iterations,
                "duration_seconds": args.duration if args.duration > 0 else 0,
                "concurrent": args.concurrent,
                "body_size_mb": args.body_size,
                "total_requests": results["iterations"],
                "successful_requests": results["successful_requests"],
                "failed_requests": results["failed_requests"],
                "mismatched_requests": results["mismatched_requests"],
                "total_time_seconds": results["total_time"],
                "requests_per_second": results["iterations"] / results["total_time"] if results["total_time"] > 0 else 0,
                "latency_p50_ms": results.get("latency_p50_ms", 0),
                "latency_p95_ms": results.get("latency_p95_ms", 0),
                "latency_p99_ms": results.get("latency_p99_ms", 0),
                "latency_avg_ms": results.get("latency_avg_ms", 0),
                "latency_min_ms": results.get("latency_min_ms", 0),
                "latency_max_ms": results.get("latency_max_ms", 0)
            }
            with open(args.output_json, 'w') as f:
                json.dump(output_data, f, indent=2)
            logger.info(f"Results saved to {args.output_json}")

    # Run GET test (binary favicon.ico)
    if args.test in ["get", "all"]:
        get_url = args.url.rstrip('/') + FAVICON_PATH

        logger.info(f"Starting GET binary data test")
        logger.info(f"URL: {get_url}")
        logger.info(f"Iterations: {args.iterations}")
        logger.info(f"Concurrent: {args.concurrent}")

        # Run the GET test
        get_results = await run_get_test(get_url, args.iterations, args.concurrent, args.client_id)

        # Print results
        get_success = print_get_results(get_results)
        all_success = all_success and get_success

    return 0 if all_success else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
