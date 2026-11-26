#!/usr/bin/env python3
"""Shared helpers for the metric-consistency scripts.

Several scripts (metrics-consistency-check.py, metrics-golden-check.py,
alerts-consistency-check.py, generate-grafana-dashboards.py) parse the same
sources for Prometheus metric names and scrape the same live endpoints. The
cross-source parsing and exposition/scraping logic used to be copy-pasted
between them; this module centralises it so a change (e.g. a new histogram
suffix or registration-call shape) lands in one place.

Exposed:
  - constants: METRIC_LITERAL, CREATE_CALL, DYNAMIC_SERIES_MARK,
    HISTOGRAM_SUFFIXES, LAZY_PREFIXES, SERVICE_ENDPOINTS, SHARED_PREFIXES
  - normalize, load_module, collect_series_block, cpp_registered_metrics,
    parse_exposition, scrape_metrics, compare, split_lazy
"""

import importlib.util
import pathlib
import re
import urllib.error
import urllib.request

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC_DIR = REPO_ROOT / "src"

METRIC_LITERAL = r"(l2_[a-z0-9_]+)"
# The registration calls come in two shapes: the direct
# MetricsManager::create_* form and the MetricRegistrar short-hand
# (reg_<registry>.<kind>("name", ...)) introduced for the metric structs.
CREATE_CALL = re.compile(
    r"(?:MetricsManager::create_\w+\(\s*[^,]+,\s*|reg_\w+\.\w+\(\s*)\""
    + METRIC_LITERAL + r"\"", re.S)
DYNAMIC_SERIES_MARK = re.compile(r"DynamicLabeledFamily<[^>]*>::Series>")
HISTOGRAM_SUFFIXES = ("_bucket", "_count", "_sum")

# Families that are registered but only appear in /metrics once the labels are
# actually populated (per-client-id duplicate detectors, per-IP limiter counters
# behind a disabled-by-default flag, DB Gateway request families).
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
    "spider": ("http://localhost:19094/metrics", ("l2_spider_",)),
}

# The l2_common registry (tracing + sentry queues) is merged into every service's
# exposer, so these names legitimately appear on endpoints other than their
# "own" service.
SHARED_PREFIXES = ("l2_tracing_", "l2_worker_sentry_", "l2_common_")


def normalize(name: str) -> str:
    for suffix in HISTOGRAM_SUFFIXES:
        if name.endswith(suffix):
            return name[: -len(suffix)]
    return name


def load_module(path: pathlib.Path, name: str, failure_cls=RuntimeError):
    """Load a Python module from an arbitrary path (used to import the
    generator scripts that are not on sys.path)."""
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise failure_cls(f"cannot load {path}")
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
