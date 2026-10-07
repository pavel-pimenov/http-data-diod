#!/usr/bin/env python3
"""Cross-source consistency check for Prometheus metric names.

Four sources describe the same metric catalogue and drift apart silently:

  1. C++ registration — `MetricsManager::create_*` calls and
     `DynamicLabeledFamily<...>::Series` literals in src/*.cpp|hpp
     (test_*.cpp excluded). This is the runtime truth.
  2. Grafana dashboards — PromQL in scripts/generate-grafana-dashboards.py
  3. README catalogue — the "Метрики Prometheus (полный каталог)" tables
  4. Golden check — CATALOG + CONDITIONAL in scripts/metrics-golden-check.py

Adding a metric without touching the other sources used to go unnoticed until
a dashboard was eyeballed, which is exactly how the round-57 proxy micro-metrics
ended up exported but invisible. This script fails on any asymmetric difference.

Two modes:
  --offline (default)   parse sources only; no containers, no network
  --runtime             additionally scrape each service's /metrics endpoint, check
                        that each service exports what it registers (and nothing
                        foreign), and that every dashboard metric is exported by
                        somebody

Runtime mode is deliberately tolerant: families whose labels only materialise
after traffic (per-client-id, per-IP rate limiter, DB Gateway) are reported as
`lazy` rather than `missing`, and the reverse direction (exported but never
registered) is reported separately since that indicates dead registrations.
A stack that is not running at all is reported as a single actionable error
("run ./rebuild-and-run.sh") instead of three "cannot scrape" lines.
"""

import argparse
import importlib.util
import pathlib
import re
import sys
import urllib.error
import urllib.request

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC_DIR = REPO_ROOT / "src"
GENERATOR_PATH = REPO_ROOT / "scripts" / "generate-grafana-dashboards.py"
GOLDEN_PATH = REPO_ROOT / "scripts" / "metrics-golden-check.py"
README_PATH = REPO_ROOT / "README.md"

METRIC_LITERAL = r"(l2_[a-z0-9_]+)"
CREATE_CALL = re.compile(
    r"MetricsManager::create_\w+\(\s*[^,]+,\s*\"" + METRIC_LITERAL + r"\"", re.S)
DYNAMIC_SERIES_MARK = re.compile(r"DynamicLabeledFamily<[^>]*>::Series>")
HISTOGRAM_SUFFIXES = ("_bucket", "_count", "_sum")

# Families that are registered but only appear in /metrics once the labels are
# actually populated (per-client-id duplicate detectors, per-IP limiter counters
# behind a disabled-by-default flag, DB Gateway request families). Kept in sync
# with metrics-golden-check.py CONDITIONAL by the cross-source check itself.
LAZY_PREFIXES = (
    "l2_proxy_per_client_id_",
    "l2_proxy_per_ip_",
    "l2_proxy_db_",
    "l2_worker_db_",
)

# /metrics endpoints published by docker-compose.yml for each role: the proxy
# exposes 19090, the worker 19091, l2-server 19092. 19093 is the worker's
# /health/ready port, not a metrics endpoint.
SERVICE_ENDPOINTS = {
    "proxy": ("http://localhost:19090/metrics", ("l2_proxy_", "l2_http_pool_",
                                                 "l2_rate_limiter_",
                                                 "l2_per_ip_rate_limiter_")),
    "worker": ("http://localhost:19091/metrics", ("l2_worker_",)),
    "server": ("http://localhost:19092/metrics", ("l2_server_",)),
}

# The l2_common registry (tracing + sentry queues) is merged into every service's
# exposer, so these names legitimately appear on endpoints other than their
# "own" service.
SHARED_PREFIXES = ("l2_tracing_", "l2_worker_sentry_", "l2_common_")


class CheckFailure(Exception):
    pass


def normalize(name: str) -> str:
    for suffix in HISTOGRAM_SUFFIXES:
        if name.endswith(suffix):
            return name[: -len(suffix)]
    return name


def load_module(path: pathlib.Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise CheckFailure(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def collect_series_block(text: str, start: int, window: int = 4000) -> str:
    """Return the brace-balanced block that starts at/after `start`."""
    begin = text.find("{", start)
    if begin < 0:
        return ""
    depth = 0
    for idx in range(begin, min(len(text), begin + window)):
        char = text[idx]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[begin:idx + 1]
    return ""


def cpp_registered_metrics(src_dir: pathlib.Path = None) -> tuple:
    """Metric names registered in C++ plus every l2_ literal found (the latter
    exposes non-registration literals such as Sentry fingerprints)."""
    src = src_dir if src_dir is not None else SRC_DIR
    registered = set()
    literals = set()
    for path in sorted(src.glob("*.cpp")) + sorted(src.glob("*.hpp")):
        if path.name.startswith("test_"):
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        literals |= set(re.findall(r"\"" + METRIC_LITERAL + r"\"", text))
        registered |= set(CREATE_CALL.findall(text))
        for match in DYNAMIC_SERIES_MARK.finditer(text):
            block = collect_series_block(text, match.end())
            registered |= set(re.findall(r"\"" + METRIC_LITERAL + r"\"", block))
    return registered, literals


def dashboard_metrics(generator) -> set:
    factories = [name for name in dir(generator)
                 if name.startswith("create_") and name.endswith("_dashboard")
                 and callable(getattr(generator, name))]
    metrics = set()
    for name in sorted(factories):
        dashboard = getattr(generator, name)()
        metrics |= generator._collect_dashboard_metrics(dashboard)
    return metrics


def readme_metrics(readme_path: pathlib.Path = None) -> set:
    path = readme_path if readme_path is not None else README_PATH
    metrics = set()
    for line in path.read_text(encoding="utf-8",
                               errors="ignore").splitlines():
        if line.startswith("|") and "`l2_" in line:
            metrics |= set(re.findall(r"`" + METRIC_LITERAL + r"`", line))
    return metrics


def golden_metrics(golden) -> set:
    return set(golden.CATALOG) | set(golden.CONDITIONAL)


def parse_exposition(body: str) -> set:
    """Family names from a Prometheus text exposition. Lines look like
    `name{labels} value`, `name_bucket{le="1"} value` or `name value`; the
    histogram suffixes are stripped so all three collapse to one family."""
    names = set()
    for line in body.splitlines():
        if not line or line.startswith("#"):
            continue
        name = line.split("{", 1)[0].split(" ", 1)[0].strip()
        if re.fullmatch(METRIC_LITERAL, name):
            names.add(normalize(name))
    return names


def scrape_metrics(url: str, timeout: float) -> set:
    """Family names exported by a live /metrics endpoint."""
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return parse_exposition(response.read().decode("utf-8", errors="ignore"))


def compare(label_a: str, set_a: set, label_b: str, set_b: set,
            problems: list, notes: list) -> None:
    only_a = sorted(set_a - set_b)
    only_b = sorted(set_b - set_a)
    if only_a:
        problems.append(f"in {label_a} but not in {label_b}: "
                        + ", ".join(only_a))
    if only_b:
        problems.append(f"in {label_b} but not in {label_a}: "
                        + ", ".join(only_b))
    if not only_a and not only_b:
        notes.append(f"{label_a} == {label_b} ({len(set_a)} metrics)")


def split_lazy(names, problems: list, notes: list, label: str,
               prefix: str) -> None:
    """Report `names` as `lazy` when their labels are traffic-dependent,
    otherwise as a hard problem."""
    lazy = sorted(name for name in names if name.startswith(LAZY_PREFIXES))
    hard = sorted(name for name in names if not name.startswith(LAZY_PREFIXES))
    if hard:
        problems.append(f"{label}: " + ", ".join(hard))
    if lazy:
        notes.append(f"{prefix}lazy (label-dependent, not yet in /metrics): "
                     + ", ".join(lazy))


def runtime_check(registered: set, dashboards: set, timeout: float,
                  problems: list, notes: list, endpoints: dict = None,
                  scraper=None) -> None:
    """Check both directions against live /metrics: every service exports what
    it registers (plus shared l2_common families), and every dashboard metric is
    exported by at least one service."""
    endpoints = endpoints if endpoints is not None else SERVICE_ENDPOINTS
    scraper = scraper if scraper is not None else scrape_metrics

    exported_by_role = {}
    unreachable = []
    for role, (url, _) in endpoints.items():
        try:
            exported_by_role[role] = scraper(url, timeout)
        except (urllib.error.URLError, OSError) as exc:
            unreachable.append(f"{role} ({url}: {exc})")

    if unreachable and len(unreachable) == len(endpoints):
        problems.append("no /metrics endpoint is reachable, so the runtime half "
                        "of the check cannot run — start the stack first "
                        "(./rebuild-and-run.sh) or use --offline: "
                        + "; ".join(unreachable))
        return
    for item in unreachable:
        problems.append(f"cannot scrape {item}")

    for role, (_, prefixes) in endpoints.items():
        exported = exported_by_role.get(role)
        if not exported:
            continue
        if not exported:
            problems.append(f"{role}: exposed no l2_* metrics")
            continue
        expected = {name for name in registered
                    if name.startswith(tuple(prefixes) + SHARED_PREFIXES)}
        foreign = sorted(exported - expected)
        if foreign:
            notes.append(f"{role}: exported but registered elsewhere: "
                         + ", ".join(foreign))
        missing = expected - exported
        if missing:
            split_lazy(missing, problems, notes,
                       f"{role}: registered but not exported",
                       f"{role}: ")
        else:
            notes.append(f"{role}: /metrics matches registration "
                         f"({len(exported)} metrics)")

    everywhere = set()
    for exported in exported_by_role.values():
        everywhere |= exported
    notes.append(f"dashboards: every metric is exported by some service "
                 f"({len(dashboards & everywhere)}/{len(dashboards)})")
    split_lazy(dashboards - everywhere, problems, notes,
               "dashboards: metric never exported by any service", "dashboards: ")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--offline", action="store_true",
                        help="parse sources only (default; implied when --runtime is absent)")
    parser.add_argument("--runtime", action="store_true",
                        help="also scrape each service's /metrics endpoint")
    parser.add_argument("--timeout", type=float, default=5.0,
                        help="per-endpoint scrape timeout in seconds (default 5)")
    args = parser.parse_args()

    problems: list = []
    notes: list = []

    try:
        registered, literals = cpp_registered_metrics()
        generator = load_module(GENERATOR_PATH, "grafana_generator")
        dashboards = dashboard_metrics(generator)
        golden = load_module(GOLDEN_PATH, "metrics_golden")
        catalogue = golden_metrics(golden)
        readme = readme_metrics()
    except CheckFailure as exc:
        print(f"FAIL: {exc}")
        return 1

    unregistered = sorted(literals - registered)
    if unregistered:
        notes.append("l2_ literals that are not metric registrations "
                     "(expected: Sentry fingerprints): "
                     + ", ".join(unregistered))

    compare("C++ registration", registered, "dashboards", dashboards,
            problems, notes)
    compare("C++ registration", registered, "README catalogue", readme,
            problems, notes)
    compare("C++ registration", registered, "golden check", catalogue,
            problems, notes)

    if args.runtime:
        runtime_check(registered, dashboards, args.timeout, problems, notes)

    print("metric consistency check")
    print(f"  mode: {'offline+runtime' if args.runtime else 'offline'}")
    print(f"  registered={len(registered)} dashboards={len(dashboards)} "
          f"readme={len(readme)} golden={len(catalogue)}")
    for note in notes:
        print(f"  - {note}")

    if problems:
        print("FAIL: metric sources disagree")
        for item in problems:
            print(f"  - {item}")
        return 1
    print("OK: metric names agree across C++, dashboards, README and golden check")
    return 0


if __name__ == "__main__":
    sys.exit(main())
