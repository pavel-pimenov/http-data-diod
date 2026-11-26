#!/usr/bin/env python3

import json
import ssl
import sys
import time
import base64
import traceback
from datetime import datetime


def log(message):
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    print(f"[{timestamp}] {message}")


def main():
    url = "http://nl-sp-p3pg-ad1.ao.nlmk:29003/DataHubRobots/DataHubSrv"
    timeout_sec = 10

    query = {
        "params": {
            "p_heat_no": 23143,
            "timestamp": 0
        },
        "jsonrpc": "2.0",
        "method": "GetHeatData",
        "id": 1
    }

    payload = json.dumps(query, ensure_ascii=False).encode("utf-8")
    auth_basic = "Basic " + base64.b64encode("login1:pass1".encode("utf-8")).decode("ascii")
    headers = {
        "Content-Type": "application/json",
        "Authorization": auth_basic,
        "X-DataHub-Client-Id": "1000023-6666666666666",
    }

    log(f"Target endpoint: {url}")
    log(f"Timeout: {timeout_sec} sec")
    log(f"JSON query: {json.dumps(query, ensure_ascii=False)}")
    log(f"Payload size: {len(payload)} bytes")

    started_at = time.monotonic()

    try:
        from urllib.request import Request, urlopen, HTTPError
        from urllib.error import URLError

        req = Request(url, data=payload, method="POST")
        for key, value in headers.items():
            req.add_header(key, value)

        context = ssl._create_unverified_context()

        log(f"Sending POST request ({len(payload)} bytes)")
        resp = urlopen(req, timeout=timeout_sec, context=context)
        status_code = getattr(resp, "getcode", lambda: 200)()
        resp_text = resp.read()
        if not isinstance(resp_text, str):
            resp_text = resp_text.decode("utf-8", "replace")
        resp_headers = dict(resp.info())
        resp.close()

        elapsed = time.monotonic() - started_at
        log(f"Status code: {status_code}")
        log(f"Response headers: {resp_headers}")
        log(f"Request completed successfully in {elapsed:.3f} sec")

        try:
            response_json = json.loads(resp_text)
            log("Decoded JSON response:")
            print(json.dumps(response_json, ensure_ascii=False, indent=2))
        except json.JSONDecodeError as exc:
            log(f"Failed to decode response as JSON: {exc}")
            log(f"Raw response text: {resp_text[:500]}")

    except HTTPError as exc:
        elapsed = time.monotonic() - started_at
        log(f"Request failed after {elapsed:.3f} sec")
        log(f"HTTP status: {exc.code}")
        log(f"Response body: {exc.read().decode('utf-8', 'replace')[:500]}")
        sys.exit(1)
    except URLError as exc:
        elapsed = time.monotonic() - started_at
        log(f"Request failed after {elapsed:.3f} sec")
        log(f"URL error: {exc.reason}")
        sys.exit(1)
    except Exception as exc:
        elapsed = time.monotonic() - started_at
        log(f"Request failed after {elapsed:.3f} sec")
        log(f"Exception type: {type(exc).__name__}")
        log(f"Exception message: {exc}")
        log("Traceback:")
        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()
