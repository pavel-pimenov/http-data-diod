#!/usr/bin/env python3
"""
Test crash handler: triggers SIGSEGV via /crash-test, waits for restart,
and verifies the crash dump contains a readable stack trace (raw addresses).

Prerequisites: the proxy must run with ENABLE_CRASH_TEST_ENDPOINT=true
(docker compose up -d with the env var set, e.g.:
  ENABLE_CRASH_TEST_ENDPOINT=true ./rebuild-and-run.sh)
"""
import re
import subprocess
import time
import sys
import os

CONTAINER = "l2-proxy"
CRASH_DUMPS_DIR = "./crash-dumps"
PROXY_URL = "http://localhost:7777"
TIMEOUT = 30


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, timeout=TIMEOUT, **kwargs)


def get_existing_dumps() -> set[str]:
    if not os.path.isdir(CRASH_DUMPS_DIR):
        return set()
    return {f for f in os.listdir(CRASH_DUMPS_DIR) if f.startswith("crash_")}


def main() -> int:
    print("=" * 60)
    print("Crash Handler Test")
    print("=" * 60)

    # Step 1: Snapshot existing crash dumps
    before = get_existing_dumps()
    print(f"\n[1] Existing crash dumps: {len(before)}")
    for f in sorted(before):
        print(f"    - {f}")

    # Step 2: Verify proxy is healthy before crash
    print("\n[2] Checking proxy health before crash...")
    r = run(["curl", "-sf", f"{PROXY_URL}/health/live"])
    if r.returncode != 0:
        print(f"    FAIL: proxy not healthy: {r.stderr.strip()}")
        return 1
    print("    OK: proxy is alive")

    # Step 3: Trigger crash
    print("\n[3] Triggering /crash-test endpoint...")
    # This will fail (connection reset) because the proxy crashes
    r = run(["curl", "-s", "--max-time", "5", f"{PROXY_URL}/crash-test"])
    if "disabled" in r.stdout:
        print("    FAIL: /crash-test is disabled. Set ENABLE_CRASH_TEST_ENDPOINT=true")
        print(f"    Response: {r.stdout.strip()}")
        return 1
    print(f"    curl exit code: {r.returncode} (expected non-zero — proxy crashed)")

    # Step 4: Wait for container restart
    print(f"\n[4] Waiting for container {CONTAINER} to restart...")
    start = time.time()
    restarted = False
    while time.time() - start < 60:
        r = run(["docker", "compose", "ps", "--format", "{{.State}}", CONTAINER])
        state = r.stdout.strip()
        if "running" in state.lower() or "healthy" in state.lower():
            restarted = True
            break
        time.sleep(2)
    if not restarted:
        print(f"    FAIL: container did not restart within 60s")
        return 1
    print(f"    OK: container restarted in {time.time() - start:.1f}s")

    # Wait for proxy to become healthy
    print("    Waiting for proxy health check...")
    start = time.time()
    healthy = False
    while time.time() - start < 30:
        r = run(["curl", "-sf", f"{PROXY_URL}/health/live"])
        if r.returncode == 0:
            healthy = True
            break
        time.sleep(2)
    if not healthy:
        print("    WARN: proxy not healthy yet after 30s")
    else:
        print(f"    OK: proxy healthy in {time.time() - start:.1f}s")

    # Step 5: Check for new crash dump
    print("\n[5] Checking for new crash dump...")
    time.sleep(1)  # small delay for file system sync
    after = get_existing_dumps()
    new_dumps = after - before
    if not new_dumps:
        print("    FAIL: no new crash dump found!")
        print(f"    Before: {sorted(before)}")
        print(f"    After:  {sorted(after)}")
        return 1
    dump_file = sorted(new_dumps)[0]
    dump_path = os.path.join(CRASH_DUMPS_DIR, dump_file)
    print(f"    OK: new crash dump: {dump_file}")

    # Step 6: Read and validate crash dump
    print(f"\n[6] Validating crash dump content: {dump_path}")
    with open(dump_path, "r") as f:
        content = f.read()

    checks = {
        "CRASH REPORT header": "CRASH REPORT" in content,
        "Signal name (SIGSEGV)": "SIGSEGV" in content,
        "PID present": "PID:" in content,
        "Timestamp present": "Timestamp:" in content,
        "STACK TRACE section": "STACK TRACE" in content,
        "Raw-address frames": re.search(r"^#[0-9]+\s+0x[0-9a-fA-F]+", content, re.M)
        is not None,
        "Non-empty stack trace": bool(
            re.findall(r"^#[0-9]+\s+0x[0-9a-fA-F]+", content, re.M)
        ),
    }

    all_ok = True
    for name, result in checks.items():
        status = "PASS" if result else "FAIL"
        if not result:
            all_ok = False
        print(f"    [{status}] {name}")

    # Print first 40 lines of crash dump
    print(f"\n{'=' * 60}")
    print("Crash dump content (first 40 lines):")
    print("=" * 60)
    lines = content.split("\n")
    for line in lines[:40]:
        print(f"  {line}")
    if len(lines) > 40:
        print(f"  ... ({len(lines) - 40} more lines)")

    print(f"\n{'=' * 60}")
    if all_ok:
        print("✅ All crash handler checks PASSED")
    else:
        print("❌ Some checks FAILED")
    print("=" * 60)
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
