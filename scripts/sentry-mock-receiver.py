#!/usr/bin/env python3
"""Mock Sentry ingest endpoint for E2E tests of the http-data-diod Sentry
client (sentry_client.cpp).

The real client POSTs the envelope to `<dsn-host>/api/<project_id>/envelope/`
with `Content-Type: application/x-sentry-envelope` and the newline-delimited
envelope body (header / auth / item-header / event JSON, see build_envelope()).

Usage:
  scripts/sentry-mock-receiver.py --port 9001            # run forever
  scripts/sentry-mock-receiver.py --wait-events 1 --timeout 120
  scripts/sentry-mock-receiver.py --events-file /tmp/sentry-events.jsonl

Exit codes (wait mode):
  0 = received the requested number of events
  3 = timed out before receiving the requested number of events
"""

import argparse
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread, Timer

_events_seen = [0]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _read_and_store(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length).decode("utf-8", "replace")
        lines = raw.split("\n")

        header = {}
        auth = {}
        item_header = {}
        payload = {}
        if len(lines) >= 1 and lines[0].strip():
            try:
                header = json.loads(lines[0])
            except Exception:
                header = {"_raw": lines[0]}
        if len(lines) >= 2 and lines[1].strip():
            auth = {"_raw": lines[1]}
        if len(lines) >= 3 and lines[2].strip():
            try:
                item_header = json.loads(lines[2])
            except Exception:
                item_header = {"_raw": lines[2]}
        if len(lines) >= 4 and lines[3].strip():
            try:
                payload = json.loads(lines[3])
            except Exception:
                payload = {"_raw": lines[3]}

        record = {
            "event_id": header.get("event_id", ""),
            "sent_at": header.get("sent_at", ""),
            "auth": auth,
            "item_type": item_header.get("type", ""),
            "payload": payload,
        }
        _events_seen[0] += 1
        with open(os.environ.get("SENTRY_MOCK_STDOUT", "/dev/null"), "a") as out:
            out.write("EVENT " + json.dumps(record) + "\n")
        print("EVENT " + json.dumps(record), flush=True)

        event = payload
        print(
            "  event_id={} level={} message={!r} fingerprint={} "
            "service={} request_id={} tags={}".format(
                header.get("event_id", "")[:12],
                event.get("level", ""),
                event.get("message", "")[:120],
                event.get("fingerprint", []),
                event.get("tags", {}).get("service", ""),
                (event.get("tags", {}) or {}).get("request_id", ""),
                event.get("tags", {}),
            ),
            flush=True,
        )
        return record

    def do_POST(self):
        record = self._read_and_store()
        body = json.dumps({"id": record.get("event_id", "")}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=9001)
    parser.add_argument("--wait-events", type=int, default=0,
                        help="exit 0 after this many events (0 = run forever)")
    parser.add_argument("--timeout", type=int, default=120,
                        help="give up and exit 3 after this many seconds")
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()

    print(f"SENTRY-MOCK listening on {args.host}:{args.port}",
          flush=True)

    def on_timeout():
        print("SENTRY-MOCK timeout: no events received", flush=True)
        os._exit(3)

    if args.wait_events:
        timer = Timer(args.timeout, on_timeout)
        timer.daemon = True
        timer.start()
        while _events_seen[0] < args.wait_events:
            thread.join(timeout=0.1)
        print(f"SENTRY-MOCK received {_events_seen[0]} events — done",
              flush=True)
        os._exit(0)
    else:
        thread.join()


if __name__ == "__main__":
    main()
