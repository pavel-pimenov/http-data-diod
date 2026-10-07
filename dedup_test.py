#!/usr/bin/env python3
"""
Integration test for worker-side request deduplication.

The proxy re-sends a NATS request/reply when it does not receive an answer
before its deadline (e.g. the reply was lost while NATS was reconnecting).
Without dedup the worker would call the L2 server twice for the same
request_id. The worker now caches produced responses (DedupCache, TTL 60s) and
serves repeated deliveries from cache.

This test speaks the raw NATS protocol over TCP (no client library needed):
it publishes the same request twice and asserts that:
  - both deliveries get a reply (the proxy would otherwise time out),
  - the L2 server was called exactly once (l2_worker_l2_calls_total +1),
  - one duplicate was served from cache (l2_worker_duplicate_requests_total +1).

Requires a running stack (./rebuild-and-run.sh).

Usage:
  python3 dedup_test.py
"""

import json
import socket
import sys
import time
import urllib.request

SUBJECT = "service.proxy"
NATS_HOST = "localhost"
NATS_PORT = 4222
WORKER_METRICS_URL = "http://localhost:19091/metrics"


class Colors:
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    NC = '\033[0m'


class NatsConnection:
    def __init__(self, host=NATS_HOST, port=NATS_PORT):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.buf = b""

    def send(self, data: bytes):
        self.sock.sendall(data)

    def _fill(self, min_bytes: int):
        while len(self.buf) < min_bytes:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise TimeoutError("connection closed")
            self.buf += chunk

    def read_line(self) -> bytes:
        idx = self.buf.find(b"\r\n")
        while idx == -1:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise TimeoutError("connection closed")
            self.buf += chunk
            idx = self.buf.find(b"\r\n")
        line = self.buf[:idx]
        self.buf = self.buf[idx + 2:]
        return line

    def read_exact(self, n: int) -> bytes:
        self._fill(n)
        data = self.buf[:n]
        self.buf = self.buf[n:]
        return data

    def read_msg(self):
        line = self.read_line()
        parts = line.split()
        if not parts or parts[0] != b"MSG":
            return None
        # MSG <subject> <sid> [<reply>] [<hdr_len>] <len>
        tokens = parts[1:]
        subject, sid = tokens[0].decode(), tokens[1].decode()
        if len(tokens) == 3:
            reply, hdr_len = None, 0
            length = int(tokens[2])
        elif len(tokens) == 4:
            # 3rd token is either the reply subject or the header length
            reply = None if tokens[2].isdigit() else tokens[2].decode()
            hdr_len = int(tokens[2]) if tokens[2].isdigit() else 0
            length = int(tokens[3])
        elif len(tokens) == 5:
            reply = tokens[2].decode()
            hdr_len = int(tokens[3])
            length = int(tokens[4])
        else:
            raise ValueError(f"unsupported MSG line: {parts!r}")
        total = hdr_len + length
        data = self.read_exact(total)
        self.read_exact(2)  # trailing \r\n
        return {"subject": subject, "sid": sid, "reply": reply,
                "headers": data[:hdr_len], "data": data[hdr_len:].decode()}

    def close(self):
        self.sock.close()


def read_msg_with_retries(conn, budget_s=30.0):
    conn.sock.settimeout(1)
    deadline = time.time() + budget_s
    while time.time() < deadline:
        try:
            msg = conn.read_msg()
            if msg is not None:
                return msg
        except (TimeoutError, socket.timeout):
            continue
    return None


def metric_value(name: str) -> float:
    text = urllib.request.urlopen(WORKER_METRICS_URL, timeout=10).read().decode()
    for line in text.splitlines():
        if line.startswith(name):
            return float(line.split()[1])
    return 0.0


def main() -> int:
    request_id = f"dedup-test-{int(time.time() * 1000)}-{__import__('secrets').token_hex(4)}"
    payload = {
        "request_id": request_id,
        "method": "POST",
        "path": "/",
        "body": json.dumps({"value": 1}),
        "timestamp": int(time.time() * 1000),
    }
    payload_bytes = json.dumps(payload).encode()

    l2_calls_before = metric_value("l2_worker_l2_calls_total")
    dup_before = metric_value("l2_worker_duplicate_requests_total")

    print(f"Sending duplicate NATS requests, request_id={request_id}")

    conn = NatsConnection()
    inbox = f"dedup.inbox.{request_id}"
    conn.send(f"SUB {inbox} 1\r\n".encode())

    # First delivery -> worker calls L2 server and caches the response
    conn.send(f"PUB {SUBJECT} {inbox} {len(payload_bytes)}\r\n".encode()
              + payload_bytes + b"\r\n")
    reply1 = read_msg_with_retries(conn)
    if reply1 is None:
        print(f"{Colors.RED}FAIL: no reply for first delivery{Colors.NC}")
        conn.close()
        return 1
    body1 = json.loads(reply1["data"])
    print(f"  reply 1: status_code={body1.get('status_code')}")

    # Second (duplicate) delivery -> served from dedup cache
    conn.send(f"PUB {SUBJECT} {inbox} {len(payload_bytes)}\r\n".encode()
              + payload_bytes + b"\r\n")
    reply2 = read_msg_with_retries(conn)
    if reply2 is None:
        print(f"{Colors.RED}FAIL: no reply for duplicate delivery "
              f"(proxy would have timed out){Colors.NC}")
        conn.close()
        return 1
    body2 = json.loads(reply2["data"])
    print(f"  reply 2: status_code={body2.get('status_code')}")
    conn.close()

    ok = True
    if body1.get("status_code") != 200:
        print(f"{Colors.RED}FAIL: first delivery status "
              f"{body1.get('status_code')} != 200{Colors.NC}")
        ok = False
    if body2.get("status_code") != 200:
        print(f"{Colors.RED}FAIL: duplicate delivery status "
              f"{body2.get('status_code')} != 200{Colors.NC}")
        ok = False
    if reply1["data"] != reply2["data"]:
        print(f"{Colors.RED}FAIL: duplicate reply differs from the original "
              f"(cached response expected){Colors.NC}")
        ok = False
    else:
        print("  OK: duplicate reply matches the cached original")

    time.sleep(1)  # let the worker flush its metrics
    l2_calls_after = metric_value("l2_worker_l2_calls_total")
    dup_after = metric_value("l2_worker_duplicate_requests_total")
    l2_delta = l2_calls_after - l2_calls_before
    dup_delta = dup_after - dup_before
    print(f"  metrics: l2_worker_l2_calls_total delta={l2_delta}, "
          f"l2_worker_duplicate_requests_total delta={dup_delta}")

    if l2_delta != 1:
        print(f"{Colors.RED}FAIL: expected exactly 1 L2 call for 2 deliveries, "
              f"got {l2_delta}{Colors.NC}")
        ok = False
    if dup_delta < 1:
        print(f"{Colors.RED}FAIL: expected >= 1 duplicate served from cache, "
              f"got {dup_delta}{Colors.NC}")
        ok = False

    if ok:
        print(f"{Colors.GREEN}All dedup checks passed.{Colors.NC}")
        return 0
    print(f"{Colors.RED}Dedup test failed.{Colors.NC}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
