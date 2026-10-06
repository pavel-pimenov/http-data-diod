#!/usr/bin/env python3
"""Env-var consistency gate: config/env reads in C++ vs docker-compose.yml.

Every environment variable the *production* C++ code reads (via Config::
get_env_*() helpers or a plain getenv() literal) must be wired in
docker-compose.yml (service environment list/mapping, build args, or a
``${VAR:-...}`` interpolation).  Test-only vars (TEST_*, FUZZ_VALUE in
test_*.cpp) and vendored trees (httplib, prometheus-cpp, base64, json, odpi)
are deliberately not scanned.

Run: python3 scripts/env-consistency-check.py
Exit: 0 when every C++-read var is present in docker-compose.yml, else 1.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = REPO_ROOT / "src"
COMPOSE_FILE = REPO_ROOT / "docker-compose.yml"

VENDORED_DIRS = ("httplib", "prometheus-cpp", "base64", "json", "odpi")

GET_ENV_RE = re.compile(r'get_env_[a-z_]*\(\s*"([A-Z0-9_]+)"')
GETENV_RE = re.compile(r'\bgetenv\s*\(\s*"([A-Z0-9_]+)"')

COMPOSE_ENV_LIST_RE = re.compile(r"^([ \t-]*)- ([A-Z][A-Z0-9_]*)\s*=")
COMPOSE_ENV_MAP_RE = re.compile(r"^([ \t]+)([A-Z][A-Z0-9_]*)\s*:")
COMPOSE_INTERP_RE = re.compile(r"\$\{([A-Z][A-Z0-9_]*)")


def cpp_source_files(root=None):
    if root is None:
        root = SRC_DIR
    for path in root.rglob("*"):
        if path.suffix not in (".cpp", ".cc", ".c", ".hpp", ".h"):
            continue
        rel = path.relative_to(root).as_posix()
        if any(part in rel for part in VENDORED_DIRS):
            continue
        if path.name.startswith("test_"):
            continue
        yield path


def cpp_env_vars(root=None):
    """All env var names read by production C++ sources, sorted."""
    found = set()
    for path in cpp_source_files(root):
        text = path.read_text(errors="ignore")
        found.update(m for m in GET_ENV_RE.findall(text))
        found.update(m for m in GETENV_RE.findall(text))
    return sorted(found)


def compose_env_vars(compose_text):
    """All env var names wired in a docker-compose.yml text, sorted.

    Keys are collected from ``environment:`` / ``args:`` blocks (list ``-
    KEY=...`` and mapping ``KEY: ...`` forms, the latter only while inside one
    of those blocks so plain YAML keys like ``image:`` are never picked up) plus
    every ``${KEY:...}`` interpolation in the file.
    """
    found = set()
    in_var_block = False
    block_indent = -1
    for raw in compose_text.splitlines():
        line = raw.rstrip()
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        block_match = re.match(r"^([ \t]+)(environment|args)\s*:$", line)
        if block_match:
            in_var_block = True
            block_indent = indent
            continue
        if in_var_block:
            if indent <= block_indent and not stripped.startswith("-"):
                in_var_block = False
            else:
                list_match = COMPOSE_ENV_LIST_RE.match(line)
                if list_match:
                    found.add(list_match.group(2))
                    continue
                map_match = COMPOSE_ENV_MAP_RE.match(line)
                if map_match:
                    found.add(map_match.group(2))
                    continue
        found.update(COMPOSE_INTERP_RE.findall(line))
    return sorted(found)


def check(cpp_vars, compose_vars):
    """Return (missing, orphans) where missing are C++ vars absent from compose."""
    cpp_set = set(cpp_vars)
    compose_set = set(compose_vars)
    missing = sorted(cpp_set - compose_set)
    orphans = sorted(compose_set - cpp_set)
    return missing, orphans


def main() -> int:
    cpp_vars = cpp_env_vars()
    compose_text = COMPOSE_FILE.read_text(errors="ignore")
    compose_vars = compose_env_vars(compose_text)
    missing, orphans = check(cpp_vars, compose_vars)

    print(f"env-consistency: {len(cpp_vars)} C++ config vars, "
          f"{len(compose_vars)} compose vars")
    if missing:
        print("FAIL: C++ reads env vars missing from docker-compose.yml:")
        for name in missing:
            print(f"  - {name}  (add it to the service environment block)")
        return 1
    print(f"OK: every C++ env var is wired in docker-compose.yml "
          f"({len(cpp_vars)}/{len(cpp_vars)})")
    if orphans:
        print(f"note: {len(orphans)} compose vars are not read by C++ "
              f"(runtime/scripts only): {', '.join(orphans)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())