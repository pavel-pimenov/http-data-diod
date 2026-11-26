#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
l2-heat-scraper: перебирает плавки (heat_no) от заданного номера ВНИЗ на N штук,
шлёт запрос GetHeatData через прокси-цепочку l2 (nginx:29003 -> l2-proxy -> worker ->
внешний ODS DataHub) и сохраняет ответ каждой плавки в отдельный JSON-файл.

Формат запроса (JSON-RPC 2.0):
    POST /DataHubRobots/DataHubSrv
    {"jsonrpc":"2.0","method":"GetHeatData","id":<id>,"params":{"timestamp":0,"p_heat_no":<heat>}}
Заголовки: Authorization: Basic <base64(user:pass)>, X-DataHub-Client-Id.

Конфигурация:
  HEAT_SCRAPE_URL        — base URL прокси
                          (default http://nl-sp-p3pg-ad1.ao.nlmk:29003/DataHubRobots/DataHubSrv)
  HEAT_SCRAPE_USER       — Basic-пользователь (default из env, напр. login1)
  HEAT_SCRAPE_PASSWORD   — Basic-пароль
  HEAT_SCRAPE_CLIENT_ID  — X-DataHub-Client-Id
  HEAT_SCRAPE_START      — стартовый номер плавки (default 23143)
  HEAT_SCRAPE_COUNT      — сколько плавок вниз (default 100)
  HEAT_SCRAPE_OUTPUT_DIR — каталог для JSON (default ./heat-output)

CLI переопределяет env: --url --start --count --output-dir --user --password --client-id.
"""

import argparse
import base64
import json
import os
import ssl
import time
import urllib.request
from urllib.error import HTTPError, URLError


def env(name, default=""):
    return os.environ.get(name, default).strip()


def build_headers(user, password, client_id):
    """Формирует заголовки запроса: Content-Type, Basic auth, X-DataHub-Client-Id."""
    headers = {
        "Content-Type": "application/json",
        "X-DataHub-Client-Id": client_id,
    }
    if user:
        token = base64.b64encode(
            "{}:{}".format(user, password).encode("utf-8")).decode("ascii")
        headers["Authorization"] = "Basic " + token
    return headers


def get_heat_data(url, heat_no, headers, req_id=1, timeout=30):
    """Шлёт запрос GetHeatData для плавки heat_no через прокси. Возвращает
    (status, body_dict_or_error). body_dict — распарсенный JSON ответа."""
    payload = {
        "jsonrpc": "2.0",
        "method": "GetHeatData",
        "id": req_id,
        "params": {"timestamp": 0, "p_heat_no": heat_no},
    }
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    # Проверка TLS отключена: прокси/ODS могут иметь самоподписанный сертификат.
    ctx = ssl._create_unverified_context()
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            body = resp.read().decode("utf-8", "replace")
            try:
                parsed = json.loads(body)
            except json.JSONDecodeError:
                parsed = {"raw": body}
            return resp.getcode(), parsed
    except HTTPError as ex:
        body = ex.read().decode("utf-8", "replace")
        try:
            parsed = json.loads(body)
        except json.JSONDecodeError:
            parsed = {"raw": body}
        return ex.code, parsed
    except URLError as ex:
        return 0, {"error": str(ex.reason if ex.reason else ex)}


def scrape(start, count, url, headers, output_dir, req_id_seed=1):
    """Перебирает плавки start, start-1, ..., start-count+1 и сохраняет JSON."""
    os.makedirs(output_dir, exist_ok=True)
    results = {"total": 0, "ok": 0, "error": 0, "files": []}
    for i in range(count):
        heat = start - i
        status, parsed = get_heat_data(url, heat, headers, req_id=req_id_seed + i)
        is_error = "error" in parsed or status >= 400
        record = {
            "heat_no": heat,
            "http_status": status,
            "scraped_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "response": parsed,
        }
        fname = os.path.join(output_dir, "heat_{:06d}.json".format(heat))
        with open(fname, "w", encoding="utf-8") as fh:
            json.dump(record, fh, ensure_ascii=False, indent=1)
        results["total"] += 1
        results["files"].append(fname)
        if is_error:
            results["error"] += 1
            print("heat_no={} -> HTTP {} (empty/error)".format(heat, status))
        else:
            results["ok"] += 1
            nproc = 0
            try:
                nproc = len(parsed.get("result", {}).get("data", {}).get("processes", []))
            except AttributeError:
                nproc = 0
            print("heat_no={} -> OK ({} processes)".format(heat, nproc))
        # Небольшая задержка, чтобы не нагружать ODS.
        time.sleep(0.05)
    summary = os.path.join(output_dir, "_scrape_summary.json")
    with open(summary, "w", encoding="utf-8") as fh:
        json.dump(results, fh, ensure_ascii=False, indent=1)
    print("Готово: {} плавок, OK={}, error={}".format(
        results["total"], results["ok"], results["error"]))
    print("Сводка: {}".format(summary))
    return results


def main():
    parser = argparse.ArgumentParser(
        description="Перебор плавок GetHeatData через прокси l2 и сохранение JSON")
    parser.add_argument("--url", default=env("HEAT_SCRAPE_URL",
        "http://nl-sp-p3pg-ad1.ao.nlmk:29003/DataHubRobots/DataHubSrv"))
    parser.add_argument("--start", type=int, default=int(env("HEAT_SCRAPE_START", "23143")))
    parser.add_argument("--count", type=int, default=int(env("HEAT_SCRAPE_COUNT", "100")))
    parser.add_argument("--output-dir", default=env("HEAT_SCRAPE_OUTPUT_DIR", "./heat-output"))
    parser.add_argument("--user", default=env("HEAT_SCRAPE_USER", ""))
    parser.add_argument("--password", default=env("HEAT_SCRAPE_PASSWORD", ""))
    parser.add_argument("--client-id", default=env("HEAT_SCRAPE_CLIENT_ID", ""))
    args = parser.parse_args()

    headers = build_headers(args.user, args.password, args.client_id)
    print("Прокси: {}".format(args.url))
    print("Старт плавка: {} вниз на {} шт".format(args.start, args.count))
    print("Вывод: {}".format(args.output_dir))
    scrape(args.start, args.count, args.url, headers, args.output_dir)
    return 0


if __name__ == "__main__":
    main()
