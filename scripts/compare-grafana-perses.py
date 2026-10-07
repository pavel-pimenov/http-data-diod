#!/usr/bin/env python3
"""Live structural + data comparison of Grafana and Perses dashboards.

Both systems are fed from the same definitions (scripts/generate-*), but a
converter drift would only show up when the running instances are compared.
This script does exactly that:

  structural  dashboard set, panel count (Grafana rows expanded), the set of
              PromQL expressions (after normalising `${vm:regex}` <-> `$vm`)
              and row/grid section titles;
  data        runs a sample of PromQL from every dashboard through Grafana
              (/api/ds/query) and Perses (/proxy/globaldatasources/...) and
              compares series keys and values (relative tolerance, since the
              two queries are issued a few ms apart).

Usage:
  python3 scripts/compare-grafana-perses.py                # structural + data
  python3 scripts/compare-grafana-perses.py --no-live      # structural only
  python3 scripts/compare-grafana-perses.py --allow-down   # rc=0 if a UI is down

Env: GRAFANA_URL (default http://localhost:3000), GRAFANA_USER/GRAFANA_PASSWORD
     (admin/admin), PERSES_URL (default http://localhost:8089).
Exit codes: 0 = parity, 1 = mismatch, 2 = a service is unreachable.
"""

import argparse
import base64
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

SAMPLES_PER_DASHBOARD = 4  # PromQL samples taken from each dashboard for the data check
REL_TOL = 1e-3
ABS_TOL = 1e-6

_VAR_RE = re.compile(r"\$\{(\w+):regex\}|\$\{(\w+)\}")


def normalize_expr(expr):
    """`${vm:regex}` / `${vm}` (Grafana) and `$vm` (Perses) mean the same here."""
    return _VAR_RE.sub(lambda m: "$" + (m.group(1) or m.group(2)), expr).strip()


def has_unresolved_var(expr):
    return "$" in expr


def http_json(url, method="GET", payload=None, headers=None):
    req = urllib.request.Request(url, data=payload, method=method)
    req.add_header("Content-Type", "application/json")
    for key, value in (headers or {}).items():
        req.add_header(key, value)
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.load(resp)


def grafana_headers():
    token = base64.b64encode(
        f"{os.environ.get('GRAFANA_USER', 'admin')}:"
        f"{os.environ.get('GRAFANA_PASSWORD', 'admin')}".encode()
    ).decode()
    return {"Authorization": f"Basic {token}"}


def grafana_dashboards(base):
    found = http_json(f"{base}/api/search?type=dash-db", headers=grafana_headers())
    result = {}
    for item in found:
        uid = item["uid"]
        doc = http_json(f"{base}/api/dashboards/uid/{uid}",
                        headers=grafana_headers())["dashboard"]
        panels, rows = [], []
        for panel in doc.get("panels", []):
            if panel.get("type") == "row":
                rows.append(panel.get("title"))
                panels.extend(panel.get("panels", []))
            else:
                panels.append(panel)
        exprs = set()
        for panel in panels:
            for target in panel.get("targets") or []:
                expr = target.get("expr")
                if isinstance(expr, str) and expr.strip():
                    exprs.add(normalize_expr(expr))
        result[uid] = {"title": item["title"], "panels": len(panels),
                       "exprs": sorted(exprs), "rows": rows}
    return result


def perses_dashboards(base):
    items = http_json(f"{base}/api/v1/dashboards")
    result = {}
    for item in items:
        name = item["metadata"]["name"]
        spec = item["spec"]
        panels = list(spec.get("panels", {}).values())
        exprs = set()
        for panel in panels:
            for query in panel.get("spec", {}).get("queries") or []:
                expr = query.get("spec", {}).get("plugin", {}).get("spec", {}).get("query")
                if isinstance(expr, str) and expr.strip():
                    exprs.add(normalize_expr(expr))
        rows = [g.get("spec", {}).get("display", {}).get("title")
                for g in spec.get("layouts", [])]
        result[name] = {"title": spec.get("display", {}).get("name"),
                        "panels": len(panels), "exprs": sorted(exprs), "rows": rows}
    return result


def grafana_query(base, expr):
    body = json.dumps({
        "queries": [{"refId": "A", "expr": expr, "instant": True,
                     "datasource": {"type": "prometheus", "uid": "prometheus"}}],
        "from": "now-5m", "to": "now",
    }).encode()
    res = http_json(f"{base}/api/ds/query", method="POST", payload=body,
                    headers=grafana_headers())["results"]["A"]
    if res.get("error"):
        raise RuntimeError(res["error"])
    values = {}
    for frame in res.get("frames", []):
        fields = frame.get("schema", {}).get("fields", [])
        data = frame.get("data", {}).get("values") or []
        for idx, field in enumerate(fields):
            if field.get("name") == "Time" or idx >= len(data):
                continue
            labels = field.get("labels") or {}
            key = tuple(sorted(labels.items())) if labels else "value"
            column = data[idx]
            if column and column[-1] is not None:
                values[key] = float(column[-1])
    return values


def perses_samples(base, expr):
    url = f"{base}/proxy/globaldatasources/prometheus/api/v1/query"
    res = http_json(url + "?" + urllib.parse.urlencode({"query": expr}))
    if res.get("status") != "success":
        raise RuntimeError(json.dumps(res)[:200])
    return res["data"]["result"]


def perses_query(base, expr):
    values = {}
    for sample in perses_samples(base, expr):
        labels = sample.get("metric", {})
        key = tuple(sorted(labels.items())) if labels else "value"
        values[key] = float(sample["value"][1])
    return values


def vm_label_value(perses_url):
    try:
        samples = perses_samples(perses_url, "count by (vm) (up)")
        return samples[0]["metric"].get("vm", "") if samples else ""
    except (urllib.error.URLError, RuntimeError, KeyError, IndexError) as exc:
        print(f"  warn: cannot resolve vm label value: {exc}", file=sys.stderr)
        return ""


def compare_structure(g_dash, p_dash):
    issues = 0
    if set(g_dash) != set(p_dash):
        issues += 1
        print(f"  DIFF dashboard set: only Grafana={sorted(set(g_dash) - set(p_dash))}"
              f" only Perses={sorted(set(p_dash) - set(g_dash))}")
    for uid in sorted(set(g_dash) & set(p_dash)):
        g, p = g_dash[uid], p_dash[uid]
        if g["panels"] != p["panels"]:
            issues += 1
            print(f"  DIFF panels G={g['panels']} P={p['panels']}")
        if g["exprs"] != p["exprs"]:
            issues += 1
            only_g = sorted(set(g["exprs"]) - set(p["exprs"]))
            only_p = sorted(set(p["exprs"]) - set(g["exprs"]))
            print(f"  DIFF expr sets: onlyG={len(only_g)} onlyP={len(only_p)}")
            for expr in only_g[:5]:
                print(f"    G-only: {expr[:110]}")
            for expr in only_p[:5]:
                print(f"    P-only: {expr[:110]}")
        if g["rows"] != p["rows"]:
            issues += 1
            print(f"  DIFF sections G={g['rows']} P={p['rows']}")
    if not issues:
        print(f"  structure (panels/exprs/sections) matches for all "
              f"{len(set(g_dash) & set(p_dash))} dashboards")
    return issues


def sample_exprs(exprs, limit):
    if len(exprs) <= limit:
        return list(exprs)
    step = len(exprs) / limit
    return [exprs[int(i * step)] for i in range(limit)]


def compare_data(g_url, p_url, g_dash, vm_value):
    issues = checked = empty = 0
    for uid in sorted(g_dash):
        exprs = [e.replace("$vm", vm_value) if vm_value else e
                 for e in g_dash[uid]["exprs"]]
        exprs = [e for e in exprs if not has_unresolved_var(e)]
        for expr in ["up"] + sample_exprs(exprs, SAMPLES_PER_DASHBOARD):
            try:
                g_vals = grafana_query(g_url, expr)
                p_vals = perses_query(p_url, expr)
            except (urllib.error.URLError, RuntimeError, KeyError) as exc:
                issues += 1
                print(f"  ERR  {uid}: {expr[:70]} -> {exc}")
                continue
            checked += 1
            if not g_vals and not p_vals:
                empty += 1
                continue
            if set(g_vals) != set(p_vals):
                issues += 1
                print(f"  DIFF {uid} series keys for {expr[:70]}")
                print(f"    G={sorted(g_vals)}")
                print(f"    P={sorted(p_vals)}")
                continue
            for key in g_vals:
                gv, pv = g_vals[key], p_vals[key]
                if abs(gv - pv) > max(ABS_TOL, REL_TOL * max(abs(gv), abs(pv))):
                    issues += 1
                    print(f"  DIFF {uid} {key}: G={gv} P={pv} for {expr[:60]}")
    print(f"  data samples compared: {checked} (empty on both sides: {empty})")
    if checked and empty == checked:
        issues += 1
        print("  ERR all data samples are empty — nothing was actually compared")
    return issues


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-live", action="store_true",
                        help="structural comparison only (no PromQL samples)")
    parser.add_argument("--allow-down", action="store_true",
                        help="exit 0 when Grafana/Perses are unreachable")
    args = parser.parse_args()

    g_url = os.environ.get("GRAFANA_URL", "http://localhost:3000").rstrip("/")
    p_url = os.environ.get("PERSES_URL", "http://localhost:8089").rstrip("/")

    try:
        g_dash = grafana_dashboards(g_url)
        p_dash = perses_dashboards(p_url)
    except (urllib.error.URLError, OSError, KeyError) as exc:
        print(f"service unreachable: {exc}")
        return 0 if args.allow_down else 2

    print(f"structural comparison: Grafana={len(g_dash)} Perses={len(p_dash)} dashboards")
    issues = compare_structure(g_dash, p_dash)

    if not args.no_live:
        vm_value = vm_label_value(p_url)
        print(f"data comparison (vm={vm_value or '<unknown>'}):")
        issues += compare_data(g_url, p_url, g_dash, vm_value)

    if issues:
        print(f"MISMATCH: {issues} issue(s) found")
        return 1
    print("OK: Grafana and Perses dashboards are in parity")
    return 0


if __name__ == "__main__":
    sys.exit(main())
