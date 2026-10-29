#!/usr/bin/env python3
"""Live chain transparency check: direct PTOKAX vs through the data diode.

The diode is transparent only if the same request produces the same status,
body bytes and upstream Content-Type both when sent straight to the hub and
when sent through the chain:

    client -> nginx :37015 -> l2-proxy :8888 -> NATS -> l2-worker -> ptokax :37014

The hub test-port is not published on the host, so direct requests are
executed inside a container attached to the hub network (`docker exec -i
<container> curl ...`, body on stdin). Chain requests go to the public nginx
port from the host.

`/fly-test-port` echoes the caller IP, so the direct request sets the same
`X-Real-IP` the chain derives from the nginx connection; the value is learned
with a probe request instead of hardcoding a Docker network address.

Usage:
    scripts/live-chain-check.py
    scripts/live-chain-check.py --chain-url http://localhost:37015 \
        --direct-container l2-worker --direct-url http://ptokax:37014
    scripts/live-chain-check.py --skip-large      # no 1 MiB scenario

Exit code 0 = every scenario matched, 1 = mismatch or transport error.
"""

import argparse
import http.client
import json
import subprocess
import sys
import zlib
from urllib.parse import urlsplit

USER_AGENT = "live-chain-check/1.0"
# The response headers compared between the two paths. nginx adds cache and
# traceparent headers of its own, so only the upstream content headers matter.
COMPARED_HEADERS = ("content-type",)


def build_scenarios(include_large=True):
    zlib_body = zlib.compress(json.dumps({"cmd": "zget"}).encode("utf-8"))
    binary_body = bytes(range(256)) * 16
    scenarios = [
        {
            "name": "json /fly-test-port",
            "path": "/fly-test-port",
            "content_type": "application/json",
            "body": b'{"cmd":"ping"}',
        },
        {
            "name": "plain /fly-stat",
            "path": "/fly-stat",
            "content_type": "text/plain",
            "body": b"live-chain-check plain body",
        },
        {
            "name": "zlib json /fly-stat",
            "path": "/fly-stat",
            "content_type": "application/octet-stream",
            "body": zlib_body,
        },
        {
            "name": "binary /fly-zget",
            "path": "/fly-zget",
            "content_type": "application/octet-stream",
            "body": binary_body,
        },
    ]
    if include_large:
        scenarios.append({
            "name": "1 MiB /fly-download",
            "path": "/fly-download",
            "content_type": "application/octet-stream",
            "body": bytes((i * 37 + 11) % 256 for i in range(1024 * 1024)),
        })
    return scenarios


class CheckError(Exception):
    pass


def chain_request(base_url, scenario, timeout, real_ip=None):
    parts = urlsplit(base_url)
    if parts.scheme not in ("http", "https"):
        raise CheckError(f"unsupported chain URL: {base_url}")
    conn_cls = (http.client.HTTPSConnection if parts.scheme == "https"
                else http.client.HTTPConnection)
    conn = conn_cls(parts.hostname, parts.port, timeout=timeout)
    headers = {
        "Content-Type": scenario["content_type"],
        "User-Agent": USER_AGENT,
    }
    if real_ip:
        headers["X-Real-IP"] = real_ip
    try:
        conn.request("POST", scenario["path"], body=scenario["body"],
                     headers=headers)
        resp = conn.getresponse()
        body = resp.read()
        resp_headers = {k.lower(): v for k, v in resp.getheaders()}
        return resp.status, resp_headers, body
    except OSError as exc:
        raise CheckError(f"chain request failed: {exc}") from exc
    finally:
        conn.close()


def direct_request(container, direct_url, scenario, timeout, real_ip=None):
    url = direct_url.rstrip("/") + scenario["path"]
    curl = [
        "curl", "-sS", "-i", "--max-time", str(timeout),
        "-X", "POST", "--data-binary", "@-",
        "-H", f"Content-Type: {scenario['content_type']}",
        "-H", f"User-Agent: {USER_AGENT}",
    ]
    if real_ip:
        curl += ["-H", f"X-Real-IP: {real_ip}"]
    curl.append(url)
    cmd = ["docker", "exec", "-i", container] + curl
    try:
        proc = subprocess.run(
            cmd, input=scenario["body"], capture_output=True, timeout=timeout + 30
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise CheckError(f"docker exec {container} failed: {exc}") from exc
    if proc.returncode != 0:
        stderr = proc.stderr.decode("utf-8", "replace").strip()
        raise CheckError(
            f"direct request failed (docker exec rc={proc.returncode}): {stderr}"
        )
    raw = proc.stdout
    head, sep, body = raw.partition(b"\r\n\r\n")
    if not sep:
        raise CheckError("direct response has no header/body separator")
    lines = head.decode("iso-8859-1").split("\r\n")
    try:
        status = int(lines[0].split()[1])
    except (IndexError, ValueError) as exc:
        raise CheckError(f"unparsable status line: {lines[0]!r}") from exc
    resp_headers = {}
    for line in lines[1:]:
        key, _, value = line.partition(":")
        if key:
            resp_headers[key.strip().lower()] = value.strip()
    return status, resp_headers, body


def probe_chain_ip(chain_url, timeout):
    """Learn the caller IP the chain reports for this host."""
    scenario = {"path": "/fly-test-port", "content_type": "application/json",
                "body": b'{"cmd":"probe"}'}
    status, _, body = chain_request(chain_url, scenario, timeout)
    if status != 200:
        raise CheckError(f"chain probe returned HTTP {status}")
    try:
        return json.loads(body.decode("utf-8")).get("ip")
    except (UnicodeDecodeError, ValueError):
        return None


def compare(scenario, chain, direct):
    problems = []
    chain_status, chain_headers, chain_body = chain
    direct_status, direct_headers, direct_body = direct

    if chain_status != direct_status:
        problems.append(f"status {chain_status} != {direct_status}")
    if chain_body != direct_body:
        problems.append(
            f"body {len(chain_body)} B != {len(direct_body)} B "
            f"(chain starts {chain_body[:32]!r}, direct {direct_body[:32]!r})"
        )
    for header in COMPARED_HEADERS:
        if chain_headers.get(header) != direct_headers.get(header):
            problems.append(
                f"{header} {chain_headers.get(header)!r} != "
                f"{direct_headers.get(header)!r}"
            )
    return problems


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--chain-url", default="http://localhost:37015",
                        help="public nginx entry point (default: %(default)s)")
    parser.add_argument("--direct-container", default="l2-worker",
                        help="container on the hub network used for direct "
                             "requests (default: %(default)s)")
    parser.add_argument("--direct-url", default="http://ptokax:37014",
                        help="hub test-port as seen from the container "
                             "(default: %(default)s)")
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="per-request timeout in seconds (default: %(default)s)")
    parser.add_argument("--skip-large", action="store_true",
                        help="skip the 1 MiB body scenario")
    args = parser.parse_args(argv)

    scenarios = build_scenarios(include_large=not args.skip_large)

    try:
        real_ip = probe_chain_ip(args.chain_url, args.timeout)
    except CheckError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print(f"chain:  {args.chain_url}")
    print(f"direct: docker exec {args.direct_container} -> {args.direct_url}")
    print(f"X-Real-IP for direct requests: {real_ip or '(not set)'}")
    print()
    print(f"{'scenario':<24} {'status':<12} {'body':<10} {'ctype':<10} result")
    print("-" * 70)

    failures = 0
    for scenario in scenarios:
        try:
            chain = chain_request(args.chain_url, scenario, args.timeout, real_ip)
            direct = direct_request(args.direct_container, args.direct_url,
                                    scenario, args.timeout, real_ip)
        except CheckError as exc:
            print(f"{scenario['name']:<24} {'-':<12} {'-':<10} {'-':<10} FAIL")
            print(f"  {exc}")
            failures += 1
            continue

        problems = compare(scenario, chain, direct)
        body_ok = "ok" if chain[2] == direct[2] else "DIFF"
        ctype_ok = ("ok" if chain[1].get("content-type") ==
                    direct[1].get("content-type") else "DIFF")
        status_ok = f"{chain[0]}/{direct[0]}"
        print(f"{scenario['name']:<24} {status_ok:<12} {body_ok:<10} "
              f"{ctype_ok:<10} {'OK' if not problems else 'FAIL'}")
        for problem in problems:
            print(f"  {problem}")
        if problems:
            failures += 1

    print()
    if failures:
        print(f"FAIL: {failures}/{len(scenarios)} scenario(s) diverged")
        return 1
    print(f"OK: {len(scenarios)}/{len(scenarios)} scenarios match byte-for-byte")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
