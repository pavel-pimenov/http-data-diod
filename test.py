#!/usr/bin/env python3
"""
Test script for HTTP Redis Proxy system
Includes functionality verification and load testing
"""

import subprocess
import time
import json
import random
import string
import difflib
from typing import List, Tuple, Dict
import statistics
import sys
import os
import argparse
import logging

# Check dependencies
def check_dependencies():
    """Check if required packages are installed."""
    required_packages = ['requests', 'aiohttp']
    missing_packages = []

    for package in required_packages:
        try:
            __import__(package.replace('-', '_'))
        except ImportError:
            missing_packages.append(package)

    if missing_packages:
        print(f"Missing dependencies: {', '.join(missing_packages)}")
        print("Please install them using: pip install -r requirements.txt")
        sys.exit(1)

# Install dependencies before other imports
check_dependencies()

import requests
import asyncio
import aiohttp

# Configuration
PROXY_URL = "http://localhost:8888"
NUM_REQUESTS = 1
CONCURRENT_REQUESTS = 1
MIN_PAYLOAD_SIZE = 100  # bytes
MAX_PAYLOAD_SIZE = 100

# Set up logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.StreamHandler(sys.stdout),
        logging.FileHandler('test_debug.log', mode='w')
    ]
)
logger = logging.getLogger(__name__)


class Colors:
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    NC = '\033[0m'  # No Color

def generate_random_json_payload(size_bytes: int) -> dict:
    """Generate a random JSON payload of approximately the given size in bytes."""
    if size_bytes < 50:  # minimum for a basic JSON
        return {"data": "x" * (size_bytes - 10)}

    # Start with a basic structure
    payload = {"data": ""}

    # Keep adding random data until we reach the desired size
    while len(json.dumps(payload).encode('utf-8')) < size_bytes:
        # Add random key-value pairs or extend existing data
        key = ''.join(random.choices(string.ascii_letters + string.digits, k=10))
        value = ''.join(random.choices(string.ascii_letters + string.digits + ' ', k=random.randint(10, 100)))
        payload[key] = value

        # If still small, extend the data field
        if len(json.dumps(payload).encode('utf-8')) < size_bytes * 0.8:
            payload["data"] += ''.join(random.choices(string.ascii_letters + string.digits + ' ', k=1000))

    # Trim if overshot
    current_size = len(json.dumps(payload).encode('utf-8'))
    if current_size > size_bytes:
        excess = current_size - size_bytes
        if "data" in payload and isinstance(payload["data"], str):
            payload["data"] = payload["data"][:-excess] if len(payload["data"]) > excess else ""

    return payload

def run_command(cmd: List[str]) -> subprocess.CompletedProcess:
    """Run a shell command and return the result."""
    return subprocess.run(cmd, capture_output=True, text=True)

def test_json_endpoint(url: str, description: str = "") -> bool:
    """Test an endpoint that should return valid JSON matching the request."""
    logger.info(f"Starting functionality test for {description} ({url})")
    print(f"Testing {description} ({url})... ", end="", flush=True)
    json_payload = generate_random_json_payload(500)  # Fixed size for functionality test
    try:
        response = requests.post(url, json=json_payload, timeout=10)
        if response.status_code == 200:
            try:
                response_json = response.json()
                # Normalize both JSON for comparison
                request_normalized = json.dumps(json_payload, sort_keys=True, indent=2)
                response_normalized = json.dumps(response_json, sort_keys=True, indent=2)

                if request_normalized == response_normalized:
                    print(f"{Colors.GREEN}PASS{Colors.NC} (Status: {response.status_code}, JSON matches)")
                    logger.info(f"Functionality test passed for {description}")
                    return True
                else:
                    print(f"{Colors.RED}FAIL{Colors.NC} (Status: {response.status_code}, JSON does not match)")
                    print(f"\n{Colors.YELLOW}=== JSON Diff (Request vs Response) ==={Colors.NC}")
                    diff = list(difflib.unified_diff(
                        request_normalized.splitlines(keepends=True),
                        response_normalized.splitlines(keepends=True),
                        fromfile='Request',
                        tofile='Response',
                        lineterm=''
                    ))
                    for line in diff:
                        if line.startswith('---') or line.startswith('+++') or line.startswith('@@'):
                            print(f"{Colors.YELLOW}{line.rstrip()}{Colors.NC}", end='')
                        elif line.startswith('-'):
                            print(f"{Colors.RED}{line.rstrip()}{Colors.NC}")
                        elif line.startswith('+'):
                            print(f"{Colors.GREEN}{line.rstrip()}{Colors.NC}")
                        else:
                            print(line.rstrip())
                    print(f"{Colors.YELLOW}=== End Diff ==={Colors.NC}")
                    logger.error(f"Functionality test failed for {description}: JSON mismatch")
                    return False
            except json.JSONDecodeError:
                print(f"{Colors.RED}FAIL{Colors.NC} (Status: {response.status_code}, Invalid JSON)")
                print(f"Response: {response.text}")
                logger.error(f"Functionality test failed for {description}: Invalid JSON")
                return False
        else:
            print(f"{Colors.RED}FAIL{Colors.NC} (Status: {response.status_code})")
            logger.error(f"Functionality test failed for {description}: Status {response.status_code}")
            return False
    except requests.RequestException as e:
        print(f"{Colors.RED}FAIL{Colors.NC} (Connection failed: {e})")
        logger.error(f"Functionality test failed for {description}: Connection error {e}")
        return False

async def make_async_request(session: aiohttp.ClientSession, request_id: int, json_payload: dict, size: int) -> Tuple[int, float, bool]:
    """Make a single async POST request with pre-generated JSON payload and return timing info."""
    start_time = time.time()
    logger.info(f"Starting async request {request_id} with payload size {size} bytes")
    try:
        async with session.post(PROXY_URL, json=json_payload) as response:
            end_time = time.time()
            status = response.status
            logger.info(f"Completed async request {request_id} in {end_time - start_time:.2f}s with status {status}")
            return request_id, end_time - start_time, status == 200
    except Exception as e:
        end_time = time.time()
        logger.error(f"Exception in async request {request_id}: {e} after {end_time - start_time:.2f}s")
        return request_id, end_time - start_time, False

async def run_load_test() -> Dict:
    """Run load testing with async requests."""
    logger.info("Starting load test")
    print(f"Running load test with {NUM_REQUESTS} requests ({CONCURRENT_REQUESTS} concurrent)...")

    # Pre-generate all payloads synchronously to avoid blocking the event loop
    logger.info("Generating payloads...")
    payloads = []
    for i in range(NUM_REQUESTS):
        size = random.randint(MIN_PAYLOAD_SIZE, MAX_PAYLOAD_SIZE)
        json_payload = generate_random_json_payload(size)
        payloads.append((i + 1, size, json_payload))
    logger.info(f"Generated {len(payloads)} payloads")

    async with aiohttp.ClientSession() as session:
        logger.info("Created aiohttp session")
        tasks = []
        for request_id, size, json_payload in payloads:
            tasks.append(make_async_request(session, request_id, json_payload, size))
        logger.info(f"Created {len(tasks)} tasks")

        logger.info("Starting asyncio.gather")
        results = await asyncio.gather(*tasks, return_exceptions=True)
        logger.info(f"Completed asyncio.gather with {len(results)} results")

    # Process results
    successful_requests = 0
    failed_requests = 0
    response_times = []

    for result in results:
        if isinstance(result, Exception):
            logger.error(f"Task returned exception: {result}")
            failed_requests += 1
        else:
            request_id, response_time, success = result
            if success:
                successful_requests += 1
                response_times.append(response_time * 1000)  # Convert to milliseconds
            else:
                failed_requests += 1

    stats = {
        'total_requests': len(results),
        'successful_requests': successful_requests,
        'failed_requests': failed_requests,
        'success_rate': (successful_requests / len(results)) * 100 if results else 0,
        'response_times': response_times
    }

    if response_times:
        stats.update({
            'avg_time': statistics.mean(response_times),
            'min_time': min(response_times),
            'max_time': max(response_times),
            'median_time': statistics.median(response_times)
        })

    logger.info(f"Load test completed: {successful_requests} successful, {failed_requests} failed")
    return stats

def print_load_results(stats: Dict):
    """Print load test results."""
    print(f"\n{Colors.GREEN}=== Load Test Results ==={Colors.NC}")
    print(f"Total Requests: {stats['total_requests']}")
    print(f"Successful Requests: {stats['successful_requests']}")
    print(f"Failed Requests: {stats['failed_requests']}")
    print(f"Success Rate: {stats['success_rate']:.1f}%")

    if 'avg_time' in stats:
        print(f"Average Response Time: {stats['avg_time']:.2f}ms")
        print(f"Min Response Time: {stats['min_time']:.2f}ms")
        print(f"Max Response Time: {stats['max_time']:.2f}ms")
        print(f"Median Response Time: {stats['median_time']:.2f}ms")

def main():
    global MIN_PAYLOAD_SIZE, MAX_PAYLOAD_SIZE

    logger.info("Starting test script")

    parser = argparse.ArgumentParser(description="Test script for HTTP Redis Proxy system")
    parser.add_argument('--min-size', type=int, default=200, help='Minimum payload size in bytes (default: 200)')
    parser.add_argument('--max-size', type=int, default=1000000, help='Maximum payload size in bytes (default: 1000000)')

    args = parser.parse_args()
    MIN_PAYLOAD_SIZE = args.min_size
    MAX_PAYLOAD_SIZE = args.max_size

    logger.info(f"Using payload sizes from {MIN_PAYLOAD_SIZE} to {MAX_PAYLOAD_SIZE} bytes")
    print(f"Using payload sizes from {MIN_PAYLOAD_SIZE} to {MAX_PAYLOAD_SIZE} bytes")

    print(f"\n{Colors.GREEN}=== Functionality Tests ==={Colors.NC}")

    all_passed = True
    all_passed &= test_json_endpoint(f"{PROXY_URL}/", "Proxy Main Endpoint")

    # Load testing
    print(f"\n{Colors.GREEN}=== Load Testing ==={Colors.NC}")
    logger.info("Starting load test phase")
    stats = asyncio.run(run_load_test())
    print_load_results(stats)

    # Determine overall success
    load_passed = stats['success_rate'] >= 95.0 and stats['failed_requests'] == 0

    if all_passed and load_passed:
        logger.info("All tests passed")
        print(f"\n{Colors.GREEN}✅ All tests passed! System is working correctly.{Colors.NC}")
        return 0
    else:
        logger.warning("Some tests failed")
        print(f"\n{Colors.RED}❌ Some tests failed. Check the output above.{Colors.NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
