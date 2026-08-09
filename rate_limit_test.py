#!/usr/bin/env python3
"""
Integration test for the global HTTP rate limiter.

Sends a burst of POST requests to the l2-proxy and verifies that:
  - when ENABLE_GLOBAL_RATE_LIMITING=true (default), the limiter eventually
    starts rejecting with HTTP 429 and those responses carry Retry-After and
    X-RateLimit-* headers;
  - when ENABLE_GLOBAL_RATE_LIMITING=false, no 429 responses appear.

The expected behaviour is inferred from the ENABLE_GLOBAL_RATE_LIMITING env
var (the same one passed to docker-compose), so the test adapts to the
currently deployed configuration. Override with --expect-429 / --expect-zero.

For a fast, deterministic trip test deploy with a small limiter override:

  docker compose -f docker-compose.yml -f docker-compose.ratelimit.yml up -d
  python3 rate_limit_test.py --expect-429

Usage:
  python3 rate_limit_test.py                 # auto-detect from env
  ENABLE_GLOBAL_RATE_LIMITING=false python3 rate_limit_test.py
"""

import asyncio
import aiohttp
import time
import argparse
import logging
import sys
import os
from typing import Optional

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[logging.StreamHandler(sys.stdout)]
)
logger = logging.getLogger(__name__)

DEFAULT_URL = 'http://localhost:8888'
REQUEST_TIMEOUT = 10
SSL_VERIFY = False

# Trip mode: stop once this many 429 responses are observed
REJECT_TARGET = 50
# Batch size per round in trip mode
BATCH_SIZE = 1000
# No-reject mode: number of requests to send
VERIFY_ITERATIONS = 3000
# No-reject mode: concurrency
VERIFY_CONCURRENT = 100


class Colors:
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    NC = '\033[0m'


async def send_batch(session: aiohttp.ClientSession, url: str, count: int,
                     concurrent: int) -> dict:
    """Send `count` small POST requests with limited concurrency."""
    semaphore = asyncio.Semaphore(concurrent)
    payload = {"value": 1, "body": "rate-limit-test"}
    counters = {"accepted": 0, "rejected": 0, "errors": 0}
    first_429_headers: Optional[dict] = None

    async def one_request():
        nonlocal first_429_headers
        async with semaphore:
            try:
                async with session.post(
                        url, json=payload,
                        timeout=aiohttp.ClientTimeout(total=REQUEST_TIMEOUT),
                        ssl=not SSL_VERIFY) as response:
                    if response.status == 200:
                        counters["accepted"] += 1
                    elif response.status == 429:
                        counters["rejected"] += 1
                        if first_429_headers is None:
                            first_429_headers = dict(response.headers)
                    else:
                        logger.warning(f"Unexpected status: {response.status}")
                        counters["errors"] += 1
            except (asyncio.TimeoutError, aiohttp.ClientError) as e:
                logger.warning(f"Request error: {e}")
                counters["errors"] += 1

    tasks = [asyncio.create_task(one_request()) for _ in range(count)]
    await asyncio.gather(*tasks)
    return {**counters, "first_429_headers": first_429_headers}


async def run_trip_mode(url: str, concurrent: int,
                        send_target: int) -> bool:
    """Send requests until 429s appear (or target reached) and verify headers."""
    logger.info(
        f"Trip mode: sending up to {send_target} requests to {url} until 429 "
        f"appears (reject target={REJECT_TARGET}, concurrent={concurrent})")
    start = time.time()
    sent = 0
    accepted = 0
    rejected = 0
    errors = 0
    first_429_headers: Optional[dict] = None

    connector = aiohttp.TCPConnector(limit=concurrent + 16, ssl=not SSL_VERIFY)
    async with aiohttp.ClientSession(connector=connector) as session:
        while sent < send_target and rejected < REJECT_TARGET:
            batch = await send_batch(
                session, url, min(BATCH_SIZE, send_target - sent), concurrent)
            sent += batch["accepted"] + batch["rejected"] + batch["errors"]
            accepted += batch["accepted"]
            rejected += batch["rejected"]
            errors += batch["errors"]
            if first_429_headers is None:
                first_429_headers = batch["first_429_headers"]
            elapsed = time.time() - start
            logger.info(
                f"Sent={sent} accepted={accepted} rejected={rejected} "
                f"errors={errors} rate={sent / elapsed:.0f} req/s")

    elapsed = time.time() - start
    rate = sent / elapsed if elapsed > 0 else 0.0
    print(f"\n{Colors.GREEN}Trip results:{Colors.NC}")
    print(f"Sent: {sent}")
    print(f"Accepted (200): {accepted}")
    print(f"Rejected (429): {rejected}")
    print(f"Errors: {errors}")
    print(f"Total time: {elapsed:.2f} seconds")
    print(f"Throughput: {rate:.0f} req/s")

    if rejected == 0:
        refill = int(os.environ.get("GLOBAL_RATE_LIMIT_REFILL_RATE", "1000"))
        print(f"{Colors.RED}❌ No 429 received: the proxy only sustained "
              f"{rate:.0f} req/s, below the configured refill of {refill} req/s. "
              f"Deploy with the small-limiter override for a deterministic "
              f"test: docker compose -f docker-compose.yml -f "
              f"docker-compose.ratelimit.yml up -d{Colors.NC}")
        return False
    if errors > 0:
        print(f"{Colors.RED}❌ {errors} request errors under load.{Colors.NC}")
        return False

    # Verify rate-limit headers on a 429 response
    headers_ok = True
    for expected in ("Retry-After", "X-RateLimit-Limit", "X-RateLimit-Remaining"):
        if not first_429_headers or expected not in first_429_headers:
            headers_ok = False
            print(f"{Colors.RED}❌ 429 response missing header "
                  f"{expected}.{Colors.NC}")
    if headers_ok:
        print(f"{Colors.GREEN}✅ 429 response carries Retry-After and "
              f"X-RateLimit-* headers:{Colors.NC}")
        print(f"   Retry-After={first_429_headers['Retry-After']}, "
              f"X-RateLimit-Limit={first_429_headers['X-RateLimit-Limit']}, "
              f"X-RateLimit-Remaining={first_429_headers['X-RateLimit-Remaining']}")
        print(f"{Colors.GREEN}✅ Global rate limiter rejected requests as "
              f"expected.{Colors.NC}")
        return True
    return False


async def run_no_reject_mode(url: str) -> bool:
    """Send a fixed number of requests and assert no 429 appears."""
    logger.info(
        f"No-reject mode: sending {VERIFY_ITERATIONS} requests to {url}, "
        f"expecting zero 429 responses")
    connector = aiohttp.TCPConnector(limit=VERIFY_CONCURRENT + 16,
                                     ssl=not SSL_VERIFY)
    async with aiohttp.ClientSession(connector=connector) as session:
        result = await send_batch(session, url, VERIFY_ITERATIONS,
                                  VERIFY_CONCURRENT)

    print(f"\n{Colors.GREEN}No-reject results:{Colors.NC}")
    print(f"Accepted (200): {result['accepted']}")
    print(f"Rejected (429): {result['rejected']}")
    print(f"Errors: {result['errors']}")

    if result["rejected"] != 0:
        print(f"{Colors.RED}❌ Unexpected 429 responses while the global "
              f"rate limiter is disabled.{Colors.NC}")
        return False
    if result["errors"] > 0:
        print(f"{Colors.RED}❌ {result['errors']} request errors.{Colors.NC}")
        return False
    print(f"{Colors.GREEN}✅ No rate limiting observed, all requests "
          f"accepted.{Colors.NC}")
    return True


def global_limiter_enabled_from_env() -> bool:
    return os.environ.get("ENABLE_GLOBAL_RATE_LIMITING", "true").lower() not in (
        "false", "0", "no")


def parse_args():
    parser = argparse.ArgumentParser(
        description="HTTP Proxy Rate Limiter Integration Test")
    parser.add_argument("--url", default=DEFAULT_URL,
                        help=f"Target proxy URL (default: {DEFAULT_URL})")
    parser.add_argument("--concurrent", type=int, default=100,
                        help="Concurrency for the trip mode burst "
                             "(default: 100)")
    parser.add_argument(
        "--iterations", type=int, default=0,
        help="Max requests for trip mode (default: auto = 3x "
             "GLOBAL_RATE_LIMIT_MAX_TOKENS, min 200)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--expect-429", action="store_true",
                      help="Assert that the global limiter rejects under load "
                           "(default: inferred from ENABLE_GLOBAL_RATE_LIMITING)")
    mode.add_argument("--expect-zero", action="store_true",
                      help="Assert that no 429 appears even under load")
    return parser.parse_args()


async def main() -> int:
    args = parse_args()
    if args.expect_429:
        expect_enabled = True
    elif args.expect_zero:
        expect_enabled = False
    else:
        expect_enabled = global_limiter_enabled_from_env()
    logger.info(f"ENABLE_GLOBAL_RATE_LIMITING (inferred) = {expect_enabled}")

    if expect_enabled:
        burst = int(os.environ.get("GLOBAL_RATE_LIMIT_MAX_TOKENS", "10000"))
        send_target = args.iterations if args.iterations > 0 else max(
            burst * 3, 200)
        success = await run_trip_mode(args.url, args.concurrent, send_target)
    else:
        success = await run_no_reject_mode(args.url)
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
