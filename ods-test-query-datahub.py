#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import json
import sys
import time
import datetime as dt
import copy
import ssl
import base64

try:
    from urllib2 import Request, urlopen, HTTPError, URLError
except ImportError:
    from urllib.request import Request, urlopen
    from urllib.error import HTTPError, URLError


# nl-sp-p3pg-ad1
# URL = "http://localhost:29003/DataHubRobots/DataHubSrv"
URL = "http://nl-sp-p3pg-ad1.ao.nlmk:29003/DataHubRobots/DataHubSrv"
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
        "p_heat_no": 23143
    }
}


def print_text(text):
    if sys.version_info[0] < 3:
        if isinstance(text, unicode):
            sys.stdout.write(text.encode("utf-8"))
        else:
            sys.stdout.write(str(text))
        sys.stdout.write("\n")
    else:
        print(text)


def print_json(label, data):
    print_text(label)
    print_text(json.dumps(data, indent=2, ensure_ascii=False))


def perform_request(url, method, headers, payload, verify):
    body = None
    if payload is not None:
        body = json.dumps(payload).encode("utf-8")

    req = Request(url, data=body)
    if hasattr(req, "get_method"):
        req.get_method = lambda: method

    for key, value in headers.items():
        req.add_header(key, value)

    open_kwargs = {}
    if not verify and hasattr(ssl, "_create_unverified_context"):
        open_kwargs["context"] = ssl._create_unverified_context()

    try:
        resp = urlopen(req, **open_kwargs)
        status_code = getattr(resp, "getcode", lambda: 200)()
        resp_text = resp.read()
        if not isinstance(resp_text, str):
            resp_text = resp_text.decode("utf-8", "replace")
        resp_headers = dict(resp.info())
        return status_code, resp_headers, resp_text
    except HTTPError as ex:
        resp_text = ex.read()
        if not isinstance(resp_text, str):
            resp_text = resp_text.decode("utf-8", "replace")
        return ex.code, dict(ex.info()), resp_text
    except URLError:
        raise
    except Exception:
        raise


def main():
    try:
        # url_ods = "https://k1l25app.ao.nlmk:ords/f?p=102:2:1692545272939::NO:::"
        # "https://k1l25app.ao.nlmk/ords/ods1/r/102/files/static/v25Y/favicon.ico"
        # "https://k1l25app.ao.nlmk/i/app_ui/font/apex-5-icon-font.woff2"
        if 1 == 0:  # временно отклчил - не работает.
            url_ods = "http://nl-sp-p3pg-ad1.ao.nlmk:29003/ords/ods1/r/102/files/static/v25Y/favicon.ico"  # noqa: E501
            print_text("\n=== ODS запрос ({}) ===".format(url_ods))
            try:
                print_text("http query: url = {}".format(url_ods))
                ods_start = time.time()
                ods_status, ods_headers, ods_text = perform_request(
                    url_ods,
                    "GET",
                    {},
                    None,
                    False
                )
                ods_elapsed = time.time() - ods_start
                print_text("Status code: {}".format(ods_status))
                print_text("Headers: {}".format(ods_headers))
                print_text("Request time: {:.3f} seconds".format(ods_elapsed))
                try:
                    print_json("\nJSON response:", json.loads(ods_text))
                except Exception:
                    print_text("\nНе-JSON ответ от ODS (первые 500 символов):")
                    print_text(ods_text[:500])
            except Exception as ods_ex:
                print_text("ODS request failed: {}".format(ods_ex))
            return

        # Первый запрос - оригинальный DataHub
        start_time = time.time()
        print_text("JSON query: {}".format(json.dumps(DATA)))
        status_code, response_headers, response_text = perform_request(
            URL,
            "POST",
            HEADERS,
            DATA,
            False
        )
        elapsed = time.time() - start_time
        print_text("Status code: {}".format(status_code))
        print_text("Headers: {}".format(response_headers))
        print_text("Request time: {:.3f} seconds".format(elapsed))
        try:
            print_json("\nJSON response:", json.loads(response_text))
        except Exception as e:
            print_text("Could not decode JSON: {}".format(e))

        # --- Добавляем два запроса к GantPool/GetGantDescr ---
        url1 = "http://nl-sp-p3pg-ad1.ao.nlmk:29003/GantPool/GetGantDescr"
        # url2 = "https://odskc2.nlmk/GantPool/GetGantDescr"

        # curl -k --request POST -H 'Content-Type: application/json' --data-raw '{"jsonrpc":"2.0","method":"getgantdata","params":{"ganttype":1,"start_time":1740000000000,"end_time":1740072000000,"my_generation":0,"data_source":"current","snapshot_id":0,"app_session":"10652747179681","app_id":"106","app_page_id":"86"},"id":1}' -v 'https://odskc2.nlmk:8181/GantPool/GetGantDescr'  # noqa: E501

        date_now = dt.datetime.now()
        gant_params = {
            "ganttype": 1,
            "start_time": int(time.mktime((date_now - dt.timedelta(hours=10)).timetuple()) * 1000),
            "end_time": int(time.mktime((date_now + dt.timedelta(hours=10)).timetuple()) * 1000),
            "my_generation": 0,
            "data_source": "current",
            "snapshot_id": 0,
            "app_session": "10652747179681",
            "app_id": "106",
            "app_page_id": "86"
        }
        gant_payload = {"jsonrpc": "2.0", "method": "getgantdata", "params": gant_params, "id": 1}
        gant_headers = {
            "Content-Type": "application/json",
            "Authorization": AUTH_BASIC,
            "X-DataHub-Client-Id": "1000023-6666666666666",
        }

        show_full_gant = "--full-gant" in sys.argv

        for i, gant_url in enumerate([url1], start=1):
            print_text("\n=== GantPool запрос #{} ({}) ===".format(i, gant_url))
            try:
                gant_start = time.time()
                status_code, resp_headers, resp_text = perform_request(
                    gant_url,
                    "POST",
                    gant_headers,
                    gant_payload,
                    False
                )
                gant_elapsed = time.time() - gant_start
                print_text("Status code: {}".format(status_code))
                print_text("Headers: {}".format(resp_headers))
                print_text("Request time: {:.3f} seconds".format(gant_elapsed))
                try:
                    resp_json = json.loads(resp_text)
                    if gant_url == url1:
                        if show_full_gant:
                            print_json("\nJSON response (полный, формат.):", resp_json)
                        else:
                            # Вырезаем только первые 2 элемента массива "elements", если есть
                            resp_short = copy.deepcopy(resp_json)
                            try:
                                elems = resp_short.get("result", {}).get("elements", None)
                                if elems and isinstance(elems, list):
                                    resp_short["result"]["elements"] = elems[:2]
                                print_json("\nJSON response (elements[:2]):", resp_short)
                            except Exception as e:
                                print_text("Ошибка при обрезке elements: {}".format(e))
                                print_text(json.dumps(resp_json, indent=2, ensure_ascii=False))
                    else:
                        print_json("\nJSON response:", resp_json)
                except Exception as e:
                    print_text("Could not decode JSON: {}".format(e))
            except Exception as gant_ex:
                print_text("Request failed: {}".format(gant_ex))

    except Exception as ex:
        print_text("Request failed: {}".format(ex))
        sys.exit(1)
    return 0


if __name__ == "__main__":
    main()
