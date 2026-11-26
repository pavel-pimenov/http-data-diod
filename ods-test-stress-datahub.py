#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import json
import datetime as dt
import time
import argparse
import ssl
import base64


try:
    from urllib2 import Request, urlopen, HTTPError, URLError
except ImportError:
    from urllib.request import Request, urlopen
    from urllib.error import HTTPError, URLError


# nl-sp-p3pg-ad1
URL = "https://odskc2.nlmk/DataHubRobots/DataHubSrv"
AUTH_BASIC = "Basic " + base64.b64encode("login1:pass1".encode("utf-8")).decode("ascii")
HEADERS = {
    "Content-Type": "application/json",
    "Authorization": AUTH_BASIC,
    "X-DataHub-Client-Id": "1000023-6666666666666",
}
DATA = {
    "jsonrpc": "2.0",
    "method": "GetHeatData",
    "id": 1,
    "params": {
        "timestamp": 0,
        "p_heat_no": 100
    }
}


def validate_jsonrpc_response(resp_json):
    """
    Валидация JSON-RPC ответа.
    Требования:
    - корень dict
    - есть поле "jsonrpc" == "2.0"
    Возвращает: (ok, reason)
    """
    if not isinstance(resp_json, dict):
        return False, "response is not a dict"

    if resp_json.get("jsonrpc") != "2.0":
        return False, "missing/invalid jsonrpc"

    return True, ""

def perform_post(url, headers, payload, timeout, verify):
    body = json.dumps(payload).encode("utf-8")
    req = Request(url, data=body)
    for key, value in headers.items():
        req.add_header(key, value)

    open_kwargs = {"timeout": timeout}
    if not verify:
        if hasattr(ssl, "_create_unverified_context"):
            open_kwargs["context"] = ssl._create_unverified_context()

    try:
        resp = urlopen(req, **open_kwargs)
        status_code = getattr(resp, "getcode", lambda: 200)()
        resp_text = resp.read()
        if not isinstance(resp_text, str):
            resp_text = resp_text.decode("utf-8", "replace")
        resp_headers = dict(resp.info())
        return status_code, resp_text, resp_headers
    except HTTPError as ex:
        resp_text = ex.read()
        if not isinstance(resp_text, str):
            resp_text = resp_text.decode("utf-8", "replace")
        return ex.code, resp_text, dict(ex.info())
    except URLError:
        raise
    except Exception:
        raise


def main():
    # Отключаем предупреждение urllib3 при verify=False
#     urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

    parser = argparse.ArgumentParser(description="Stress test for DataHub JSON-RPC GetHeatData.")
    parser.add_argument("--dump-dir", default="dumps", help="Каталог для дампов ошибок (запрос/ответ)")  # noqa: E501
    parser.add_argument("--start-heat", type=int, default=DATA["params"]["p_heat_no"], help="Начальный p_heat_no")  # noqa: E501
    parser.add_argument("--count", type=int, default=100, help="Сколько запросов сделать (инкрементируя p_heat_no)")  # noqa: E501
    parser.add_argument("--timeout", type=float, default=10.0, help="Timeout на запрос (сек)")
    parser.add_argument("--sleep", type=float, default=0.0, help="Пауза между запросами (сек)")
    parser.add_argument("--timestamp", type=int, default=DATA["params"]["timestamp"], help="timestamp в params")  # noqa: E501
    args = parser.parse_args()

    ok = 0
    err = 0
    started_at = dt.datetime.now()

    for i in range(args.count):
        heat_no = args.start_heat + i

        payload = {
            "jsonrpc": DATA["jsonrpc"],
            "method": DATA["method"],
            "id": i + 1,
            "params": {
                "timestamp": args.timestamp,
                "p_heat_no": heat_no,
            },
        }

        try:
            status_code, resp_text, resp_headers = perform_post(
                URL,
                HEADERS,
                payload,
                args.timeout,
                False,
            )
            # HTTP-уровень
            if status_code != 200:
                err += 1
                print("[{}/{}] p_heat_no={} HTTP {}: {}".format(i + 1, args.count, heat_no, status_code, resp_text[:500]))  # noqa: E501
            else:
                # JSON-RPC уровень
                try:
                    resp_json = json.loads(resp_text)
                except Exception:
                    err += 1
                    print("[{}/{}] p_heat_no={} Invalid JSON: {}".format(i + 1, args.count, heat_no, resp_text[:500]))  # noqa: E501
                else:
                    ok_rpc, reason = validate_jsonrpc_response(resp_json)
                    if ok_rpc:
                        ok += 1
                    else:
                        err += 1
                        ts = dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
                        dump_path = "{}/dump_{}_heat_{}.json".format(args.dump_dir, ts, heat_no)

                        dump_obj = {
                            "when": dt.datetime.now().isoformat(),
                            "request": {
                                "url": URL,
                                "headers": HEADERS,
                                "payload": payload,
                            },
                            "response": {
                                "status_code": status_code,
                                "headers": resp_headers,
                                "text": resp_text,
                                "json": resp_json,
                            },
                            "reason": reason,
                        }

                        try:
                            import os
                            if not os.path.isdir(args.dump_dir):
                                os.makedirs(args.dump_dir)
                            with open(dump_path, "w") as f:
                                json.dump(dump_obj, f, ensure_ascii=False, indent=2)
                        except Exception as ex_dump:
                            print("[{}/{}] p_heat_no={} Dump failed: {}".format(i + 1, args.count, heat_no, ex_dump))  # noqa: E501

                        print("[{}/{}] p_heat_no={} Invalid JSON-RPC ({}). Dump: {}".format(i + 1, args.count, heat_no, reason, dump_path))  # noqa: E501
        except Exception as ex:
            err += 1
            print("[{}/{}] p_heat_no={} Request failed: {}".format(i + 1, args.count, heat_no, ex))

        if args.sleep > 0:
            time.sleep(args.sleep)

    finished_at = dt.datetime.now()
    delta = finished_at - started_at
    took = (delta.days * 86400) + delta.seconds + (delta.microseconds / 1000000.0)
    total = ok + err
    rps = (total / took) if took > 0 else 0.0

    print("\n=== SUMMARY ===")
    print("Started:  {}".format(started_at.strftime("%Y-%m-%d %H:%M:%S")))
    print("Finished: {}".format(finished_at.strftime("%Y-%m-%d %H:%M:%S")))
    print("Took:     {:.3f} sec".format(took))
    print("Total:    {}".format(total))
    print("OK:       {}".format(ok))
    print("Errors:   {}".format(err))
    print("RPS:      {:.2f}".format(rps))

    return 0 if err == 0 else 2

if __name__ == "__main__":
    main()
