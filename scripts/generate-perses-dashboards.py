#!/usr/bin/env python3
"""Perses dashboard sync for the metrics dashboards of this repo.

Two import modes:
  native (default) — builds Perses dashboards directly from the same Grafana
      dashboard definitions used by generate-grafana-dashboards.py
      (deterministic, full PromQL kept, ${vm:regex} -> $vm);
  migrate — feeds the generated Grafana dashboard JSON to Perses' native
      Grafana->Perses migrate endpoint (/api/migrate). Kept as a fallback: for
      dashboards using template variables the query migration degrades to
      "migration_from_grafana_not_supported".

A global default Prometheus datasource (HTTP proxy -> VictoriaMetrics) is
provisioned so dashboards resolve without datasource references.

Usage:
    python3 scripts/generate-perses-dashboards.py              # sync to http://localhost:8089
    python3 scripts/generate-perses-dashboards.py --check      # offline smoke (no network)
    python3 scripts/generate-perses-dashboards.py --dry-run
    python3 scripts/generate-perses-dashboards.py --mode migrate --dry-run
    python3 scripts/generate-perses-dashboards.py --url http://localhost:8089 --project l2

Environment variables (overridden by CLI flags):
    PERSES_URL          - Perses server URL (default: http://localhost:8089)
    PROMETHEUS_URL      - VictoriaMetrics/Prometheus URL for the datasource
                          (default: http://victoria-metrics:8428)
"""

import argparse
import importlib.util
import json
import os
import pathlib
import re
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

import requests

_SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
_GENERATOR_PATH = _SCRIPT_DIR / "generate-grafana-dashboards.py"
_generator_spec = importlib.util.spec_from_file_location(
    "generate_grafana_dashboards", _GENERATOR_PATH)
if _generator_spec is None or _generator_spec.loader is None:
    sys.exit(f"cannot load {_GENERATOR_PATH}")
gg = importlib.util.module_from_spec(_generator_spec)
_generator_spec.loader.exec_module(gg)

PERSES_URL = 'http://localhost:8089'
PERSES_PROJECT = 'l2'
PERSES_DATASOURCE = 'prometheus'
PROMETHEUS_URL = gg.PROMETHEUS_URL

DASHBOARDS: List[Tuple[Any, str]] = [
    (gg.create_tracing_dashboard, 'l2-distributed-tracing'),
    (gg.create_sentry_dashboard, 'l2-sentry-delivery'),
    (gg.create_proxy_dashboard, 'l2-proxy'),
    (gg.create_worker_dashboard, 'l2-worker'),
    (gg.create_server_dashboard, 'l2-server'),
    (gg.create_slo_dashboard, 'l2-slo-tracking'),
    (gg.create_nats_dashboard, 'nats-dashboard'),
    (gg.create_nginx_dashboard, 'nginx-metrics'),
]

_SAFE_UNITS = {'decimal', 'binary', 'percent', 'percent-decimal', 'bytes', 'time'}

_UNIT_MAP = {
    'short': 'decimal',
    'decbytes': 'bytes',
    'bytes/sec': 'bytes/s',
    'Bps': 'bytes/s',
    'percentunit': 'percent-decimal',
    'percent': 'percent',
    'bytes': 'bytes',
}

_CALC_MAP = {
    'lastNotNull': 'last-number',
    'last': 'last-number',
    'mean': 'mean',
    'max': 'max',
    'min': 'min',
    'sum': 'sum',
}


class PersesAPI:
    def __init__(self, url: str, timeout: float = 10.0, retries: int = 3) -> None:
        self.url = url.rstrip('/')
        self.timeout = timeout
        self.retries = retries

    def _request(self, method: str, path: str,
                 payload: Optional[Dict[str, Any]] = None) -> Tuple[int, Any]:
        last: Optional[Exception] = None
        for attempt in range(self.retries):
            try:
                resp = requests.request(method, f"{self.url}{path}", json=payload,
                                        timeout=self.timeout)
                if resp.status_code >= 500:
                    last = Exception(f"HTTP {resp.status_code} on {method} {path}")
                    time.sleep(2 ** attempt)
                    continue
                if resp.status_code == 204:
                    return resp.status_code, None
                try:
                    return resp.status_code, resp.json()
                except ValueError:
                    return resp.status_code, resp.text
            except requests.exceptions.RequestException as exc:
                last = exc
                time.sleep(2 ** attempt)
        print(f"Perses request failed after {self.retries} attempts: {method} {path}: {last}",
              file=sys.stderr)
        return -1, None

    def test_connection(self) -> bool:
        code, _ = self._request("GET", "/api/v1/globaldatasources")
        return code == 200

    def project_exists(self, name: str) -> bool:
        code, _ = self._request("GET", f"/api/v1/projects/{name}")
        return code == 200

    def create_project(self, name: str) -> bool:
        if self.project_exists(name):
            print(f"Project '{name}' already exists, skip")
            return True
        code, body = self._request("POST", "/api/v1/projects",
                                   {"kind": "Project", "metadata": {"name": name}})
        if code in (200, 201):
            print(f"Project '{name}' created")
            return True
        print(f"Failed to create project '{name}': HTTP {code} {body}", file=sys.stderr)
        return False

    def datasource_exists(self, name: str) -> bool:
        code, _ = self._request("GET", f"/api/v1/globaldatasources/{name}")
        return code == 200

    def create_global_datasource(self, name: str, url: str) -> bool:
        if self.datasource_exists(name):
            print(f"GlobalDatasource '{name}' already exists, skip")
            return True
        datasource = {
            "kind": "GlobalDatasource",
            "metadata": {"name": name},
            "spec": {
                "default": True,
                "plugin": {
                    "kind": "PrometheusDatasource",
                    "spec": {
                        "proxy": {
                            "kind": "HTTPProxy",
                            "spec": {"url": url},
                        }
                    },
                },
            },
        }
        code, body = self._request("POST", "/api/v1/globaldatasources", datasource)
        if code in (200, 201):
            print(f"GlobalDatasource '{name}' created ({url})")
            return True
        print(f"Failed to create GlobalDatasource '{name}': HTTP {code} {body}",
              file=sys.stderr)
        return False

    def migrate(self, source: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        code, resp = self._request("POST", "/api/migrate", {"grafanaDashboard": source})
        if code == 200 and isinstance(resp, dict):
            return resp
        if code in (400, 422):
            print(f"migrate rejected (HTTP {code}): {resp}", file=sys.stderr)
        else:
            print(f"migrate failed (HTTP {code}): {resp}", file=sys.stderr)
        return None

    def dashboard_exists(self, project: str, name: str) -> bool:
        code, _ = self._request("GET", f"/api/v1/projects/{project}/dashboards/{name}")
        return code == 200

    def save_dashboard(self, project: str, name: str,
                       dashboard: Dict[str, Any]) -> Tuple[bool, str, int]:
        dashboard["metadata"]["name"] = name
        dashboard["metadata"]["project"] = project
        path = f"/api/v1/projects/{project}/dashboards"
        if self.dashboard_exists(project, name):
            method, action = "PUT", "updated"
            path = f"{path}/{name}"
        else:
            method, action = "POST", "created"
        code, body = self._request(method, path, dashboard)
        if code in (200, 201):
            return True, action, code
        print(f"Failed to {action} dashboard '{name}': HTTP {code} {body}", file=sys.stderr)
        return False, action, code


def grafana_source(dash: Dict[str, Any]) -> Dict[str, Any]:
    if isinstance(dash, dict) and isinstance(dash.get("dashboard"), dict):
        return dash["dashboard"]
    return dash


def migrate_dashboard(api: PersesAPI, dash: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    source = grafana_source(dash)
    result = api.migrate(source)
    if result is None and source is not dash:
        result = api.migrate(dash)
    return result


def _rewrite_promql(expr: str) -> str:
    expr = re.sub(r'\$\{vm:regex\}', '$vm', expr)
    expr = re.sub(r'\$\{vm\}', '$vm', expr)
    return expr


def _format_unit(unit: Optional[str]) -> str:
    u = _UNIT_MAP.get(unit or '', unit or '')
    return u if u in _SAFE_UNITS else ''


def _calc(calcs: Optional[List[str]]) -> str:
    c = (calcs or ['lastNotNull'])[0]
    return _CALC_MAP.get(c, 'last-number')


def _duration(time_cfg: Optional[Dict[str, Any]]) -> str:
    fmt = (time_cfg or {}).get('from')
    if isinstance(fmt, str) and fmt.startswith('now-'):
        return fmt[4:]
    return '1h'


def _convert_variables(dash: Dict[str, Any]) -> List[Dict[str, Any]]:
    variables: List[Dict[str, Any]] = []
    for entry in (dash.get('templating') or {}).get('list') or []:
        if entry.get('type') != 'query':
            continue
        definition = entry.get('definition') or ''
        match = re.search(r'label_values\(\s*([^,]+)\s*,\s*([^)]+)\)', definition)
        if not match:
            continue
        metric = match.group(1).strip()
        label = match.group(2).strip()
        name = entry.get('name')
        if not name or not label:
            continue
        variables.append({
            "kind": "ListVariable",
            "spec": {
                "display": {
                    "name": entry.get('label') or name,
                    "description": entry.get('description') or '',
                    "hidden": False,
                },
                "allowAllValue": bool(entry.get('includeAll')),
                "allowMultiple": bool(entry.get('multi')),
                "sort": "alphabetical-asc",
                "plugin": {
                    "kind": "PrometheusLabelValuesVariable",
                    "spec": {"labelName": label, "matchers": [metric]},
                },
                "name": name,
            },
        })
    return variables


def _convert_queries(targets: Optional[List[Dict[str, Any]]]) -> List[Dict[str, Any]]:
    queries: List[Dict[str, Any]] = []
    for target in targets or []:
        expr = target.get('expr')
        if not expr:
            continue
        queries.append({
            "kind": "TimeSeriesQuery",
            "spec": {
                "plugin": {
                    "kind": "PrometheusTimeSeriesQuery",
                    "spec": {"query": _rewrite_promql(expr)},
                }
            },
        })
    return queries


def _convert_panel(panel: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    ptype = panel.get('type')
    title = panel.get('title', '')
    defaults = (panel.get('fieldConfig') or {}).get('defaults') or {}
    unit = _format_unit(defaults.get('unit'))
    calcs = ((panel.get('options') or {}).get('reduceOptions') or {}).get('calcs')
    if ptype in ('timeseries', 'graph'):
        y_axis = {}
        if unit:
            y_axis["format"] = {"unit": unit}
        plugin = {
            "kind": "TimeSeriesChart",
            "spec": {
                "legend": {"mode": "list", "position": "bottom"},
                "yAxis": y_axis,
            },
        }
    elif ptype == 'stat':
        spec: Dict[str, Any] = {"calculation": _calc(calcs)}
        if unit:
            spec["format"] = {"unit": unit}
        plugin = {"kind": "StatChart", "spec": spec}
    elif ptype == 'bargauge':
        # Плагина BargaugeChart в персесе нет; десятичная шкала баров = BarChart
        # (bar-gauges графаны отдают несколько серий, напр. по client_id).
        spec = {"calculation": _calc(calcs)}
        if unit:
            spec["format"] = {"unit": unit}
        plugin = {"kind": "BarChart", "spec": spec}
    else:
        return None
    queries = _convert_queries(panel.get('targets'))
    if not queries:
        return None
    return {
        "kind": "Panel",
        "spec": {"display": {"name": title}, "plugin": plugin, "queries": queries},
    }


def convert_dashboard_to_perses(grafana_json: Dict[str, Any]) -> Dict[str, Any]:
    dash = grafana_source(grafana_json)
    meta: Dict[str, Any] = {
        "name": dash.get('uid') or 'dashboard',
        "tags": dash.get('tags') or [],
    }
    spec: Dict[str, Any] = {
        "display": {
            "name": dash.get('title', ''),
            "description": dash.get('description') or '',
        },
        "duration": _duration(dash.get('time')),
    }
    if dash.get('refresh'):
        spec["refreshInterval"] = dash.get('refresh')
    variables = _convert_variables(dash)
    if variables:
        spec["variables"] = variables

    panels: Dict[str, Any] = {}
    layouts: List[Dict[str, Any]] = []
    current_items: List[Dict[str, Any]] = []
    current_title = dash.get('title', '')
    layout_idx = 0
    panel_idx = 0
    has_layout = False

    def flush_layout() -> None:
        nonlocal current_items, current_title
        if current_items:
            layout: Dict[str, Any] = {
                "kind": "Grid",
                "spec": {
                    "display": {"title": current_title},
                    "items": current_items,
                },
            }
            layouts.append(layout)
        current_items = []

    for panel in dash.get('panels') or []:
        if panel.get('type') == 'row':
            flush_layout()
            layout_idx += 1
            current_title = panel.get('title', '')
            panel_idx = 0
            has_layout = True
            continue
        converted = _convert_panel(panel)
        if converted is None:
            continue
        if not has_layout:
            current_title = dash.get('title', '')
            has_layout = True
        name = f"{layout_idx}_{panel_idx}"
        panels[name] = converted
        grid = panel.get('gridPos') or {}
        current_items.append({
            "x": grid.get('x', 0),
            "y": grid.get('y', 0),
            "width": grid.get('w', 12),
            "height": grid.get('h', 8),
            "content": {"$ref": f"#/spec/panels/{name}"},
        })
        panel_idx += 1
    flush_layout()

    spec["panels"] = panels
    spec["layouts"] = layouts
    return {"kind": "Dashboard", "metadata": meta, "spec": spec}


def _validate_perses_dashboard(dashboard: Dict[str, Any],
                               name: str) -> List[str]:
    errors: List[str] = []
    spec = dashboard.get('spec') or {}
    panels = spec.get('panels') or {}
    if not panels:
        errors.append(f"{name}: no panels converted")
        return errors
    if not spec.get('layouts'):
        errors.append(f"{name}: no layouts")
    refs = []
    for layout in spec.get('layouts') or []:
        for item in (layout.get('spec') or {}).get('items') or []:
            ref = (item.get('content') or {}).get('$ref')
            if ref:
                refs.append(ref)
    for ref in refs:
        key = ref.rsplit('/', 1)[-1]
        if key not in panels:
            errors.append(f"{name}: layout ref {ref} missing from panels")
    for pkey, panel in panels.items():
        for query in (panel.get('spec') or {}).get('queries') or []:
            expr = (query.get('spec') or {}).get('plugin', {}).get('spec', {}).get('query', '')
            if not expr or expr == 'migration_from_grafana_not_supported':
                errors.append(f"{name}: panel {pkey} has empty/placeholder query")
            if '${' in expr:
                errors.append(f"{name}: panel {pkey} unresolved variable {expr}")
    return errors


def offline_check() -> int:
    print(f"Offline smoke check: {len(DASHBOARDS)} dashboard definitions")
    ok = 0
    for func, name in DASHBOARDS:
        converted = convert_dashboard_to_perses(func())
        errors = _validate_perses_dashboard(converted, name)
        if errors:
            for error in errors:
                print(f"  FAIL {error}", file=sys.stderr)
            continue
        spec = converted['spec']
        print(f"  OK {name}: {len(spec['panels'])} panels, {len(spec['layouts'])} layouts, "
              f"{len(spec.get('variables') or [])} variables")
        ok += 1
    if ok != len(DASHBOARDS):
        return 1
    print(f"Offline check passed ({ok}/{len(DASHBOARDS)})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description='Perses dashboard sync (native conversion from generated Grafana dashboards)',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  python3 scripts/generate-perses-dashboards.py
  PERSES_URL=http://localhost:8089 python3 scripts/generate-perses-dashboards.py
  python3 scripts/generate-perses-dashboards.py --dry-run
  python3 scripts/generate-perses-dashboards.py --check
  python3 scripts/generate-perses-dashboards.py --mode migrate
  python3 scripts/generate-perses-dashboards.py --output-dir ./monitoring/perses/generated
        """,
    )
    parser.add_argument('--url', type=str, default=os.getenv('PERSES_URL', PERSES_URL),
                        help='Perses server URL (default: PERSES_URL env or %(default)s)')
    parser.add_argument('--project', type=str, default=PERSES_PROJECT,
                        help='Perses project name (default: %(default)s)')
    parser.add_argument('--mode', choices=['native', 'migrate'], default='native',
                        help='Build mode (default: %(default)s)')
    parser.add_argument('--datasource-name', type=str, default=PERSES_DATASOURCE,
                        help='Global datasource name (default: %(default)s)')
    parser.add_argument('--datasource-url', type=str,
                        default=os.getenv('PROMETHEUS_URL', PROMETHEUS_URL),
                        help='Prometheus/VictoriaMetrics URL for the datasource '
                             '(default: PROMETHEUS_URL env or %(default)s)')
    parser.add_argument('--timeout', type=float, default=10.0, help='HTTP timeout (s)')
    parser.add_argument('--retries', type=int, default=3, help='Retries per request')
    parser.add_argument('--dry-run', action='store_true', help='Validate only, do not write')
    parser.add_argument('--output-dir', type=str, default=None,
                        help='Write the generated Perses dashboards JSON to a directory')
    parser.add_argument('--check', action='store_true', help='Offline smoke check, no network')

    args = parser.parse_args()

    if args.check:
        return offline_check()

    api = PersesAPI(args.url, timeout=args.timeout, retries=args.retries)
    print(f"Perses URL: {api.url} (mode={args.mode})")

    if not api.test_connection():
        print(f"Cannot connect to Perses at {api.url}. Exiting.", file=sys.stderr)
        return 1

    if not api.create_project(args.project):
        return 1

    if not api.create_global_datasource(args.datasource_name, args.datasource_url):
        return 1

    out_dir = pathlib.Path(args.output_dir) if args.output_dir else None
    if out_dir:
        out_dir.mkdir(parents=True, exist_ok=True)

    ok_count = 0
    failures: List[str] = []
    for func, name in DASHBOARDS:
        print(f"Processing dashboard: {name}")
        grafana_dash = func()
        if args.mode == 'native':
            perses_dash = convert_dashboard_to_perses(grafana_dash)
        else:
            perses_dash = migrate_dashboard(api, grafana_dash)
        if perses_dash is None:
            failures.append(name)
            continue
        errors = _validate_perses_dashboard(perses_dash, name)
        if errors:
            for error in errors[:5]:
                print(f"  FAIL {error}", file=sys.stderr)
        if out_dir:
            (out_dir / f"{name}.json").write_text(
                json.dumps(perses_dash, indent=2, ensure_ascii=False), encoding="utf-8")
        if args.dry_run:
            exists = api.dashboard_exists(args.project, name)
            print(f"  DRY-RUN: would {'create' if not exists else 'update'} {name}")
            if not errors:
                ok_count += 1
            else:
                failures.append(name)
            continue
        ok, action, _ = api.save_dashboard(args.project, name, perses_dash)
        if ok:
            ok_count += 1
            print(f"  {action}: {name}")
        else:
            failures.append(name)

    print("=" * 60)
    total = len(DASHBOARDS)
    print(f"Perses dashboard sync complete: {ok_count}/{total} successful")
    if failures:
        print(f"Failures: {', '.join(failures)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
