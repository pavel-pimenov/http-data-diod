#!/usr/bin/env python3
"""Shared async helpers for the E2E test scripts.

The e2e tests (worker-reconnect, graceful-shutdown, db-gateway, sentry) used
to mix sync (urllib/subprocess) and async (aiohttp/nats-py) styles with
copy-pasted helpers. This module centralises the common pieces on the async
style so a change lands in one place and the scripts stay consistent.

Exposed:
  - log            timestamped console logger
  - docker         async wrapper around the docker CLI (captures output)
  - nats_url       authorized NATS URL, honors E2E_NATS_URL / NATS_TOKEN/.env
  - http_get/http_post  async HTTP helpers (aiohttp), JSON in/out
  - wait_for       poll an async predicate until it returns truthy or times out
  - wait_http      poll an async request until it returns a wanted status
"""

import asyncio
import json
import os
import time
from pathlib import Path
from typing import List

import aiohttp

REPO_ROOT = Path(__file__).resolve().parent.parent


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


async def docker(args: List[str], timeout: float = 40.0):
    """Run a docker command, capturing stdout/stderr. Returns the CompletedProcess
    (never raises on a non-zero exit — callers inspect .returncode)."""
    import subprocess

    return await asyncio.wait_for(
        asyncio.get_running_loop().run_in_executor(
            None,
            lambda: subprocess.run(
                ["docker", *args], capture_output=True, text=True, timeout=timeout
            ),
        ),
        timeout=timeout + 10,
    )


def nats_url(nats_port: int = 4222) -> str:
    """Authorized NATS URL; honors native nats:// scheme override via
    E2E_NATS_URL, else E2E_NATS_TOKEN, else NATS_TOKEN from the repo .env."""
    env_url = os.environ.get("E2E_NATS_URL")
    if env_url:
        return env_url
    token = os.environ.get("E2E_NATS_TOKEN")
    if not token:
        env_path = REPO_ROOT / ".env"
        if env_path.exists():
            for line in env_path.read_text().splitlines():
                if line.startswith("NATS_TOKEN="):
                    token = line.split("=", 1)[1].strip()
                    break
    if not token or token in ("", "-"):
        return f"nats://localhost:{nats_port}"
    return f"nats://{token}@localhost:{nats_port}"


async def http_get(url: str, session: aiohttp.ClientSession,
                   timeout: float = 10.0) -> tuple:
    """GET and return (status, parsed-json-or-text). Network errors become (-1, "")."""
    try:
        async with session.get(url, timeout=aiohttp.ClientTimeout(total=timeout)) as resp:
            body = await resp.text()
            try:
                parsed = json.loads(body)
            except json.JSONDecodeError:
                parsed = body
            return resp.status, parsed
    except (aiohttp.ClientError, asyncio.TimeoutError):
        return -1, ""


async def http_post(url: str, body: dict, session: aiohttp.ClientSession,
                    timeout: float = 30.0) -> tuple:
    """POST JSON and return (status, parsed-json-or-text). Network errors become (-1, "")."""
    headers = {"Content-Type": "application/json"}
    try:
        async with session.post(
            url, data=json.dumps(body), headers=headers,
            timeout=aiohttp.ClientTimeout(total=timeout),
        ) as resp:
            text = await resp.text()
            try:
                parsed = json.loads(text)
            except json.JSONDecodeError:
                parsed = text
            return resp.status, parsed
    except (aiohttp.ClientError, asyncio.TimeoutError):
        return -1, ""


async def wait_for(predicate, timeout_s: float, step_s: float = 0.5,
                   desc: str = ""):
    """Poll an async `predicate()` until it returns truthy; else raise TimeoutError."""
    deadline = time.monotonic() + timeout_s
    last = None
    while time.monotonic() < deadline:
        last = await predicate()
        if last:
            return last
        await asyncio.sleep(step_s)
    raise TimeoutError(f"timed out waiting for {desc} (last result: {last!r})")


async def wait_http(url: str, session: aiohttp.ClientSession, want_status: int,
                    timeout_s: float, step_s: float = 1.0,
                    desc: str = "") -> int:
    """Poll an HTTP GET until it returns `want_status` (or any status in
    `want_status` when it is a tuple/container). Returns the final status."""
    async def _probe():
        status, _ = await http_get(url, session, timeout=min(timeout_s, 10))
        if isinstance(want_status, (tuple, list, set)):
            return status in want_status, status
        return status == want_status, status
    _, status = await wait_for(
        lambda: _probe(), timeout_s=timeout_s, step_s=step_s,
        desc=desc or f"{url} -> {want_status}",
    )
    return status
