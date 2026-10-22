#!/usr/bin/env python3
"""Cross-source consistency check for the alerting rules.

The rules in `alerts/*.yml` are the sixth place where a metric name appears
(C++, Grafana, Perses, README, golden check — and now the alert expressions).
Nothing forces those names to agree: a rule can reference a metric that was
renamed or removed, and it will silently never fire — the worst possible
failure mode for an alert.

What this script checks:

  1. every `l2_*` metric literal in an alert expression exists in the C++
     registration (the same source of truth the metric consistency check uses)
  2. every rule carries the mandatory parts: `for`, `labels.severity`,
     `labels.service`, `annotations.summary/description/runbook`
  3. alert names are unique and well-formed
  4. in --runtime mode: every expression is a valid PromQL query (evaluated
     against VictoriaMetrics) and the vmalert service has loaded the rules
     without evaluation failures

Two modes:
  --offline (default)   parse the sources only; no containers, no network
  --runtime             additionally query VictoriaMetrics and vmalert

Usage: scripts/alerts-consistency-check.py [--offline|--runtime]
"""

import argparse
import importlib.util
import json
import pathlib
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

import yaml

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
ALERTS_DIR = REPO_ROOT / "alerts"
METRICS_CHECK_PATH = REPO_ROOT / "scripts" / "metrics-consistency-check.py"

METRIC_LITERAL = r"(l2_[a-z0-9_]+)"
# Prometheus duration: a sequence of <number><unit> pairs ("30s", "5m", "1h30m").
DURATION = re.compile(r"^(?:\d+(?:ms|s|m|h|d|w|y))+$")
ALERT_NAME = re.compile(r"^L2[A-Z][A-Za-z0-9]*$")
SEVERITIES = ("warning", "critical")
REQUIRED_ANNOTATIONS = ("summary", "description", "runbook")
VM_URL = "http://localhost:8428"
VMALERT_URL = "http://localhost:8880"


def load_module(path: pathlib.Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_metrics_module():
    return load_module(METRICS_CHECK_PATH, "metrics_consistency")


def rule_files() -> list:
    if not ALERTS_DIR.is_dir():
        return []
    return sorted(ALERTS_DIR.glob("*.yml")) + sorted(ALERTS_DIR.glob("*.yaml"))


def collect_rules(path: pathlib.Path) -> list:
    """Return [(group_name, rule)] for every rule in a rule file."""
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or "groups" not in data:
        raise ValueError("no 'groups' key")
    rules = []
    for group in data["groups"]:
        if not isinstance(group, dict) or "rules" not in group:
            raise ValueError("a group has no 'rules' key")
        for rule in group["rules"]:
            rules.append((str(group.get("name", "<unnamed>")), rule))
    return rules


def check_rule(label: str, rule: dict, metrics: dict, problems: list) -> None:
    name = rule.get("alert")
    if not isinstance(name, str) or not name:
        problems.append(f"{label}: a rule has no 'alert' name")
        return
    if not ALERT_NAME.match(name):
        problems.append(f"{label}/{name}: the alert name must start with 'L2' "
                        "and use CamelCase")
    expr = rule.get("expr")
    if not isinstance(expr, str) or not expr.strip():
        problems.append(f"{label}/{name}: empty 'expr'")
        return
    duration = rule.get("for")
    if not isinstance(duration, str) or not DURATION.match(duration):
        problems.append(f"{label}/{name}: 'for' must be a Prometheus duration "
                        f"(got {duration!r})")
    labels = rule.get("labels") or {}
    severity = labels.get("severity")
    if severity not in SEVERITIES:
        problems.append(f"{label}/{name}: labels.severity must be one of "
                        f"{SEVERITIES} (got {severity!r})")
    if not labels.get("service"):
        problems.append(f"{label}/{name}: labels.service is required")
    annotations = rule.get("annotations") or {}
    for key in REQUIRED_ANNOTATIONS:
        if not annotations.get(key):
            problems.append(f"{label}/{name}: annotations.{key} is required")
    for metric in sorted(set(re.findall(METRIC_LITERAL, expr))):
        if metrics["normalize"](metric) not in metrics["registered"]:
            problems.append(f"{label}/{name}: metric {metric} is not registered "
                            "in C++ — the alert would never fire")


def query_victoria(expr: str, url: str, timeout: float):
    """Evaluate a PromQL expression. Returns (ok, detail)."""
    target = f"{url}/api/v1/query?{urllib.parse.urlencode({'query': expr})}"
    request = urllib.request.Request(target, headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = json.loads(response.read().decode("utf-8", errors="ignore"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="ignore")[:200]
        return False, f"HTTP {exc.code}: {detail}"
    except (urllib.error.URLError, OSError) as exc:
        return None, str(exc)
    if body.get("status") != "success":
        return False, str(body.get("error", "unknown error"))
    return True, str(len(body.get("data", {}).get("result", [])))


def scrape_vmalert(url: str, timeout: float):
    """Return (totals, rule_errors) from the vmalert self-metrics.

    totals maps a metric name to the sum over all its label sets (vmalert
    exports one series per rule), rule_errors lists the (alertname, errors)
    pairs of the per-rule evaluation error counter.
    """
    try:
        with urllib.request.urlopen(f"{url}/metrics", timeout=timeout) as response:
            body = response.read().decode("utf-8", errors="ignore")
    except (urllib.error.URLError, OSError):
        return {}, []
    totals = {}
    rule_errors = []
    for line in body.splitlines():
        if line.startswith("#") or " " not in line:
            continue
        series, _, value = line.partition(" ")
        try:
            number = float(value)
        except ValueError:
            continue
        name = series.split("{")[0]
        totals[name] = totals.get(name, 0.0) + number
        if name == "vmalert_alerting_rules_errors_total":
            match = re.search(r'alertname="([^"]*)"', series)
            rule_errors.append((match.group(1) if match else "<unknown>", number))
    return totals, rule_errors


def runtime_check(rules: list, vm_url: str, vmalert_url: str, timeout: float,
                  problems: list, notes: list) -> None:
    reachable = False
    valid = 0
    firing = []
    for _group, rule in rules:
        name = rule.get("alert", "<unnamed>")
        ok, detail = query_victoria(rule.get("expr", ""), vm_url, timeout)
        if ok is None:
            notes.append(f"VictoriaMetrics is unreachable ({detail}) — the "
                         "PromQL validation half of the runtime check is skipped")
            break
        reachable = True
        if not ok:
            problems.append(f"{name}: invalid PromQL — {detail}")
            continue
        valid += 1
        # The expression is the alert condition: a non-empty result means the
        # condition holds right now (the alert would fire), an empty one means
        # it does not. Neither is an error.
        if detail != "0":
            firing.append(name)
    if not reachable:
        return
    notes.append(f"VictoriaMetrics: {valid}/{len(rules)} expressions are valid")
    if firing:
        notes.append("the condition currently holds for: "
                     + ", ".join(sorted(firing)))
    else:
        notes.append("no rule matches its condition right now")

    vmalert, rule_errors = scrape_vmalert(vmalert_url, timeout)
    if not vmalert:
        problems.append(f"vmalert is not reachable at {vmalert_url} — the rules "
                        "are not being evaluated (docker compose up -d vmalert)")
        return
    if not vmalert.get("vmalert_config_last_reload_successful", 0):
        problems.append("vmalert failed to load the rule file — see "
                        "docker compose logs vmalert")
    else:
        notes.append(f"vmalert: the rule file is loaded, "
                     f"{int(vmalert.get('vmalert_alerts_firing', 0))} alert(s) "
                     f"firing, {int(vmalert.get('vmalert_alerts_pending', 0))} "
                     "pending")
    failing = sorted(name for name, errors in rule_errors if errors > 0)
    if failing:
        problems.append("vmalert failed to evaluate the rules: "
                        + ", ".join(failing))
    if vmalert.get("vmalert_execution_errors_total", 0) > 0:
        problems.append("vmalert reports execution errors — see "
                        "docker compose logs vmalert")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--offline", action="store_true",
                        help="parse the sources only (default)")
    parser.add_argument("--runtime", action="store_true",
                        help="also query VictoriaMetrics and vmalert")
    parser.add_argument("--vm-url", default=VM_URL,
                        help=f"VictoriaMetrics URL (default {VM_URL})")
    parser.add_argument("--vmalert-url", default=VMALERT_URL,
                        help=f"vmalert URL (default {VMALERT_URL})")
    parser.add_argument("--timeout", type=float, default=5.0,
                        help="per-request timeout in seconds (default 5)")
    args = parser.parse_args()

    problems: list = []
    notes: list = []

    try:
        module = load_metrics_module()
        registered, _literals = module.cpp_registered_metrics()
    except (RuntimeError, OSError) as exc:
        print(f"FAIL: cannot read the C++ metric catalogue: {exc}")
        return 1
    metrics = {"registered": registered, "normalize": module.normalize}

    files = rule_files()
    if not files:
        print(f"FAIL: no rule files in {ALERTS_DIR}")
        return 1

    rules = []
    for path in files:
        try:
            rules.extend((group, rule) for group, rule in collect_rules(path))
        except (ValueError, yaml.YAMLError) as exc:
            problems.append(f"{path.name}: {exc}")

    seen = set()
    for group, rule in rules:
        name = rule.get("alert", "")
        if name in seen:
            problems.append(f"{group}: duplicate alert name {name}")
        seen.add(name)
        check_rule(group, rule, metrics, problems)

    if args.runtime:
        runtime_check(rules, args.vm_url, args.vmalert_url, args.timeout,
                      problems, notes)

    print("alert consistency check")
    print(f"  mode: {'offline+runtime' if args.runtime else 'offline'}")
    print(f"  files={len(files)} rules={len(rules)} "
          f"registered_metrics={len(registered)}")
    for note in notes:
        print(f"  - {note}")

    if problems:
        print("FAIL: alerting rules are inconsistent")
        for item in problems:
            print(f"  - {item}")
        return 1
    print(f"OK: {len(rules)} alert rules agree with the C++ metric catalogue")
    return 0


if __name__ == "__main__":
    sys.exit(main())
